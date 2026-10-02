#include "Game.h"

#include "Bindings.h"

#include "Config.h"
#include "Log.h"
#include "Mem.h"
#include "Mesher.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Dishonored's world shape read straight from its static meshes, the way SkyCraft reads Skyrim's:
// Trace and FastTrace don't see this game's floors, but every StaticMesh keeps its collision
// triangles (a kDOP tree) in memory. Layout found from the probe (StaticMesh, 32-bit):
//   +0x38 LODModels   TIndirectArray<FStaticMeshRenderData> (data, num, max)
//   +0x70 kDOP nodes  TArray, +0x7C kDOP triangles TArray<{u16 v1, v2, v3, material}>
//   +0x8C Bounds      origin xyz, extent xyz, radius (mesh space)
// The vertices the triangles index are the render data's position buffer, found by looking for an
// array of points that fits the mesh's bounds and is long enough for every index (checked per mesh).
// Each component places its mesh with LocalToWorld (UE3 row-vector matrix, rows at +0, +16, +32,
// origin at +48).
namespace discraft::MeshWorld
{
	namespace
	{
		using ue3::Obj;

		constexpr int kOffLodModels = 0x38;
		constexpr int kOffKdopTris = 0x7C;
		constexpr int kOffBounds = 0x8C;

		struct Tri
		{
			float x[3], y[3], z[3];  // Minecraft space
			float minX, maxX, minZ, maxZ;
		};

		// Where the position array was found the last time (tried first for the next mesh).
		struct Path
		{
			int  outer{ -1 };     // offset in the render data
			bool viaPointer{ false };
			int  inner{ 0 };      // offset in the object that pointer leads to
			int  stride{ 12 };
		};
		Path lastPath;

		std::vector<Tri>                                 tris;
		std::unordered_map<long long, std::vector<int>> buckets;  // 1-block xz cells -> tris
		Obj        smcClass = 0;
		ue3::Field fMesh, fLocalToWorld, fBounds;
		bool       enabled = true;
		int        generation = 0;
		bool       breakPending = false;
		bool       ready = false;
		ULONGLONG  builtAt = 0;
		Vec3d      builtFor{ 1e9, 1e9, 1e9 };
		int        meshesUsed = 0, meshesSkipped = 0;

		long long BucketKey(int a_x, int a_z) { return (static_cast<long long>(a_x) << 32) ^ static_cast<unsigned>(a_z); }

		template <class T>
		bool Rd(std::uintptr_t a_addr, T& a_out)
		{
			return a_addr > 0x10000 && mem::TryRead(a_addr, a_out);
		}

		struct Points
		{
			std::uintptr_t data{ 0 };
			int            count{ 0 };
			int            stride{ 12 };
		};

		bool FitsBounds(std::uintptr_t a_data, int a_count, int a_stride, const float a_lo[3], const float a_hi[3])
		{
			const int n = std::min(a_count, 128);
			int       inside = 0;
			for (int i = 0; i < n; ++i) {
				float p[3];
				for (int c = 0; c < 3; ++c) {
					if (!Rd(a_data + static_cast<std::uintptr_t>(i) * a_stride + c * 4, p[c]) || !std::isfinite(p[c])) {
						return false;
					}
				}
				bool in = true;
				for (int c = 0; c < 3; ++c) {
					in = in && p[c] >= a_lo[c] && p[c] <= a_hi[c];
				}
				inside += in ? 1 : 0;
			}
			// All points inside, and not all the same (a zero-filled array fits small bounds too).
			if (inside != n || n < 3) {
				return false;
			}
			float a[3], b[3];
			for (int c = 0; c < 3; ++c) {
				Rd(a_data + c * 4, a[c]);
				Rd(a_data + static_cast<std::uintptr_t>(n - 1) * a_stride + c * 4, b[c]);
			}
			return a[0] != b[0] || a[1] != b[1] || a[2] != b[2];
		}

		// (data, num, max) at a_at, looking like a TArray of a_minCount or more.
		bool ArrayAt(std::uintptr_t a_at, int a_minCount, std::uintptr_t& a_data, int& a_count)
		{
			std::uint32_t d = 0;
			std::int32_t  n = 0, m = 0;
			if (!Rd(a_at, d) || !Rd(a_at + 4, n) || !Rd(a_at + 8, m)) {
				return false;
			}
			if (d < 0x10000 || n < a_minCount || n > 2000000 || m < n || m > 4000000) {
				return false;
			}
			a_data = d;
			a_count = n;
			return true;
		}

		bool TryPath(std::uintptr_t a_render, const Path& a_p, int a_minCount, const float a_lo[3], const float a_hi[3], Points& a_out)
		{
			std::uintptr_t base = a_render + static_cast<std::uintptr_t>(a_p.outer);
			if (a_p.viaPointer) {
				std::uint32_t q = 0;
				if (!Rd(base, q)) {
					return false;
				}
				base = q + static_cast<std::uintptr_t>(a_p.inner);
			}
			std::uintptr_t data = 0;
			int            count = 0;
			if (!ArrayAt(base, a_minCount, data, count) || !FitsBounds(data, count, a_p.stride, a_lo, a_hi)) {
				return false;
			}
			a_out = { data, count, a_p.stride };
			return true;
		}

		bool FindPoints(std::uintptr_t a_render, int a_minCount, const float a_lo[3], const float a_hi[3], Points& a_out)
		{
			if (lastPath.outer >= 0 && TryPath(a_render, lastPath, a_minCount, a_lo, a_hi, a_out)) {
				return true;
			}
			for (const int stride : { 12, 16, 20, 24, 28, 32 }) {
				for (int outer = 0; outer < 0x300; outer += 4) {
					Path p{ outer, false, 0, stride };
					if (TryPath(a_render, p, a_minCount, a_lo, a_hi, a_out)) {
						lastPath = p;
						DC_INFO("mesh: vertex positions at render data +0x%X (array), stride %d", outer, stride);
						return true;
					}
					for (int inner = 0; inner < 0x30; inner += 4) {
						p = { outer, true, inner, stride };
						if (TryPath(a_render, p, a_minCount, a_lo, a_hi, a_out)) {
							lastPath = p;
							DC_INFO("mesh: vertex positions at render data +0x%X -> +0x%X, stride %d", outer, inner, stride);
							return true;
						}
					}
				}
			}
			return false;
		}

		bool ReadBytes(std::uintptr_t a_addr, std::size_t a_bytes, void* a_dst)
		{
			if (a_addr < 0x10000 || !mem::Readable(a_addr, a_bytes)) {
				return false;
			}
			std::memcpy(a_dst, reinterpret_cast<const void*>(a_addr), a_bytes);
			return true;
		}

		// A mesh's collision triangles in its own space, decoded once and shared by every component
		// that uses it.
		struct LocalMesh
		{
			std::vector<float>         pos;  // xyz per vertex
			std::vector<std::uint16_t> idx;  // 3 per triangle
			bool                       ok{ false };
		};
		std::unordered_map<Obj, LocalMesh> meshes;

		const LocalMesh& Decode(Obj a_mesh)
		{
			auto [it, fresh] = meshes.try_emplace(a_mesh);
			LocalMesh& lm = it->second;
			if (!fresh) {
				return lm;
			}
			std::uint32_t triData = 0;
			std::int32_t  triCount = 0;
			if (!Rd(a_mesh + kOffKdopTris, triData) || !Rd(a_mesh + kOffKdopTris + 4, triCount) || triCount <= 0 || triCount > 200000) {
				return lm;
			}
			float bounds[7];
			if (!ReadBytes(a_mesh + kOffBounds, sizeof(bounds), bounds)) {
				return lm;
			}
			for (const float b : bounds) {
				if (!std::isfinite(b)) {
					return lm;
				}
			}
			float lo[3], hi[3];
			for (int c = 0; c < 3; ++c) {
				const float pad = std::abs(bounds[3 + c]) * 0.02f + 1.0f;
				lo[c] = bounds[c] - std::abs(bounds[3 + c]) - pad;
				hi[c] = bounds[c] + std::abs(bounds[3 + c]) + pad;
			}
			std::vector<std::uint16_t> raw(static_cast<std::size_t>(triCount) * 4);
			if (!ReadBytes(triData, raw.size() * 2, raw.data())) {
				return lm;
			}
			int maxIndex = 0;
			for (int t = 0; t < triCount; ++t) {
				maxIndex = std::max({ maxIndex, int(raw[t * 4]), int(raw[t * 4 + 1]), int(raw[t * 4 + 2]) });
			}
			std::uint32_t lods = 0, render = 0;
			std::int32_t  lodCount = 0;
			if (!Rd(a_mesh + kOffLodModels, lods) || !Rd(a_mesh + kOffLodModels + 4, lodCount) || lodCount <= 0) {
				return lm;
			}
			Points pts;
			const bool found = (Rd(lods, render) && FindPoints(render, maxIndex + 1, lo, hi, pts)) || FindPoints(lods, maxIndex + 1, lo, hi, pts);
			if (!found) {
				if (meshesSkipped++ < 5) {
					DC_INFO("mesh: no vertex positions found for %s (%d triangles, max index %d)", ue3::PathOf(a_mesh).c_str(), triCount, maxIndex);
				}
				return lm;
			}
			std::vector<std::uint8_t> verts(static_cast<std::size_t>(maxIndex + 1) * pts.stride);
			if (!ReadBytes(pts.data, verts.size(), verts.data())) {
				return lm;
			}
			lm.pos.resize(static_cast<std::size_t>(maxIndex + 1) * 3);
			for (int i = 0; i <= maxIndex; ++i) {
				std::memcpy(&lm.pos[static_cast<std::size_t>(i) * 3], verts.data() + static_cast<std::size_t>(i) * pts.stride, 12);
			}
			lm.idx.resize(static_cast<std::size_t>(triCount) * 3);
			for (int t = 0; t < triCount; ++t) {
				lm.idx[t * 3] = raw[t * 4];
				lm.idx[t * 3 + 1] = raw[t * 4 + 1];
				lm.idx[t * 3 + 2] = raw[t * 4 + 2];
			}
			lm.ok = true;
			return lm;
		}

		// The structure being built (swapped in when complete, so collision never sees half of it).
		struct Building
		{
			std::vector<Tri>                                 tris;
			std::unordered_map<long long, std::vector<int>> buckets;
			std::vector<Obj>                                 queue;
			std::size_t                                      cursor{ 0 };
			int                                              used{ 0 };
			Vec3d                                            feet{};
			ULONGLONG                                        startedAt{ 0 };
			bool                                             active{ false };
		};
		Building next;

		// One component's collision triangles, in Minecraft space, added to a_b.
		bool AddComponent(Building& a_b, Obj a_comp, double a_upb)
		{
			const Obj mesh = ue3::GetObj(a_comp, fMesh);
			if (!mesh) {
				return false;
			}
			const LocalMesh& lm = Decode(mesh);
			if (!lm.ok) {
				return false;
			}
			float m[16];
			if (!ReadBytes(a_comp + static_cast<std::uintptr_t>(fLocalToWorld.offset), sizeof(m), m)) {
				return false;
			}
			const std::size_t nv = lm.pos.size() / 3;
			std::vector<Vec3d> w(nv);
			for (std::size_t i = 0; i < nv; ++i) {
				const float* p = &lm.pos[i * 3];
				const UeVector u{ p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12], p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13],
					p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14] };
				w[i] = UeToMc(u, a_upb);
			}
			for (std::size_t t = 0; t + 2 < lm.idx.size(); t += 3) {
				Tri tri{};
				for (int k = 0; k < 3; ++k) {
					const Vec3d& v = w[lm.idx[t + k]];
					tri.x[k] = static_cast<float>(v.x);
					tri.y[k] = static_cast<float>(v.y);
					tri.z[k] = static_cast<float>(v.z);
				}
				tri.minX = std::min({ tri.x[0], tri.x[1], tri.x[2] });
				tri.maxX = std::max({ tri.x[0], tri.x[1], tri.x[2] });
				tri.minZ = std::min({ tri.z[0], tri.z[1], tri.z[2] });
				tri.maxZ = std::max({ tri.z[0], tri.z[1], tri.z[2] });
				if (tri.maxX - tri.minX > 256 || tri.maxZ - tri.minZ > 256) {
					continue;  // garbage
				}
				const int id = static_cast<int>(a_b.tris.size());
				a_b.tris.push_back(tri);
				for (int bx = static_cast<int>(std::floor(tri.minX)); bx <= static_cast<int>(std::floor(tri.maxX)); ++bx) {
					for (int bz = static_cast<int>(std::floor(tri.minZ)); bz <= static_cast<int>(std::floor(tri.maxZ)); ++bz) {
						a_b.buckets[BucketKey(bx, bz)].push_back(id);
					}
				}
			}
			return true;
		}

		// Every static mesh component in the level (rescanned now and then; cheap compared to decoding).
		std::vector<Obj> components;
		ULONGLONG        componentsAt = 0;

		void ScanComponents()
		{
			components.clear();
			for (int i = 0, n = ue3::ObjectCount(); i < n; ++i) {
				const Obj o = ue3::ObjectAt(i);
				if (o && ue3::IsA(o, smcClass) && !ue3::IsDefaultObject(o)) {
					components.push_back(o);
				}
			}
			componentsAt = ::GetTickCount64();
		}

		void StartBuild(const Vec3d& a_feet)
		{
			const double upb = State().unitsPerBlock;
			if (components.empty() || ::GetTickCount64() - componentsAt > 30000) {
				ScanComponents();
			}
			next = Building{};
			next.active = true;
			next.feet = a_feet;
			next.startedAt = ::GetTickCount64();
			static const double radius = config::Float("World", "fMeshRadiusBlocks", 40.0f);
			const UeVector      centre = McToUe(a_feet.x, a_feet.y, a_feet.z, upb);
			const double        reach = radius * upb;
			std::vector<std::pair<double, Obj>> near_;
			for (const Obj o : components) {
				float b[7];
				if (!ReadBytes(o + static_cast<std::uintptr_t>(fBounds.offset), sizeof(b), b) || !(b[6] > 1.0f) || b[6] > 1e6f) {
					continue;
				}
				const double dx = b[0] - centre.x, dy = b[1] - centre.y, dz = b[2] - centre.z;
				const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (d > reach + b[6] || ue3::GetBool(o, bind::F.hiddenGame)) {
					continue;
				}
				near_.emplace_back(d, o);
			}
			std::sort(near_.begin(), near_.end());  // nearest first
			for (const auto& [d, o] : near_) {
				next.queue.push_back(o);
			}
		}

		// Works on the build for about a_budgetMs; swaps it in when complete.
		void StepBuild(double a_budgetMs)
		{
			LARGE_INTEGER freq, start, now;
			::QueryPerformanceFrequency(&freq);
			::QueryPerformanceCounter(&start);
			const double upb = State().unitsPerBlock;
			while (next.cursor < next.queue.size()) {
				if (AddComponent(next, next.queue[next.cursor], upb)) {
					++next.used;
				}
				++next.cursor;
				::QueryPerformanceCounter(&now);
				if (double(now.QuadPart - start.QuadPart) * 1000.0 / double(freq.QuadPart) > a_budgetMs) {
					return;
				}
			}
			tris.swap(next.tris);
			buckets.swap(next.buckets);
			meshesUsed = next.used;
			builtFor = next.feet;
			builtAt = ::GetTickCount64();
			ready = !tris.empty();
			if (breakPending) {
				breakPending = false;
				++generation;
			}
			static int logs = 0;
			if (logs++ < 8) {
				DC_INFO("mesh: %d of %zu nearby static meshes give %zu collision triangles (built over %llu ms, %zu meshes decoded)", meshesUsed, next.queue.size(),
					tris.size(), builtAt - next.startedAt, meshes.size());
			}
			next = Building{};
		}
	}

	bool Ready() { return enabled && ready; }

	void Update(const Vec3d& a_feet)
	{
		static bool configured = false;
		if (!configured) {
			configured = true;
			enabled = config::Bool("World", "bMeshCollision", true);
			smcClass = ue3::FindClass("Engine.StaticMeshComponent");
			fMesh = ue3::FindField(smcClass, "StaticMesh");
			fLocalToWorld = ue3::FindField(smcClass, "LocalToWorld");
			fBounds = ue3::FindField(smcClass, "Bounds");
			enabled = enabled && smcClass && fMesh && fLocalToWorld && fBounds;
			DC_INFO("mesh: collision from static meshes %s", enabled ? "on" : "off (classes not found or bMeshCollision=0)");
		}
		if (!enabled) {
			return;
		}
		if (!next.active) {
			const double    moved = std::hypot(a_feet.x - builtFor.x, a_feet.z - builtFor.z);
			const ULONGLONG now = ::GetTickCount64();
			static const double radius = config::Float("World", "fMeshRadiusBlocks", 40.0f);
			if (moved > radius * 0.4 || now - builtAt > 20000) {
				StartBuild(a_feet);
			}
		}
		if (next.active) {
			// Nothing yet: build faster (the player waits for collision anyway).
			static const double budget = config::Float("World", "fMeshBudgetMs", 2.0f);
			StepBuild(ready ? budget : budget * 4.0);
		}
	}

	void Reset()
	{
		tris.clear();
		buckets.clear();
		meshes.clear();
		components.clear();
		next = Building{};
		ready = false;
		builtAt = 0;
		builtFor = { 1e9, 1e9, 1e9 };
	}

	int Generation()
	{
		return generation;
	}

	void BreakAt(const std::vector<std::array<int, 3>>& a_cells)
	{
		if (!enabled || a_cells.empty()) {
			return;
		}
		if (components.empty()) {
			ScanComponents();
		}
		const double upb = State().unitsPerBlock;
		static const double maxRadius = config::Float("World", "fBreakableRadiusBlocks", 3.0f) * upb;
		static const ue3::Field fields[] = { ue3::FindField(smcClass, "CollideActors"), ue3::FindField(smcClass, "BlockActors"),
			ue3::FindField(smcClass, "BlockZeroExtent"), ue3::FindField(smcClass, "BlockNonZeroExtent"), ue3::FindField(smcClass, "BlockRigidBody") };
		int broken = 0;
		for (const auto& c : a_cells) {
			const UeVector p = McToUe(c[0] + 0.5, c[1] + 0.5, c[2] + 0.5, upb);
			for (const Obj o : components) {
				float b[7];
				if (!ReadBytes(o + static_cast<std::uintptr_t>(fBounds.offset), sizeof(b), b) || !(b[6] > 1.0f) || b[6] > maxRadius) {
					continue;
				}
				const float pad = static_cast<float>(upb * 0.5);
				if (std::abs(p.x - b[0]) > b[3] + pad || std::abs(p.y - b[1]) > b[4] + pad || std::abs(p.z - b[2]) > b[5] + pad) {
					continue;
				}
				if (ue3::GetBool(o, bind::F.hiddenGame)) {
					continue;
				}
				// The whole prop goes: not drawn, nothing to bump into.
				bool hid = false;
				if (bind::Fn.setComponentHidden) {
					ue3::Params h(bind::Fn.setComponentHidden);
					h.SetBool("NewHidden", true);
					hid = h.Invoke(o);
				}
				if (!hid || !ue3::GetBool(o, bind::F.hiddenGame)) {
					ue3::SetBool(o, bind::F.hiddenGame, true);
				}
				for (const auto& f : fields) {
					if (f) {
						ue3::SetBool(o, f, false);
					}
				}
				++broken;
				static int logged = 0;
				if (logged++ < 20) {
					const Obj mesh = ue3::GetObj(o, fMesh);
					DC_INFO("mesh: broke %s (%s) at block (%d, %d, %d)", ue3::NameOf(o).c_str(), mesh ? ue3::PathOf(mesh).c_str() : "?", c[0], c[1], c[2]);
				}
			}
		}
		if (broken) {
			breakPending = true;  // generation moves when the rebuilt collision is swapped in
			next = Building{};
			builtAt = 0;  // rebuild collision without them
			builtFor = { 1e9, 1e9, 1e9 };
		}
	}

	void Column(double a_x, double a_z, float a_yMin, float a_yMax, std::vector<mesher::Hit>& a_out)
	{
		a_out.clear();
		const auto it = buckets.find(BucketKey(static_cast<int>(std::floor(a_x)), static_cast<int>(std::floor(a_z))));
		if (it == buckets.end()) {
			return;
		}
		const float px = static_cast<float>(a_x), pz = static_cast<float>(a_z);
		for (const int id : it->second) {
			const Tri& t = tris[id];
			if (px < t.minX || px > t.maxX || pz < t.minZ || pz > t.maxZ) {
				continue;
			}
			// Barycentric in xz.
			const float d = (t.z[1] - t.z[2]) * (t.x[0] - t.x[2]) + (t.x[2] - t.x[1]) * (t.z[0] - t.z[2]);
			if (std::abs(d) < 1e-9f) {
				continue;  // vertical: a wall; columns next to it see it as the end of a floor
			}
			const float w0 = ((t.z[1] - t.z[2]) * (px - t.x[2]) + (t.x[2] - t.x[1]) * (pz - t.z[2])) / d;
			const float w1 = ((t.z[2] - t.z[0]) * (px - t.x[2]) + (t.x[0] - t.x[2]) * (pz - t.z[2])) / d;
			const float w2 = 1.0f - w0 - w1;
			if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) {
				continue;
			}
			const float y = w0 * t.y[0] + w1 * t.y[1] + w2 * t.y[2];
			if (y < a_yMin || y > a_yMax) {
				continue;
			}
			// Normal in Minecraft space. The mapping from Unreal flips handedness and the winding
			// convention isn't known, so which way a surface faces is taken from the geometry around
			// it instead: anything walkable-flat counts as a top (most of what a vertical line meets in
			// a city), steep undersides as bottoms.
			const float ux = t.x[1] - t.x[0], uy = t.y[1] - t.y[0], uz = t.z[1] - t.z[0];
			const float vx = t.x[2] - t.x[0], vy = t.y[2] - t.y[0], vz = t.z[2] - t.z[0];
			float       nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
			const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
			if (len < 1e-9f) {
				continue;
			}
			nx /= len;
			ny /= len;
			nz /= len;
			static const int winding = config::Int("World", "iMeshWinding", 0);  // 0 auto (flat = top), 1 / -1 forced
			bool up = ny > 0.0f;
			if (winding == 0) {
				up = true;  // a vertical line through a closed mesh meets tops and bottoms in pairs; see below
			} else if (winding < 0) {
				up = !up;
			}
			mesher::Hit h;
			h.y = y;
			h.up = up;
			h.nx = nx * (ny < 0 ? -1.0f : 1.0f);
			h.ny = std::abs(ny);
			h.nz = nz * (ny < 0 ? -1.0f : 1.0f);
			a_out.push_back(h);
		}
		if (a_out.size() > 1) {
			// Auto winding: sorted from the top, surfaces alternate top, underside, top, ... (the
			// line enters and leaves each solid). Two surfaces within a hair are the same one.
			std::sort(a_out.begin(), a_out.end(), [](const mesher::Hit& a, const mesher::Hit& b) { return a.y > b.y; });
			std::vector<mesher::Hit> merged;
			for (const auto& h : a_out) {
				if (!merged.empty() && std::abs(merged.back().y - h.y) < 0.02f) {
					continue;
				}
				merged.push_back(h);
			}
			static const int winding = config::Int("World", "iMeshWinding", 0);
			if (winding == 0) {
				for (std::size_t i = 0; i < merged.size(); ++i) {
					merged[i].up = (i % 2) == 0;
				}
			}
			a_out.swap(merged);
		}
		if (a_out.size() > 32) {
			a_out.resize(32);
		}
	}

	std::string Summary()
	{
		char b[96];
		std::snprintf(b, sizeof(b), "mesh %s: %zu tris from %d meshes", Ready() ? "on" : "off", tris.size(), meshesUsed);
		return b;
	}
}
