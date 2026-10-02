#include "WorldRender.h"

#include "Config.h"
#include "Log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace discraft::WorldRender
{
	namespace
	{
		struct Vertex
		{
			float    x, y, z;
			D3DCOLOR color;
			float    u, v;
		};
		constexpr DWORD kFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

		struct LineVertex
		{
			float    x, y, z;
			D3DCOLOR color;
		};
		constexpr DWORD kLineFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE;

		struct Section
		{
			IDirect3DVertexBuffer9* vb{ nullptr };
			UINT                    capacity{ 0 };
			UINT                    opaque{ 0 }, cutout{ 0 }, translucent{ 0 };  // vertices, in that order in the buffer
			int                     sx{ 0 }, sy{ 0 }, sz{ 0 };
		};

		struct Texture
		{
			IDirect3DTexture9* tex{ nullptr };
			UINT               w{ 0 }, h{ 0 };
		};

		struct Batch
		{
			std::uint32_t texture;
			std::uint32_t first;
			std::uint32_t count;
			std::uint32_t flags;
		};

		std::unordered_map<std::uint64_t, Section>  sections;
		Texture                                     atlas;
		std::unordered_map<std::uint32_t, Texture>  textures;
		std::vector<Vertex>                         sceneVerts;
		std::vector<Batch>                          sceneBatches;
		double                                      sceneOrigin[3]{};
		IDirect3DSurface9*                          ownDepth = nullptr;
		UINT                                        ownDepthW = 0, ownDepthH = 0;
		float                                       brightness = 1.0f;
		float                                       nearClipUu = 10.0f;
		int                                         fovMode = 0;
		float                                       rollSign = 1.0f;
		bool                                        configured = false;
		std::array<float, 16>                       lightCurve{};

		// Direction ordinal + 1 -> Minecraft's fixed face shading (down, up, north, south, west, east).
		constexpr float kFaceShade[7] = { 1.0f, 0.5f, 1.0f, 0.8f, 0.8f, 0.6f, 0.6f };

		void Configure()
		{
			configured = true;
			brightness = config::Float("Render", "fBlockBrightness", 1.0f);
			nearClipUu = config::Float("Render", "fNearClipUU", 10.0f);
			fovMode = config::Int("Render", "iFovMode", 0);
			rollSign = config::Bool("Render", "bInvertRoll", false) ? -1.0f : 1.0f;
			for (int l = 0; l < 16; ++l) {
				// Minecraft's light curve, brightened a little (like its "Bright" setting).
				const float v = l / 15.0f;
				const float curve = v / (4.0f - 3.0f * v);
				lightCurve[static_cast<std::size_t>(l)] = std::pow(curve, 0.55f);
			}
		}

		std::uint64_t Key(int a_x, int a_y, int a_z)
		{
			return (std::uint64_t(std::uint32_t(a_x) & 0x1FFFFF) << 42) | (std::uint64_t(std::uint32_t(a_y) & 0x1FFFFF) << 21) |
			       std::uint64_t(std::uint32_t(a_z) & 0x1FFFFF);
		}

		template <class T>
		void Release(T*& a_p)
		{
			if (a_p) {
				a_p->Release();
				a_p = nullptr;
			}
		}

		std::uint32_t Swizzle(std::uint32_t a_rgba) { return (a_rgba & 0xFF00FF00u) | ((a_rgba >> 16) & 0xFFu) | ((a_rgba & 0xFFu) << 16); }

		bool Upload(IDirect3DDevice9* a_device, Texture& a_tex, UINT a_w, UINT a_h, const std::uint8_t* a_rgba, const RECT* a_rect = nullptr)
		{
			if (!a_rect && (!a_tex.tex || a_tex.w != a_w || a_tex.h != a_h)) {
				Release(a_tex.tex);
				if (FAILED(a_device->CreateTexture(a_w, a_h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &a_tex.tex, nullptr))) {
					DC_ERROR("render: texture %ux%u creation failed", a_w, a_h);
					return false;
				}
				a_tex.w = a_w;
				a_tex.h = a_h;
			}
			if (!a_tex.tex) {
				return false;
			}
			D3DLOCKED_RECT locked{};
			if (FAILED(a_tex.tex->LockRect(0, &locked, a_rect, 0))) {
				return false;
			}
			const UINT rows = a_rect ? static_cast<UINT>(a_rect->bottom - a_rect->top) : a_h;
			const UINT cols = a_rect ? static_cast<UINT>(a_rect->right - a_rect->left) : a_w;
			for (UINT y = 0; y < rows; ++y) {
				auto*       dst = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(locked.pBits) + std::size_t(y) * locked.Pitch);
				const auto* src = reinterpret_cast<const std::uint32_t*>(a_rgba) + std::size_t(y) * cols;
				for (UINT x = 0; x < cols; ++x) {
					dst[x] = Swizzle(src[x]);
				}
			}
			a_tex.tex->UnlockRect(0);
			return true;
		}

		D3DCOLOR Shade(std::uint32_t a_rgba, std::uint32_t a_light, std::uint32_t a_flags)
		{
			const float block = lightCurve[a_light & 0xF];
			const float sky = lightCurve[(a_light >> 8) & 0xF];
			const float face = kFaceShade[(a_flags >> 4) & 7];
			const float k = std::max(std::max(block, sky), 0.05f) * face * brightness;
			const auto  channel = [&](int a_shift) { return static_cast<DWORD>(std::clamp(((a_rgba >> a_shift) & 0xFF) * k, 0.0f, 255.0f)); };
			return D3DCOLOR_ARGB((a_rgba >> 24) & 0xFF, channel(0), channel(8), channel(16));
		}

		Vertex Convert(const proto::RenVertex& a_v)
		{
			return { a_v.x, a_v.y, a_v.z, Shade(a_v.color, a_v.light, a_v.flags), a_v.u, a_v.v };
		}

		void SetSection(IDirect3DDevice9* a_device, const proto::RenSection& a_header, const proto::RenVertex* a_verts)
		{
			const auto key = Key(a_header.sx, a_header.sy, a_header.sz);
			if (a_header.vertexCount == 0) {
				if (auto it = sections.find(key); it != sections.end()) {
					Release(it->second.vb);
					sections.erase(it);
				}
				return;
			}
			// Opaque, then cutout, then translucent triangles (by their first vertex's flags).
			std::vector<Vertex> ordered;
			ordered.reserve(a_header.vertexCount);
			UINT counts[3]{};
			for (int pass = 0; pass < 3; ++pass) {
				for (UINT t = 0; t + 2 < a_header.vertexCount; t += 3) {
					const std::uint32_t f = a_verts[t].flags;
					const int           kind = (f & 2) ? 2 : (f & 1) ? 1 : 0;
					if (kind != pass) {
						continue;
					}
					for (UINT k = 0; k < 3; ++k) {
						ordered.push_back(Convert(a_verts[t + k]));
					}
					counts[pass] += 3;
				}
			}
			auto& s = sections[key];
			s.sx = a_header.sx;
			s.sy = a_header.sy;
			s.sz = a_header.sz;
			const UINT bytes = static_cast<UINT>(ordered.size() * sizeof(Vertex));
			if (!s.vb || s.capacity < bytes) {
				Release(s.vb);
				s.capacity = std::max<UINT>(bytes, 4096);
				if (FAILED(a_device->CreateVertexBuffer(s.capacity, D3DUSAGE_WRITEONLY, kFvf, D3DPOOL_MANAGED, &s.vb, nullptr))) {
					DC_ERROR("render: vertex buffer of %u bytes failed", s.capacity);
					sections.erase(key);
					return;
				}
			}
			void* dst = nullptr;
			if (bytes && SUCCEEDED(s.vb->Lock(0, bytes, &dst, 0))) {
				std::memcpy(dst, ordered.data(), bytes);
				s.vb->Unlock();
			}
			s.opaque = counts[0];
			s.cutout = counts[1];
			s.translucent = counts[2];
		}

		// Blocks mined out of Dishonored's world, per section (bit x + 16z + 256y).
		std::map<std::tuple<int, int, int>, std::array<std::uint8_t, 512>> dug;
		bool stencilLogged = false;

		void Handle(IDirect3DDevice9* a_device, std::uint32_t a_type, const std::uint8_t* a_data, std::uint32_t a_bytes)
		{
			switch (a_type) {
			case proto::kRenAtlas:
				{
					if (a_bytes < sizeof(proto::RenAtlas)) {
						return;
					}
					proto::RenAtlas h;
					std::memcpy(&h, a_data, sizeof(h));
					if (a_bytes >= sizeof(h) + std::size_t(h.width) * h.height * 4) {
						if (Upload(a_device, atlas, h.width, h.height, a_data + sizeof(h))) {
							DC_INFO("render: block atlas %ux%u", h.width, h.height);
						}
					} else if (h.width && h.height && h.width <= 16384 && h.height <= 16384) {
						// Size only: the pixels follow as atlas regions (strips).
						Release(atlas.tex);
						atlas.w = atlas.h = 0;
						if (SUCCEEDED(a_device->CreateTexture(h.width, h.height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &atlas.tex, nullptr))) {
							atlas.w = h.width;
							atlas.h = h.height;
							DC_INFO("render: block atlas %ux%u (arriving in strips)", h.width, h.height);
						} else {
							DC_ERROR("render: atlas texture %ux%u creation failed", h.width, h.height);
						}
					}
					break;
				}
			case proto::kRenAtlasRegion:
				{
					if (a_bytes < sizeof(proto::RenAtlasRegion) || !atlas.tex) {
						return;
					}
					proto::RenAtlasRegion r;
					std::memcpy(&r, a_data, sizeof(r));
					if (r.x + r.width > atlas.w || r.y + r.height > atlas.h || a_bytes < sizeof(r) + std::size_t(r.width) * r.height * 4) {
						return;
					}
					const RECT rect{ static_cast<LONG>(r.x), static_cast<LONG>(r.y), static_cast<LONG>(r.x + r.width), static_cast<LONG>(r.y + r.height) };
					Upload(a_device, atlas, atlas.w, atlas.h, a_data + sizeof(r), &rect);
					break;
				}
			case proto::kRenSection:
				{
					if (a_bytes < sizeof(proto::RenSection)) {
						return;
					}
					proto::RenSection h;
					std::memcpy(&h, a_data, sizeof(h));
					if (a_bytes < sizeof(h) + std::size_t(h.vertexCount) * sizeof(proto::RenVertex)) {
						return;
					}
					std::vector<proto::RenVertex> verts(h.vertexCount);
					std::memcpy(verts.data(), a_data + sizeof(h), verts.size() * sizeof(proto::RenVertex));
					SetSection(a_device, h, verts.data());
					break;
				}
			case proto::kRenClearAll:
				Clear();
				dug.clear();
				break;
			case proto::kRenDug:
				{
					if (a_bytes < sizeof(proto::RenDug)) {
						return;
					}
					proto::RenDug h;
					std::memcpy(&h, a_data, sizeof(h));
					const auto key = std::make_tuple(h.sx, h.sy, h.sz);
					if (!h.count || a_bytes < sizeof(h) + 512) {
						dug.erase(key);
						return;
					}
					std::memcpy(dug[key].data(), a_data + sizeof(h), 512);
					static int dugLogged = 0;
					if (dugLogged++ < 10) {
						DC_INFO("render: Minecraft dug %u block(s) out of section (%d, %d, %d)", h.count, h.sx, h.sy, h.sz);
					}
					break;
				}
			case proto::kRenTexture:
				{
					if (a_bytes < sizeof(proto::RenTexture)) {
						return;
					}
					proto::RenTexture h;
					std::memcpy(&h, a_data, sizeof(h));
					if (a_bytes >= sizeof(h) + std::size_t(h.width) * h.height * 4) {
						Upload(a_device, textures[h.id], h.width, h.height, a_data + sizeof(h));
					}
					break;
				}
			case proto::kRenScene:
				{
					if (a_bytes < sizeof(proto::RenScene)) {
						return;
					}
					proto::RenScene h;
					std::memcpy(&h, a_data, sizeof(h));
					const std::size_t need = sizeof(h) + std::size_t(h.batchCount) * sizeof(proto::RenBatch) + std::size_t(h.vertexCount) * sizeof(proto::RenVertex);
					if (a_bytes < need) {
						return;
					}
					sceneOrigin[0] = h.originX;
					sceneOrigin[1] = h.originY;
					sceneOrigin[2] = h.originZ;
					sceneBatches.resize(h.batchCount);
					std::memcpy(sceneBatches.data(), a_data + sizeof(h), sceneBatches.size() * sizeof(Batch));
					sceneVerts.resize(h.vertexCount);
					const auto* src = reinterpret_cast<const proto::RenVertex*>(a_data + sizeof(h) + sceneBatches.size() * sizeof(Batch));
					for (std::size_t i = 0; i < sceneVerts.size(); ++i) {
						proto::RenVertex v;
						std::memcpy(&v, src + i, sizeof(v));
						sceneVerts[i] = Convert(v);
					}
					break;
				}
			default:
				// Avatar (third person isn't drawn in Dishonored), ragdoll, lights, NPC solids, dug
				// blocks: not used on this side.
				break;
			}
		}

		// ---- matrices (Direct3D: row vectors, left-handed camera space x right, y up, z forward) ----
		struct V3
		{
			double x, y, z;
		};
		V3 Cross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
		V3 Norm(const V3& a)
		{
			const double l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
			return l > 1e-12 ? V3{ a.x / l, a.y / l, a.z / l } : V3{ 0, 0, 1 };
		}

		struct CameraBasis
		{
			V3 right, up, forward;
		};

		CameraBasis Basis(const CameraView& a_view)
		{
			const auto f = McLookVector(a_view.yaw, a_view.pitch);
			const double yaw = a_view.yaw * 0.017453292519943295;
			V3        forward{ f.x, f.y, f.z };
			V3        right{ -std::cos(yaw), 0.0, -std::sin(yaw) };
			V3        up = Norm(Cross(right, forward));
			const double roll = a_view.roll * 0.017453292519943295 * rollSign;
			if (std::fabs(roll) > 1e-6) {
				const double c = std::cos(roll), s = std::sin(roll);
				const V3     r2{ right.x * c + up.x * s, right.y * c + up.y * s, right.z * c + up.z * s };
				const V3     u2{ up.x * c - right.x * s, up.y * c - right.y * s, up.z * c - right.z * s };
				right = r2;
				up = u2;
			}
			return { right, up, forward };
		}

		D3DMATRIX Identity()
		{
			D3DMATRIX m{};
			m._11 = m._22 = m._33 = m._44 = 1.0f;
			return m;
		}

		D3DMATRIX Translation(double a_x, double a_y, double a_z)
		{
			D3DMATRIX m = Identity();
			m._41 = static_cast<float>(a_x);
			m._42 = static_cast<float>(a_y);
			m._43 = static_cast<float>(a_z);
			return m;
		}

		D3DMATRIX ViewMatrix(const CameraBasis& a_b)
		{
			D3DMATRIX m = Identity();
			m._11 = static_cast<float>(a_b.right.x);
			m._21 = static_cast<float>(a_b.right.y);
			m._31 = static_cast<float>(a_b.right.z);
			m._12 = static_cast<float>(a_b.up.x);
			m._22 = static_cast<float>(a_b.up.y);
			m._32 = static_cast<float>(a_b.up.z);
			m._13 = static_cast<float>(a_b.forward.x);
			m._23 = static_cast<float>(a_b.forward.y);
			m._33 = static_cast<float>(a_b.forward.z);
			return m;
		}

		// Infinite far plane, like Unreal Engine 3's own projection.
		D3DMATRIX Projection(float a_fovDeg, float a_aspect, float a_near, bool a_reversed)
		{
			const float t = std::tan(a_fovDeg * 0.5f * 0.017453292f);
			float       xs = 1.0f / t, ys = xs * a_aspect;
			if (fovMode == 1) {  // the FOV is vertical
				ys = 1.0f / t;
				xs = ys / a_aspect;
			}
			D3DMATRIX m{};
			m._11 = xs;
			m._22 = ys;
			m._34 = 1.0f;
			if (a_reversed) {
				m._33 = 0.0f;
				m._43 = a_near;
			} else {
				m._33 = 1.0f;
				m._43 = -a_near;
			}
			return m;
		}

		IDirect3DSurface9* OwnDepth(IDirect3DDevice9* a_device, UINT a_w, UINT a_h, D3DMULTISAMPLE_TYPE a_ms)
		{
			if (ownDepth && (ownDepthW != a_w || ownDepthH != a_h)) {
				Release(ownDepth);
			}
			if (!ownDepth) {
				if (FAILED(a_device->CreateDepthStencilSurface(a_w, a_h, D3DFMT_D24S8, a_ms, 0, TRUE, &ownDepth, nullptr))) {
					return nullptr;
				}
				ownDepthW = a_w;
				ownDepthH = a_h;
			}
			return ownDepth;
		}

		void Quad(std::vector<Vertex>& a_out, const V3& a_c, const V3& a_ax, const V3& a_ay, const float a_uv[4], D3DCOLOR a_color)
		{
			const auto at = [&](double sx, double sy) {
				return V3{ a_c.x + a_ax.x * sx + a_ay.x * sy, a_c.y + a_ax.y * sx + a_ay.y * sy, a_c.z + a_ax.z * sx + a_ay.z * sy };
			};
			const V3 p00 = at(-1, -1), p10 = at(1, -1), p11 = at(1, 1), p01 = at(-1, 1);
			const auto v = [&](const V3& p, float u, float w) { return Vertex{ float(p.x), float(p.y), float(p.z), a_color, u, w }; };
			a_out.push_back(v(p00, a_uv[0], a_uv[3]));
			a_out.push_back(v(p10, a_uv[2], a_uv[3]));
			a_out.push_back(v(p11, a_uv[2], a_uv[1]));
			a_out.push_back(v(p00, a_uv[0], a_uv[3]));
			a_out.push_back(v(p11, a_uv[2], a_uv[1]));
			a_out.push_back(v(p01, a_uv[0], a_uv[1]));
		}

		// Dropped items, arrows and dropped blocks (camera-relative), plus the block outline.
		void DrawWorldEntities(IDirect3DDevice9* a_device, const CameraView& a_view, const CameraBasis& a_b)
		{
			static proto::WorldEntities we;
			if (!Link::Get().ReadWorldEntities(we)) {
				return;
			}
			std::vector<Vertex> verts;
			for (std::uint32_t i = 0; i < we.count; ++i) {
				const auto& e = we.entities[i];
				const float scale = e.scale > 0.0f ? e.scale : 0.25f;
				const V3    c{ e.x - a_view.x, e.y + scale * 0.5 - a_view.y, e.z - a_view.z };
				const D3DCOLOR white = D3DCOLOR_ARGB(255, 230, 230, 230);
				if (e.kind == proto::kWeItem || e.kind == proto::kWeArrow || e.kind == proto::kWeTrident) {
					const double h = scale * 0.5;
					Quad(verts, c, { a_b.right.x * h, a_b.right.y * h, a_b.right.z * h }, { a_b.up.x * h, a_b.up.y * h, a_b.up.z * h }, e.uv[0], white);
				} else if (e.kind == proto::kWeBlock) {
					const double h = scale * 0.5, yaw = e.yaw * 0.017453292519943295;
					const V3     ax{ std::cos(yaw) * h, 0, std::sin(yaw) * h }, az{ -std::sin(yaw) * h, 0, std::cos(yaw) * h }, ay{ 0, h, 0 };
					const auto   off = [&](const V3& d) { return V3{ c.x + d.x, c.y + d.y, c.z + d.z }; };
					const auto   neg = [](const V3& d) { return V3{ -d.x, -d.y, -d.z }; };
					Quad(verts, off(ay), ax, az, e.uv[1], white);         // top
					Quad(verts, off(neg(ay)), ax, az, e.uv[2], white);    // bottom
					Quad(verts, off(ax), az, ay, e.uv[0], white);
					Quad(verts, off(neg(ax)), az, ay, e.uv[0], white);
					Quad(verts, off(az), ax, ay, e.uv[0], white);
					Quad(verts, off(neg(az)), ax, ay, e.uv[0], white);
				}
			}
			const D3DMATRIX identity = Identity();
			if (!verts.empty() && atlas.tex) {
				a_device->SetTransform(D3DTS_WORLD, &identity);
				a_device->SetTexture(0, atlas.tex);
				a_device->SetFVF(kFvf);
				a_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(verts.size() / 3), verts.data(), sizeof(Vertex));
			}
			if (we.hasSelection) {
				const float g = 0.002f;
				const float x0 = we.selMin[0] - g - float(a_view.x), y0 = we.selMin[1] - g - float(a_view.y), z0 = we.selMin[2] - g - float(a_view.z);
				const float x1 = we.selMax[0] + g - float(a_view.x), y1 = we.selMax[1] + g - float(a_view.y), z1 = we.selMax[2] + g - float(a_view.z);
				const D3DCOLOR black = D3DCOLOR_ARGB(160, 0, 0, 0);
				const float    p[8][3] = { { x0, y0, z0 }, { x1, y0, z0 }, { x1, y0, z1 }, { x0, y0, z1 }, { x0, y1, z0 }, { x1, y1, z0 }, { x1, y1, z1 },
                    { x0, y1, z1 } };
				const int      edges[12][2] = { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 },
                    { 3, 7 } };
				LineVertex     lines[24];
				for (int k = 0; k < 12; ++k) {
					for (int e = 0; e < 2; ++e) {
						const auto& q = p[edges[k][e]];
						lines[k * 2 + e] = { q[0], q[1], q[2], black };
					}
				}
				a_device->SetTexture(0, nullptr);
				a_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
				a_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
				a_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
				a_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
				a_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
				a_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
				a_device->SetFVF(kLineFvf);
				a_device->SetTransform(D3DTS_WORLD, &identity);
				a_device->DrawPrimitiveUP(D3DPT_LINELIST, 12, lines, sizeof(LineVertex));
			}
		}
	}

	void Drain(IDirect3DDevice9* a_device)
	{
		if (!configured) {
			Configure();
		}
		Link::Get().DrainRender([&](std::uint32_t a_type, const std::uint8_t* a_data, std::uint32_t a_bytes) { Handle(a_device, a_type, a_data, a_bytes); },
			16ull << 20);
	}

	namespace
	{
		// Dug cells near the camera as cubes (camera-relative), slightly grown so Minecraft's faces on
		// their sides draw in front of the cut.
		void DugCubes(const CameraView& a_view, std::vector<Vertex>& a_out)
		{
			constexpr int    kRange = 48;
			constexpr float  e = 0.004f;
			constexpr D3DCOLOR kHole = D3DCOLOR_XRGB(24, 20, 18);
			static const int faces[6][4] = { { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 } };
			for (const auto& [key, bits] : dug) {
				const auto [sx, sy, sz] = key;
				const double cx = sx * 16.0 + 8.0 - a_view.x, cy = sy * 16.0 + 8.0 - a_view.y, cz = sz * 16.0 + 8.0 - a_view.z;
				if (std::fabs(cx) > kRange + 8 || std::fabs(cy) > kRange + 8 || std::fabs(cz) > kRange + 8) {
					continue;
				}
				for (int i = 0; i < 4096; ++i) {
					if (!(bits[i >> 3] & (1u << (i & 7)))) {
						continue;
					}
					const float x0 = float(sx * 16 + (i & 15) - a_view.x) - e, y0 = float(sy * 16 + (i >> 8) - a_view.y) - e,
								z0 = float(sz * 16 + ((i >> 4) & 15) - a_view.z) - e;
					const float x1 = x0 + 1.0f + 2 * e, y1 = y0 + 1.0f + 2 * e, z1 = z0 + 1.0f + 2 * e;
					Vertex c[8];
					for (int k = 0; k < 8; ++k) {
						c[k] = { (k & 4) ? x1 : x0, (k & 2) ? y1 : y0, (k & 1) ? z1 : z0, kHole, 0.0f, 0.0f };
					}
					for (const auto& f : faces) {
						a_out.push_back(c[f[0]]);
						a_out.push_back(c[f[1]]);
						a_out.push_back(c[f[2]]);
						a_out.push_back(c[f[0]]);
						a_out.push_back(c[f[2]]);
						a_out.push_back(c[f[3]]);
					}
				}
			}
		}

		bool HasStencil(D3DFORMAT a_f) { return a_f == D3DFMT_D24S8 || a_f == D3DFMT_D24FS8 || a_f == D3DFMT_D24X4S4 || a_f == D3DFMT_D15S1; }

		// Cuts the dug cells out of the game's picture: where the game's surface lies inside a dug cube
		// (counted like a shadow volume in the stencil), its depth is pushed back to the cube's far
		// side and painted dark; Minecraft's blocks around the hole then draw over it as its walls.
		void PunchHoles(IDirect3DDevice9* a_device, const CameraView& a_view, bool a_reversed, bool a_stencil)
		{
			std::vector<Vertex> cubes;
			DugCubes(a_view, cubes);
			if (cubes.empty()) {
				return;
			}
			const D3DMATRIX identity = Translation(0, 0, 0);
			a_device->SetTransform(D3DTS_WORLD, &identity);
			a_device->SetTexture(0, nullptr);
			a_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
			a_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
			a_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
			if (a_stencil) {
				a_device->Clear(0, nullptr, D3DCLEAR_STENCIL, 0, 1.0f, 0);
			}
			// 1: count faces in front of the game's surface: front faces +1, back faces -1 (either sign
			// of winding works, only "not zero" is tested).
			if (a_stencil) {
			a_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
			a_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
			a_device->SetRenderState(D3DRS_ZFUNC, a_reversed ? D3DCMP_GREATEREQUAL : D3DCMP_LESSEQUAL);
			a_device->SetRenderState(D3DRS_STENCILENABLE, TRUE);
			a_device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, TRUE);
			a_device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
			a_device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_INCR);
			a_device->SetRenderState(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
			a_device->SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
			a_device->SetRenderState(D3DRS_CCW_STENCILFUNC, D3DCMP_ALWAYS);
			a_device->SetRenderState(D3DRS_CCW_STENCILPASS, D3DSTENCILOP_DECR);
			a_device->SetRenderState(D3DRS_CCW_STENCILFAIL, D3DSTENCILOP_KEEP);
			a_device->SetRenderState(D3DRS_CCW_STENCILZFAIL, D3DSTENCILOP_KEEP);
			a_device->SetRenderState(D3DRS_STENCILMASK, 0xFF);
			a_device->SetRenderState(D3DRS_STENCILWRITEMASK, 0xFF);
			a_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(cubes.size() / 3), cubes.data(), sizeof(Vertex));
			}
			// 2: inside a cube: take its far side's depth, and paint the hole. (No stencil: every pixel
			// where the game's surface is nearer than the cube's far side, walls in front included.)
			a_device->SetRenderState(D3DRS_STENCILENABLE, a_stencil);
			a_device->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
			a_device->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_NOTEQUAL);
			a_device->SetRenderState(D3DRS_STENCILREF, 0);
			a_device->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
			a_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
			a_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
			a_device->SetRenderState(D3DRS_ZFUNC, a_reversed ? D3DCMP_LESSEQUAL : D3DCMP_GREATEREQUAL);
			a_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(cubes.size() / 3), cubes.data(), sizeof(Vertex));
			// Back to drawing blocks.
			a_device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
			a_device->SetRenderState(D3DRS_ZFUNC, a_reversed ? D3DCMP_GREATEREQUAL : D3DCMP_LESSEQUAL);
			a_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
			a_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
			a_device->SetTexture(0, atlas.tex);
		}
	}

	void Draw(IDirect3DDevice9* a_device, const CameraView& a_view, int a_width, int a_height, int a_depthMode)
	{
		if (!a_view.valid || !atlas.tex || a_width <= 0 || a_height <= 0) {
			return;
		}
		const double upb = State().unitsPerBlock;
		const float  nearPlane = static_cast<float>(nearClipUu / upb);

		// Depth: the game's own buffer if it fits the back buffer (blocks then hide behind its walls),
		// else one of our own (blocks only hide behind each other).
		IDirect3DSurface9* backBuffer = nullptr;
		IDirect3DSurface9* gameDepth = nullptr;
		a_device->GetRenderTarget(0, &backBuffer);
		a_device->GetDepthStencilSurface(&gameDepth);
		D3DSURFACE_DESC bbDesc{}, dsDesc{};
		if (backBuffer) {
			backBuffer->GetDesc(&bbDesc);
		}
		bool useGame = a_depthMode != 0 && gameDepth;
		if (useGame) {
			gameDepth->GetDesc(&dsDesc);
			useGame = dsDesc.Width >= bbDesc.Width && dsDesc.Height >= bbDesc.Height && dsDesc.MultiSampleType == bbDesc.MultiSampleType;
		}
		IDirect3DSurface9* depth = useGame ? gameDepth : OwnDepth(a_device, bbDesc.Width, bbDesc.Height, bbDesc.MultiSampleType);
		const bool         reversed = useGame && a_depthMode == 2;
		if (depth) {
			a_device->SetDepthStencilSurface(depth);
			if (!useGame) {
				a_device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
			}
		}

		const auto basis = Basis(a_view);
		const auto view = ViewMatrix(basis);
		const auto proj = Projection(a_view.fov, float(a_width) / float(a_height), nearPlane, reversed);
		a_device->SetTransform(D3DTS_VIEW, &view);
		a_device->SetTransform(D3DTS_PROJECTION, &proj);

		a_device->SetVertexShader(nullptr);
		a_device->SetPixelShader(nullptr);
		a_device->SetRenderState(D3DRS_ZENABLE, depth ? D3DZB_TRUE : D3DZB_FALSE);
		a_device->SetRenderState(D3DRS_ZFUNC, reversed ? D3DCMP_GREATEREQUAL : D3DCMP_LESSEQUAL);
		a_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		a_device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		a_device->SetRenderState(D3DRS_LIGHTING, FALSE);
		a_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
		a_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		a_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		a_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		a_device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
		a_device->SetRenderState(D3DRS_ALPHAREF, 0x80);
		a_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		a_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		a_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
		a_device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
		a_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		a_device->SetRenderState(D3DRS_CLIPPING, TRUE);
		a_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		a_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		a_device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		a_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		a_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		a_device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
		a_device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
		a_device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		a_device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		a_device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		a_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		a_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		a_device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		a_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		a_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		a_device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
		a_device->SetFVF(kFvf);
		a_device->SetTexture(0, atlas.tex);

		if (useGame && !dug.empty()) {
			const bool stencil = HasStencil(dsDesc.Format);
			if (!stencilLogged) {
				stencilLogged = true;
				DC_INFO("render: cutting %zu dug section(s) into the game's picture (depth format %u, %s)", dug.size(), static_cast<unsigned>(dsDesc.Format),
					stencil ? "stencil" : "no stencil: holes also show through walls in front of them");
			}
			PunchHoles(a_device, a_view, reversed, stencil);
		}

		const auto sectionMatrix = [&](const Section& s) {
			return Translation(s.sx * 16.0 - a_view.x, s.sy * 16.0 - a_view.y, s.sz * 16.0 - a_view.z);
		};
		// Opaque, then cutout (alpha-tested) blocks.
		for (int pass = 0; pass < 2; ++pass) {
			a_device->SetRenderState(D3DRS_ALPHATESTENABLE, pass == 1);
			for (const auto& [key, s] : sections) {
				const UINT first = pass == 0 ? 0 : s.opaque;
				const UINT count = pass == 0 ? s.opaque : s.cutout;
				if (!count || !s.vb) {
					continue;
				}
				const auto world = sectionMatrix(s);
				a_device->SetTransform(D3DTS_WORLD, &world);
				a_device->SetStreamSource(0, s.vb, 0, sizeof(Vertex));
				a_device->DrawPrimitive(D3DPT_TRIANGLELIST, first, count / 3);
			}
		}
		// Every other entity and the particles (alpha-tested; translucent batches blended).
		if (!sceneBatches.empty()) {
			const auto world = Translation(sceneOrigin[0] - a_view.x, sceneOrigin[1] - a_view.y, sceneOrigin[2] - a_view.z);
			a_device->SetTransform(D3DTS_WORLD, &world);
			for (const auto& b : sceneBatches) {
				if (b.first + b.count > sceneVerts.size() || b.count < 3) {
					continue;
				}
				IDirect3DTexture9* tex = atlas.tex;
				if (b.texture) {
					const auto it = textures.find(b.texture);
					tex = it != textures.end() ? it->second.tex : nullptr;
				}
				if (!tex) {
					continue;
				}
				const bool translucent = (b.flags & 1) != 0;
				a_device->SetRenderState(D3DRS_ALPHABLENDENABLE, translucent);
				a_device->SetRenderState(D3DRS_ZWRITEENABLE, !translucent);
				a_device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
				a_device->SetRenderState(D3DRS_ALPHAREF, translucent ? 0x04 : 0x80);
				a_device->SetTexture(0, tex);
				a_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, b.count / 3, sceneVerts.data() + b.first, sizeof(Vertex));
			}
			a_device->SetRenderState(D3DRS_ALPHAREF, 0x80);
			a_device->SetTexture(0, atlas.tex);
		}
		a_device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
		a_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		a_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		DrawWorldEntities(a_device, a_view, basis);

		// Translucent blocks (water, glass, ice), far to near, without writing depth.
		std::vector<std::pair<double, const Section*>> order;
		for (const auto& [key, s] : sections) {
			if (s.translucent && s.vb) {
				const double dx = s.sx * 16.0 + 8.0 - a_view.x, dy = s.sy * 16.0 + 8.0 - a_view.y, dz = s.sz * 16.0 + 8.0 - a_view.z;
				order.emplace_back(dx * dx + dy * dy + dz * dz, &s);
			}
		}
		if (!order.empty()) {
			std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			a_device->SetTexture(0, atlas.tex);
			a_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
			a_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			a_device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			a_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
			a_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			a_device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
			a_device->SetFVF(kFvf);
			a_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
			a_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
			a_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
			for (const auto& [d, s] : order) {
				const auto world = sectionMatrix(*s);
				a_device->SetTransform(D3DTS_WORLD, &world);
				a_device->SetStreamSource(0, s->vb, 0, sizeof(Vertex));
				a_device->DrawPrimitive(D3DPT_TRIANGLELIST, s->opaque + s->cutout, s->translucent / 3);
			}
		}
		a_device->SetStreamSource(0, nullptr, 0, 0);
		if (backBuffer) {
			backBuffer->Release();
		}
		if (gameDepth) {
			gameDepth->Release();
		}
	}

	void OnLostDevice() { Release(ownDepth); }

	void Clear()
	{
		for (auto& [key, s] : sections) {
			Release(s.vb);
		}
		sections.clear();
		sceneBatches.clear();
		sceneVerts.clear();
	}
}
