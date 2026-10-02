#include "Game.h"

#include "Config.h"
#include "Log.h"
#include "Mem.h"
#include "WorldRender.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

// Direct3D 9 (Dishonored's renderer): Present draws Minecraft into the finished frame. First the
// blocks and entities in the world (depth-tested against the game's depth buffer when it can be
// used), then Minecraft's overlay (hand, hotbar, hearts, inventory and every other screen),
// then DisCraft's own status line.
namespace discraft::Render
{
	namespace
	{
		using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
		using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
		using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);

		struct DeviceHooks
		{
			void**    vtable;
			PresentFn present;
			ResetFn   reset;
		};
		std::mutex               hookLock;
		std::vector<DeviceHooks> deviceHooks;
		CreateDeviceFn           originalCreateDevice = nullptr;

		struct ScreenVertex
		{
			float    x, y, z, rhw;
			D3DCOLOR color;
			float    u, v;
		};
		constexpr DWORD kScreenFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

		// Render-thread resources.
		IDirect3DStateBlock9* stateBlock = nullptr;
		IDirect3DTexture9*    overlay = nullptr;
		UINT                  overlayW = 0, overlayH = 0;
		bool                  overlayFlip = true;
		bool                  haveOverlay = false;
		bool                  overlayNative = false;  // the device takes RGBA (A8B8G8R8) as is
		bool                  formatChecked = false;
		IDirect3DTexture9*    textTexture = nullptr;
		UINT                  textW = 0, textH = 0;
		std::wstring          textShown;
		int                   lastScreenH = 0;
		bool                  drawBlocks = true;
		bool                  configured = false;

		std::mutex   noteLock;
		std::wstring note;
		ULONGLONG    noteUntil = 0;
		int          depthMode = 1;

		template <class T>
		void Release(T*& a_p)
		{
			if (a_p) {
				a_p->Release();
				a_p = nullptr;
			}
		}

		const DeviceHooks* HooksOf(IDirect3DDevice9* a_device)
		{
			void**          vtable = *reinterpret_cast<void***>(a_device);
			std::lock_guard guard(hookLock);
			for (const auto& h : deviceHooks) {
				if (h.vtable == vtable) {
					return &h;
				}
			}
			return nullptr;
		}

		// ---- the overlay -----------------------------------------------------------------------
		void CheckFormat(IDirect3DDevice9* a_device)
		{
			formatChecked = true;
			IDirect3D9*                 d3d = nullptr;
			D3DDEVICE_CREATION_PARAMETERS params{};
			D3DDISPLAYMODE              mode{};
			if (SUCCEEDED(a_device->GetDirect3D(&d3d)) && SUCCEEDED(a_device->GetCreationParameters(&params)) &&
				SUCCEEDED(d3d->GetAdapterDisplayMode(params.AdapterOrdinal, &mode))) {
				overlayNative = SUCCEEDED(d3d->CheckDeviceFormat(params.AdapterOrdinal, params.DeviceType, mode.Format, 0, D3DRTYPE_TEXTURE, D3DFMT_A8B8G8R8));
			}
			Release(d3d);
			DC_INFO("render: overlay texture format %s", overlayNative ? "A8B8G8R8 (no conversion)" : "A8R8G8B8 (converted)");
		}

		void UploadOverlay(IDirect3DDevice9* a_device)
		{
			auto& link = Link::Get();
			if (!link.AcquireOverlayFrame()) {
				return;
			}
			const auto* hdr = link.FrontHeader();
			if (!hdr->width || !hdr->height || hdr->width > proto::kMaxOverlayW || hdr->height > proto::kMaxOverlayH) {
				return;
			}
			if (!formatChecked) {
				CheckFormat(a_device);
			}
			if (!overlay || overlayW != hdr->width || overlayH != hdr->height) {
				Release(overlay);
				if (FAILED(a_device->CreateTexture(hdr->width, hdr->height, 1, 0, overlayNative ? D3DFMT_A8B8G8R8 : D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &overlay,
						nullptr))) {
					DC_ERROR("render: overlay texture %ux%u failed", hdr->width, hdr->height);
					return;
				}
				overlayW = hdr->width;
				overlayH = hdr->height;
				DC_INFO("render: overlay %ux%u", overlayW, overlayH);
			}
			D3DLOCKED_RECT locked{};
			if (FAILED(overlay->LockRect(0, &locked, nullptr, 0))) {
				return;
			}
			const auto* src = link.FrontPixels();
			const UINT  row = overlayW * 4;
			for (UINT y = 0; y < overlayH; ++y) {
				auto*       dst = static_cast<std::uint8_t*>(locked.pBits) + std::size_t(y) * locked.Pitch;
				const auto* s = src + std::size_t(y) * row;
				if (overlayNative) {
					std::memcpy(dst, s, row);
				} else {
					auto*       d32 = reinterpret_cast<std::uint32_t*>(dst);
					const auto* s32 = reinterpret_cast<const std::uint32_t*>(s);
					for (UINT x = 0; x < overlayW; ++x) {
						const std::uint32_t p = s32[x];
						d32[x] = (p & 0xFF00FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16);
					}
				}
			}
			overlay->UnlockRect(0);
			overlayFlip = (hdr->flags & 1) != 0;
			haveOverlay = true;
		}

		void Rect(std::vector<ScreenVertex>& a_out, float a_x0, float a_y0, float a_x1, float a_y1, float a_w, float a_h, D3DCOLOR a_color, bool a_flip)
		{
			if (a_x1 <= a_x0 || a_y1 <= a_y0) {
				return;
			}
			const auto uv = [&](float x, float y, float& u, float& v) {
				u = x / a_w;
				v = y / a_h;
				if (a_flip) {
					v = 1.0f - v;
				}
			};
			ScreenVertex q[4];
			const float  xs[4] = { a_x0, a_x1, a_x1, a_x0 }, ys[4] = { a_y0, a_y0, a_y1, a_y1 };
			for (int i = 0; i < 4; ++i) {
				q[i] = { xs[i] - 0.5f, ys[i] - 0.5f, 0.0f, 1.0f, a_color, 0.0f, 0.0f };
				uv(xs[i], ys[i], q[i].u, q[i].v);
			}
			a_out.insert(a_out.end(), { q[0], q[1], q[2], q[0], q[2], q[3] });
		}

		void Common2D(IDirect3DDevice9* a_device)
		{
			a_device->SetVertexShader(nullptr);
			a_device->SetPixelShader(nullptr);
			a_device->SetFVF(kScreenFvf);
			a_device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
			a_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
			a_device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
			a_device->SetRenderState(D3DRS_LIGHTING, FALSE);
			a_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
			a_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
			a_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
			a_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
			a_device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
			a_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
			a_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
			a_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
			a_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
			a_device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
			a_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
			a_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			a_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			a_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
			a_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			a_device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
			a_device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
			a_device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
			a_device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
			a_device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
			a_device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
			a_device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
			a_device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
		}

		void DrawOverlay(IDirect3DDevice9* a_device, int a_w, int a_h)
		{
			auto&       st = State();
			const float w = float(a_w), h = float(a_h);
			const bool  scaled = overlayW != static_cast<UINT>(a_w) || overlayH != static_cast<UINT>(a_h);
			Common2D(a_device);
			a_device->SetSamplerState(0, D3DSAMP_MINFILTER, scaled ? D3DTEXF_LINEAR : D3DTEXF_POINT);
			a_device->SetSamplerState(0, D3DSAMP_MAGFILTER, scaled ? D3DTEXF_LINEAR : D3DTEXF_POINT);
			a_device->SetTexture(0, overlay);

			// Minecraft's crosshair and attack indicator are drawn with its invert blend against the
			// game's picture, in a pass of their own; the main pass leaves that rectangle out.
			const int   scale = st.mcGuiScale;
			const float g = float(scale) * w / float(std::max<UINT>(overlayW, 1));
			const bool  invert = st.mcCrosshair && scale > 0;
			const float cx = w * 0.5f, cy = h * 0.5f;
			const float ix0 = invert ? cx - 12.0f * g : 0, iy0 = invert ? cy - 12.0f * g : 0, ix1 = invert ? cx + 12.0f * g : 0,
						iy1 = invert ? cy + 28.0f * g : 0;
			std::vector<ScreenVertex> quads;
			const D3DCOLOR            white = D3DCOLOR_ARGB(255, 255, 255, 255);
			if (invert) {
				Rect(quads, 0, 0, w, iy0, w, h, white, overlayFlip);
				Rect(quads, 0, iy1, w, h, w, h, white, overlayFlip);
				Rect(quads, 0, iy0, ix0, iy1, w, h, white, overlayFlip);
				Rect(quads, ix1, iy0, w, iy1, w, h, white, overlayFlip);
			} else {
				Rect(quads, 0, 0, w, h, w, h, white, overlayFlip);
			}
			if (!quads.empty()) {
				a_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(quads.size() / 3), quads.data(), sizeof(ScreenVertex));
			}
			if (invert) {
				quads.clear();
				Rect(quads, ix0, iy0, ix1, iy1, w, h, white, overlayFlip);
				// out = src * (1 - dst) + dst * (1 - src): Minecraft's BlendFunction.INVERT.
				a_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_INVDESTCOLOR);
				a_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCCOLOR);
				a_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quads.data(), sizeof(ScreenVertex));
				a_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
				a_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
			}
			// Minecraft's (virtual) mouse cursor while one of its screens is open: a plain arrow.
			if (st.mcScreenOpen) {
				const float px = float(st.cursorX) * w / float(std::max<UINT>(overlayW, 1));
				const float py = float(st.cursorY) * h / float(std::max<UINT>(overlayH, 1));
				const float s = std::max(1.0f, h / 1080.0f);
				const auto  v = [&](float x, float y, D3DCOLOR c) { return ScreenVertex{ px + x * s, py + y * s, 0, 1, c, 0, 0 }; };
				const D3DCOLOR black = D3DCOLOR_ARGB(255, 0, 0, 0);
				const ScreenVertex arrow[6] = { v(-1, -2, black), v(13, 14, black), v(-1, 20, black), v(1, 2, white), v(9, 13, white), v(1, 16, white) };
				a_device->SetTexture(0, nullptr);
				a_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
				a_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
				a_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
				a_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
				a_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, arrow, sizeof(ScreenVertex));
			}
		}

		// ---- the status line (GDI text in a texture) ---------------------------------------------
		void BuildText(IDirect3DDevice9* a_device, const std::wstring& a_text, int a_screenH)
		{
			textShown = a_text;
			lastScreenH = a_screenH;
			Release(textTexture);
			if (a_text.empty()) {
				return;
			}
			const int fontPx = std::max(16, a_screenH / 48);
			HDC       dc = ::CreateCompatibleDC(nullptr);
			HFONT     font = ::CreateFontW(-fontPx, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
					ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
			HGDIOBJ   oldFont = ::SelectObject(dc, font);
			RECT      measure{ 0, 0, 4096, 0 };
			::DrawTextW(dc, a_text.c_str(), static_cast<int>(a_text.size()), &measure, DT_CALCRECT | DT_LEFT | DT_NOPREFIX | DT_WORDBREAK);
			const int w = std::clamp<int>(measure.right + 8, 8, 4096), h = std::clamp<int>(measure.bottom + 8, 8, 1024);
			BITMAPINFO bi{};
			bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
			bi.bmiHeader.biWidth = w;
			bi.bmiHeader.biHeight = -h;
			bi.bmiHeader.biPlanes = 1;
			bi.bmiHeader.biBitCount = 32;
			bi.bmiHeader.biCompression = BI_RGB;
			void*   bits = nullptr;
			HBITMAP bmp = ::CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
			if (bmp && bits) {
				HGDIOBJ oldBmp = ::SelectObject(dc, bmp);
				std::memset(bits, 0, std::size_t(w) * h * 4);
				::SetBkMode(dc, TRANSPARENT);
				::SetTextColor(dc, RGB(255, 255, 255));
				RECT r{ 4, 4, w - 4, h - 4 };
				::DrawTextW(dc, a_text.c_str(), static_cast<int>(a_text.size()), &r, DT_LEFT | DT_NOPREFIX | DT_WORDBREAK);
				::GdiFlush();
				if (SUCCEEDED(a_device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &textTexture, nullptr))) {
					D3DLOCKED_RECT locked{};
					if (SUCCEEDED(textTexture->LockRect(0, &locked, nullptr, 0))) {
						for (int y = 0; y < h; ++y) {
							const auto* src = static_cast<const std::uint32_t*>(bits) + std::size_t(y) * w;
							auto*       dst = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(locked.pBits) + std::size_t(y) * locked.Pitch);
							for (int x = 0; x < w; ++x) {
								// White text on black: coverage is the brightness. Premultiplied white.
								const std::uint32_t p = src[x];
								const std::uint32_t a = std::max({ p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF });
								dst[x] = (a << 24) | (a << 16) | (a << 8) | a;
							}
						}
						textTexture->UnlockRect(0);
						textW = static_cast<UINT>(w);
						textH = static_cast<UINT>(h);
					}
				}
				::SelectObject(dc, oldBmp);
				::DeleteObject(bmp);
			}
			::SelectObject(dc, oldFont);
			::DeleteObject(font);
			::DeleteDC(dc);
		}

		void DrawText(IDirect3DDevice9* a_device, int a_screenH)
		{
			if (!textTexture) {
				return;
			}
			Common2D(a_device);
			a_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
			a_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
			a_device->SetTexture(0, textTexture);
			const float margin = std::max(12.0f, a_screenH / 60.0f);
			const float w = float(textW), h = float(textH);
			std::vector<ScreenVertex> quads;
			// Shadow (black: the texture's colour times the black diffuse), then the text.
			a_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
			a_device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			Rect(quads, margin + 2, margin + 2, margin + 2 + w, margin + 2 + h, w, h, D3DCOLOR_ARGB(255, 0, 0, 0), false);
			for (auto& q : quads) {
				q.u = (q.x + 0.5f - margin - 2) / w;
				q.v = (q.y + 0.5f - margin - 2) / h;
			}
			a_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quads.data(), sizeof(ScreenVertex));
			quads.clear();
			Rect(quads, margin, margin, margin + w, margin + h, w, h, D3DCOLOR_ARGB(255, 255, 255, 255), false);
			for (auto& q : quads) {
				q.u = (q.x + 0.5f - margin) / w;
				q.v = (q.y + 0.5f - margin) / h;
			}
			a_device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, quads.data(), sizeof(ScreenVertex));
		}

		std::wstring CurrentText()
		{
			std::wstring text = Game::StatusText();
			std::lock_guard guard(noteLock);
			if (!note.empty() && ::GetTickCount64() < noteUntil) {
				text = text.empty() ? note : text + L"\n" + note;
			}
			return text;
		}

		// ---- the frame -------------------------------------------------------------------------
		void Frame(IDirect3DDevice9* a_device)
		{
			if (!configured) {
				configured = true;
				drawBlocks = config::Bool("Render", "bDrawBlocks", true);
				depthMode = std::clamp(config::Int("Render", "iDepthMode", 1), 0, 2);
			}
			auto& st = State();
			auto& link = Link::Get();
			IDirect3DSurface9* backBuffer = nullptr;
			if (FAILED(a_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer) {
				return;
			}
			D3DSURFACE_DESC desc{};
			backBuffer->GetDesc(&desc);
			st.screenW = static_cast<int>(desc.Width);
			st.screenH = static_cast<int>(desc.Height);
			// Keep Minecraft linked through game pauses that stop the game's own frame update.
			if (::GetTickCount64() - st.lastTickMs.load() < 2000) {
				link.Heartbeat();
			}
			WorldRender::Drain(a_device);  // always: the ring mustn't fill up
			UploadOverlay(a_device);

			const bool hidden = st.gameMenuOpen || st.suspended;
			const bool world = drawBlocks && st.mcInWorld && link.McAlive() && !hidden;
			const bool ui = haveOverlay && st.mcInWorld && link.McAlive() && !hidden;
			const auto text = CurrentText();
			if (text != textShown || (lastScreenH != static_cast<int>(desc.Height) && !text.empty())) {
				BuildText(a_device, text, static_cast<int>(desc.Height));
			}
			if (!world && !ui && text.empty()) {
				backBuffer->Release();
				return;
			}
			if (!stateBlock && FAILED(a_device->CreateStateBlock(D3DSBT_ALL, &stateBlock))) {
				backBuffer->Release();
				return;
			}
			stateBlock->Capture();
			IDirect3DSurface9* oldTarget = nullptr;
			IDirect3DSurface9* oldDepth = nullptr;
			D3DVIEWPORT9       oldViewport{};
			a_device->GetRenderTarget(0, &oldTarget);
			a_device->GetDepthStencilSurface(&oldDepth);
			a_device->GetViewport(&oldViewport);

			a_device->SetRenderTarget(0, backBuffer);
			a_device->SetDepthStencilSurface(oldDepth);
			D3DVIEWPORT9 vp{ 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
			a_device->SetViewport(&vp);
			if (SUCCEEDED(a_device->BeginScene())) {
				if (world) {
					WorldRender::Draw(a_device, Game::View(), static_cast<int>(desc.Width), static_cast<int>(desc.Height), depthMode);
				}
				if (ui) {
					DrawOverlay(a_device, static_cast<int>(desc.Width), static_cast<int>(desc.Height));
				}
				if (!text.empty()) {
					DrawText(a_device, static_cast<int>(desc.Height));
				}
				a_device->EndScene();
			}
			a_device->SetRenderTarget(0, oldTarget);
			a_device->SetDepthStencilSurface(oldDepth);
			a_device->SetViewport(&oldViewport);
			stateBlock->Apply();
			Release(oldTarget);
			Release(oldDepth);
			backBuffer->Release();
		}

		HRESULT STDMETHODCALLTYPE PresentHook(IDirect3DDevice9* a_device, const RECT* a_src, const RECT* a_dst, HWND a_window, const RGNDATA* a_dirty)
		{
			const auto* hooks = HooksOf(a_device);
			Frame(a_device);
			return hooks ? hooks->present(a_device, a_src, a_dst, a_window, a_dirty) : D3DERR_INVALIDCALL;
		}

		HRESULT STDMETHODCALLTYPE ResetHook(IDirect3DDevice9* a_device, D3DPRESENT_PARAMETERS* a_params)
		{
			const auto* hooks = HooksOf(a_device);
			Release(stateBlock);
			WorldRender::OnLostDevice();
			DC_INFO("render: device reset");
			return hooks ? hooks->reset(a_device, a_params) : D3DERR_INVALIDCALL;
		}

		void HookDevice(IDirect3DDevice9* a_device)
		{
			void** vtable = *reinterpret_cast<void***>(a_device);
			{
				std::lock_guard guard(hookLock);
				for (const auto& h : deviceHooks) {
					if (h.vtable == vtable) {
						return;
					}
				}
				deviceHooks.push_back({ vtable, reinterpret_cast<PresentFn>(vtable[17]), reinterpret_cast<ResetFn>(vtable[16]) });
			}
			mem::WritePointer(&vtable[17], reinterpret_cast<void*>(&PresentHook));
			mem::WritePointer(&vtable[16], reinterpret_cast<void*>(&ResetHook));
			DC_INFO("render: Direct3D 9 device hooked");
		}

		HRESULT STDMETHODCALLTYPE CreateDeviceHook(IDirect3D9* a_d3d, UINT a_adapter, D3DDEVTYPE a_type, HWND a_focus, DWORD a_flags,
			D3DPRESENT_PARAMETERS* a_params, IDirect3DDevice9** a_device)
		{
			const HRESULT hr = originalCreateDevice(a_d3d, a_adapter, a_type, a_focus, a_flags, a_params, a_device);
			if (SUCCEEDED(hr) && a_device && *a_device) {
				DC_INFO("render: the game created its Direct3D 9 device (%ux%u, flags 0x%lX)", a_params ? a_params->BackBufferWidth : 0,
					a_params ? a_params->BackBufferHeight : 0, a_flags);
				HookDevice(*a_device);
			}
			return hr;
		}
	}

	void InstallEarly()
	{
		HMODULE module = ::LoadLibraryW(L"d3d9.dll");
		if (!module) {
			DC_ERROR("render: d3d9.dll not found");
			return;
		}
		using CreateFn = IDirect3D9*(WINAPI*)(UINT);
		const auto create = reinterpret_cast<CreateFn>(reinterpret_cast<void*>(::GetProcAddress(module, "Direct3DCreate9")));
		IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
		if (!d3d) {
			DC_ERROR("render: Direct3DCreate9 failed");
			return;
		}
		// Every IDirect3D9 shares this vtable: the game's CreateDevice comes through our hook.
		void** vtable = *reinterpret_cast<void***>(d3d);
		originalCreateDevice = reinterpret_cast<CreateDeviceFn>(vtable[16]);
		mem::WritePointer(&vtable[16], reinterpret_cast<void*>(&CreateDeviceHook));

		// A throwaway device hooks the device vtable now too, in case the game's device exists already.
		HWND window = ::CreateWindowExW(0, L"STATIC", L"DisCraft", WS_OVERLAPPED, 0, 0, 8, 8, nullptr, nullptr, nullptr, nullptr);
		D3DPRESENT_PARAMETERS pp{};
		pp.Windowed = TRUE;
		pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
		pp.BackBufferFormat = D3DFMT_UNKNOWN;
		pp.hDeviceWindow = window;
		IDirect3DDevice9* device = nullptr;
		if (window && SUCCEEDED(originalCreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
						  D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &pp, &device)) &&
			device) {
			HookDevice(device);
			device->Release();
		} else {
			DC_WARN("render: no throwaway device (the game's own device is hooked when it's created)");
		}
		if (window) {
			::DestroyWindow(window);
		}
		d3d->Release();
	}

	void Notify(const std::wstring& a_text, float a_seconds)
	{
		std::lock_guard guard(noteLock);
		note = a_text;
		noteUntil = ::GetTickCount64() + static_cast<ULONGLONG>(a_seconds * 1000.0f);
	}

	void CycleDepthMode()
	{
		depthMode = (depthMode + 1) % 3;
		static const wchar_t* names[] = { L"DisCraft depth: blocks over everything (own depth only)", L"DisCraft depth: the game's depth (normal Z)",
			L"DisCraft depth: the game's depth (reversed Z)" };
		Notify(names[depthMode], 3.0f);
		DC_INFO("render: depth mode %d", depthMode);
	}
}
