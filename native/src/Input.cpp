#include "Game.h"

#include "Config.h"
#include "Log.h"
#include "Mem.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

// Keyboard and mouse: while Minecraft drives the player, the game window's input goes to Minecraft
// (through the shared input ring), except a few keys Dishonored keeps (its menu, journal,
// quickload). Dishonored may read the mouse and keyboard through window messages, Raw Input or
// DirectInput; all three are filtered so the game sees nothing it shouldn't.
namespace discraft::Input
{
	namespace
	{
		// Windows scan code (bit 7 set for extended keys, like DirectInput's DIK codes) -> SDL
		// scancode / USB HID usage (what Minecraft uses). Table from SkyCraft (MIT).
		constexpr auto kDikToSdl = [] {
			std::array<std::uint16_t, 256> t{};
			t[0x01] = 41;  // Esc
			for (int i = 0; i < 9; ++i) t[0x02 + i] = static_cast<std::uint16_t>(30 + i);  // 1-9
			t[0x0B] = 39;  // 0
			t[0x0C] = 45, t[0x0D] = 46, t[0x0E] = 42, t[0x0F] = 43;  // - = Backspace Tab
			t[0x10] = 20, t[0x11] = 26, t[0x12] = 8, t[0x13] = 21, t[0x14] = 23;  // Q W E R T
			t[0x15] = 28, t[0x16] = 24, t[0x17] = 12, t[0x18] = 18, t[0x19] = 19;  // Y U I O P
			t[0x1A] = 47, t[0x1B] = 48, t[0x1C] = 40, t[0x1D] = 224;               // [ ] Enter LCtrl
			t[0x1E] = 4, t[0x1F] = 22, t[0x20] = 7, t[0x21] = 9, t[0x22] = 10;     // A S D F G
			t[0x23] = 11, t[0x24] = 13, t[0x25] = 14, t[0x26] = 15;                // H J K L
			t[0x27] = 51, t[0x28] = 52, t[0x29] = 53, t[0x2A] = 225, t[0x2B] = 49;  // ; ' ` LShift backslash
			t[0x2C] = 29, t[0x2D] = 27, t[0x2E] = 6, t[0x2F] = 25, t[0x30] = 5;    // Z X C V B
			t[0x31] = 17, t[0x32] = 16, t[0x33] = 54, t[0x34] = 55, t[0x35] = 56;  // N M , . /
			t[0x36] = 229, t[0x37] = 85, t[0x38] = 226, t[0x39] = 44, t[0x3A] = 57;  // RShift KP* LAlt Space Caps
			for (int i = 0; i < 10; ++i) t[0x3B + i] = static_cast<std::uint16_t>(58 + i);  // F1-F10
			t[0x45] = 83, t[0x46] = 71;                                             // NumLock ScrollLock
			t[0x47] = 95, t[0x48] = 96, t[0x49] = 97, t[0x4A] = 86;                 // KP7 KP8 KP9 KP-
			t[0x4B] = 92, t[0x4C] = 93, t[0x4D] = 94, t[0x4E] = 87;                 // KP4 KP5 KP6 KP+
			t[0x4F] = 89, t[0x50] = 90, t[0x51] = 91, t[0x52] = 98, t[0x53] = 99;   // KP1 KP2 KP3 KP0 KP.
			t[0x56] = 100, t[0x57] = 68, t[0x58] = 69;                              // OEM102 F11 F12
			t[0x9C] = 88, t[0x9D] = 228, t[0xB5] = 84, t[0xB7] = 70, t[0xB8] = 230;  // KPEnter RCtrl KP/ PrtSc RAlt
			t[0xC5] = 72, t[0xC7] = 74, t[0xC8] = 82, t[0xC9] = 75, t[0xCB] = 80;  // Pause Home Up PgUp Left
			t[0xCD] = 79, t[0xCF] = 77, t[0xD0] = 81, t[0xD1] = 78, t[0xD2] = 73;  // Right End Down PgDn Insert
			t[0xD3] = 76, t[0xDB] = 227, t[0xDC] = 231, t[0xDD] = 101;             // Delete LWin RWin Menu
			return t;
		}();

		// ---- configuration -----------------------------------------------------------------------
		struct Keys
		{
			std::vector<UINT> game;  // virtual keys Dishonored keeps
			UINT              activate{ 'G' };
			UINT              minecraftMenu{ 'O' };
			UINT              toggle{ VK_F11 };
			UINT              depthMode{ VK_F10 };
			std::vector<bool> gameScan = std::vector<bool>(256, false);  // the same keys as scan codes (DirectInput)
			std::vector<UINT> mc;    // bGameDrives: keys that go to Minecraft only
			std::vector<bool> mcScan = std::vector<bool>(256, false);
		};
		Keys keys;

		UINT ParseKey(std::string a_name)
		{
			std::transform(a_name.begin(), a_name.end(), a_name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (a_name.empty()) {
				return 0;
			}
			if (a_name.size() == 1) {
				const char c = a_name[0];
				if ((c >= 'a' && c <= 'z')) {
					return static_cast<UINT>(c - 'a' + 'A');
				}
				if (c >= '0' && c <= '9') {
					return static_cast<UINT>(c);
				}
				if (c == '`' || c == '~') {
					return VK_OEM_3;
				}
			}
			if (a_name[0] == 'f' && a_name.size() <= 3 && std::isdigit(static_cast<unsigned char>(a_name[1]))) {
				const int n = std::atoi(a_name.c_str() + 1);
				if (n >= 1 && n <= 24) {
					return static_cast<UINT>(VK_F1 + n - 1);
				}
			}
			static const std::map<std::string, UINT> named{ { "escape", VK_ESCAPE }, { "esc", VK_ESCAPE }, { "tab", VK_TAB }, { "space", VK_SPACE },
				{ "enter", VK_RETURN }, { "return", VK_RETURN }, { "backspace", VK_BACK }, { "insert", VK_INSERT }, { "delete", VK_DELETE },
				{ "home", VK_HOME }, { "end", VK_END }, { "pageup", VK_PRIOR }, { "pagedown", VK_NEXT }, { "up", VK_UP }, { "down", VK_DOWN },
				{ "left", VK_LEFT }, { "right", VK_RIGHT }, { "shift", VK_SHIFT }, { "ctrl", VK_CONTROL }, { "control", VK_CONTROL },
				{ "alt", VK_MENU }, { "slash", VK_OEM_2 }, { "/", VK_OEM_2 }, { "tilde", VK_OEM_3 }, { "grave", VK_OEM_3 }, { "pause", VK_PAUSE }, { "capslock", VK_CAPITAL } };
			if (const auto it = named.find(a_name); it != named.end()) {
				return it->second;
			}
			DC_WARN("input: unknown key name \"%s\" in DisCraft.ini", a_name.c_str());
			return 0;
		}

		void LoadKeys()
		{
			keys.game.clear();
			for (const auto& name : config::List("Controls", "sGameKeys", "Escape,Tab,F9")) {
				if (const UINT vk = ParseKey(name)) {
					keys.game.push_back(vk);
					const UINT sc = ::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
					if (sc < 256) {
						keys.gameScan[sc] = true;
					}
				}
			}
			keys.mc.clear();
			for (const auto& name : config::List("Controls", "sMinecraftKeys", "E,Q,T,Slash,F1,F3,F4,F5,1,2,3,4,5,6,7,8,9,O")) {
				if (const UINT vk = ParseKey(name)) {
					keys.mc.push_back(vk);
					const UINT sc = ::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
					if (sc < 256) {
						keys.mcScan[sc] = true;
					}
				}
			}
			keys.activate = ParseKey(config::String("Controls", "sActivateKey", "G"));
			keys.minecraftMenu = ParseKey(config::String("Controls", "sMinecraftMenuKey", "O"));
			keys.toggle = ParseKey(config::String("Controls", "sToggleKey", "F11"));
			keys.depthMode = ParseKey(config::String("Controls", "sDepthModeKey", "F10"));
		}

		bool IsMcKey(UINT a_vk) { return std::find(keys.mc.begin(), keys.mc.end(), a_vk) != keys.mc.end(); }
		bool IsGameKey(UINT a_vk) { return std::find(keys.game.begin(), keys.game.end(), a_vk) != keys.game.end(); }

		// ---- state ---------------------------------------------------------------------------------
		std::mutex lookLock;
		float      lookDx = 0.0f, lookDy = 0.0f;
		bool       useRequested = false;
		HWND       window = nullptr;
		WNDPROC    originalProc = nullptr;
		// Keys and buttons come as window messages, unless the game asked Raw Input for none
		// (RIDEV_NOLEGACY): then Raw Input is where they come from. Never both (no double presses).
		bool       rawKeys = false;
		bool       rawButtons = false;
		ULONGLONG  rawFlagsCheckedMs = 0;
		bool       registeredOwnRawInput = false;
		ULONGLONG  routingSince = 0;
		ULONGLONG  lastDeltaMs = 0;
		std::array<bool, 256> heldScan{};  // keys we told Minecraft are down
		wchar_t    highSurrogate = 0;

		enum class Source
		{
			kNone,
			kDirectInput,
			kRaw,
		};
		Source    activeSource = Source::kNone;
		ULONGLONG activeSourceMs = 0;

		bool Routing()
		{
			auto& st = State();
			if (!st.routeInput || st.gameMenuOpen || st.suspended) {
				return false;
			}
			// No game frame for a while (loading, paused without ticking): leave input to the game.
			return ::GetTickCount64() - st.lastTickMs.load() < 300;
		}

		// The game walks its player (bGameDrives): mouse buttons, the wheel and the hotbar keys still
		// go to Minecraft (break, place, pick), the rest stays with the game.
		bool Mirroring()
		{
			auto& st = State();
			return !Routing() && st.mirrorButtons && !st.gameMenuOpen && !st.suspended && !st.mcScreenOpen &&
			       ::GetTickCount64() - st.lastTickMs.load() < 300;
		}

		const void*          activeDevice = nullptr;
		std::array<bool, 8> buttonDown{};

		// a_device: which device (or read path) it came from; only one counts, since the game may
		// read the same mouse several ways (state and buffered data, two device objects).
		void AddDelta(long a_dx, long a_dy, Source a_source, const void* a_device = nullptr)
		{
			if (!a_dx && !a_dy) {
				return;
			}
			const ULONGLONG now = ::GetTickCount64();
			// Only one source counts (the game may read the same mouse two ways).
			if (activeSource != a_source || activeDevice != a_device) {
				if (activeSource != Source::kNone && now - activeSourceMs < 1000) {
					return;
				}
				activeSource = a_source;
				activeDevice = a_device;
				DC_INFO("input: mouse movement from %s", a_source == Source::kRaw ? "Raw Input" : "DirectInput");
			}
			activeSourceMs = now;
			lastDeltaMs = now;
			auto& st = State();
			if (st.mcScreenOpen) {
				const int   sw = std::max(1, st.screenW.load()), ow = st.overlayW, oh = st.overlayH;
				const float scale = float(ow) / float(sw);
				const int   x = std::clamp(st.cursorX.load() + static_cast<int>(std::lround(a_dx * scale)), 0, ow - 1);
				const int   y = std::clamp(st.cursorY.load() + static_cast<int>(std::lround(a_dy * scale)), 0, oh - 1);
				st.cursorX = x;
				st.cursorY = y;
				Link::Get().PushInput(proto::kInCursor, 0, x, y);
				return;
			}
			std::lock_guard guard(lookLock);
			lookDx += static_cast<float>(a_dx);
			lookDy += static_cast<float>(a_dy);
		}

		void SendKey(UINT a_scan, bool a_down)
		{
			a_scan &= 0xFF;
			const auto sdl = kDikToSdl[a_scan];
			if (!sdl) {
				return;
			}
			if (!a_down && !heldScan[a_scan]) {
				return;
			}
			heldScan[a_scan] = a_down;
			Link::Get().PushInput(proto::kInKey, sdl, a_down ? 1 : 0);
		}

		// One key, from window messages or Raw Input. Returns true if it's swallowed (the game
		// doesn't see it).
		bool OnKey(UINT a_vk, UINT a_scan, bool a_down, bool a_repeat)
		{
			auto& st = State();
			if (a_vk == keys.toggle && keys.toggle) {
				if (a_down && !a_repeat && st.installed) {
					st.suspended = !st.suspended;
					ReleaseAll();
					Render::Notify(st.suspended ? L"DisCraft paused: Dishonored has the controls (F11 to go back)" : L"DisCraft: Minecraft has the controls", 4.0f);
				}
				return st.installed;
			}
			if (!Routing()) {
				// Space reaches both: Corvo jumps, and Minecraft sees the double tap that starts flying.
				if (Mirroring() && a_vk == VK_SPACE) {
					SendKey(a_scan, a_down);
					return false;
				}
				if (Mirroring() && IsMcKey(a_vk)) {
					if (a_vk == keys.minecraftMenu && keys.minecraftMenu) {
						if (a_down && !a_repeat) {
							Link::Get().PushInput(proto::kInOpenMenu, 0);
						}
						return true;
					}
					SendKey(a_scan, a_down);
					return true;
				}
				return false;
			}
			if (a_vk == keys.depthMode && keys.depthMode && !st.mcScreenOpen) {
				if (a_down && !a_repeat) {
					Render::CycleDepthMode();
				}
				return true;
			}
			if (!st.mcScreenOpen) {
				if (IsGameKey(a_vk)) {
					if (a_down && !a_repeat) {
						ReleaseAll();
					}
					return false;
				}
				if (a_vk == keys.activate && keys.activate) {
					if (a_down && !a_repeat) {
						useRequested = true;
					}
					return true;
				}
				if (a_vk == keys.minecraftMenu && keys.minecraftMenu) {
					if (a_down && !a_repeat) {
						ReleaseAll();
						Link::Get().PushInput(proto::kInOpenMenu, 0);
					}
					return true;
				}
			}
			SendKey(a_scan, a_down);
			return true;
		}

		bool OnButton(std::uint16_t a_sdlButton, bool a_down)
		{
			if (!Routing() && !Mirroring()) {
				return false;
			}
			// The same click can come from window messages, Raw Input and DirectInput: send changes only.
			if (a_sdlButton < buttonDown.size()) {
				if (buttonDown[a_sdlButton] == a_down) {
					return true;
				}
				buttonDown[a_sdlButton] = a_down;
			}
			static bool loggedClick = false;
			if (!loggedClick && a_down) {
				loggedClick = true;
				DC_INFO("input: first mouse button to Minecraft (%u, %s)", a_sdlButton, Routing() ? "routed" : "mirrored");
			}
			Link::Get().PushInput(proto::kInMouseButton, a_sdlButton, a_down ? 1 : 0);
			return true;
		}

		void OnText(wchar_t a_char)
		{
			if (!State().mcScreenOpen) {
				return;
			}
			if (a_char >= 0xD800 && a_char <= 0xDBFF) {
				highSurrogate = a_char;
				return;
			}
			std::int32_t code = a_char;
			if (a_char >= 0xDC00 && a_char <= 0xDFFF && highSurrogate) {
				code = 0x10000 + ((highSurrogate - 0xD800) << 10) + (a_char - 0xDC00);
			}
			highSurrogate = 0;
			if (code >= 0x20 && code != 0x7F) {
				Link::Get().PushInput(proto::kInText, 0, code);
			}
		}

		UINT ScanOf(LPARAM a_lParam)
		{
			const UINT sc = (static_cast<UINT>(a_lParam) >> 16) & 0xFF;
			return (static_cast<UINT>(a_lParam) & (1u << 24)) ? (sc | 0x80) : sc;
		}

		// Raw Input: mouse deltas always; keys and buttons only if the game asked for no legacy messages.
		void OnRawInput(LPARAM a_lParam)
		{
			UINT size = 0;
			::GetRawInputData(reinterpret_cast<HRAWINPUT>(a_lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
			if (!size || size > 1024) {
				return;
			}
			alignas(8) std::uint8_t buffer[1024];
			if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(a_lParam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) != size) {
				return;
			}
			const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer);
			if (raw->header.dwType == RIM_TYPEMOUSE) {
				const auto& m = raw->data.mouse;
				if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE) && Routing()) {
					AddDelta(m.lLastX, m.lLastY, Source::kRaw);
				}
				if (rawButtons) {
					const USHORT f = m.usButtonFlags;
					if (f & RI_MOUSE_LEFT_BUTTON_DOWN) OnButton(1, true);
					if (f & RI_MOUSE_LEFT_BUTTON_UP) OnButton(1, false);
					if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) OnButton(3, true);
					if (f & RI_MOUSE_RIGHT_BUTTON_UP) OnButton(3, false);
					if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) OnButton(2, true);
					if (f & RI_MOUSE_MIDDLE_BUTTON_UP) OnButton(2, false);
					if ((f & RI_MOUSE_WHEEL) && Routing()) {
						Link::Get().PushInput(proto::kInScroll, 0, static_cast<SHORT>(m.usButtonData));
					}
				}
			} else if (raw->header.dwType == RIM_TYPEKEYBOARD && rawKeys) {
				const auto& k = raw->data.keyboard;
				const UINT  scan = (k.MakeCode & 0x7F) | ((k.Flags & RI_KEY_E0) ? 0x80 : 0);
				const bool  down = !(k.Flags & RI_KEY_BREAK);
				const bool  repeat = down && heldScan[scan & 0xFF];
				OnKey(k.VKey, scan, down, repeat);
			}
		}

		// Which Raw Input registrations this process has (the game's, and ours if we added one).
		void CheckRawRegistrations(bool& a_mouseRegistered)
		{
			a_mouseRegistered = false;
			UINT count = 0;
			::GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE));
			std::vector<RAWINPUTDEVICE> devices(count);
			bool                        keyboardRaw = false, buttonsRaw = false;
			if (count && ::GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE)) != static_cast<UINT>(-1)) {
				for (const auto& d : devices) {
					if (d.usUsagePage != 1) {
						continue;
					}
					if (d.usUsage == 2) {
						a_mouseRegistered = true;
						buttonsRaw |= (d.dwFlags & RIDEV_NOLEGACY) != 0;
					} else if (d.usUsage == 6) {
						keyboardRaw |= (d.dwFlags & RIDEV_NOLEGACY) != 0;
					}
				}
			}
			if (keyboardRaw != rawKeys || buttonsRaw != rawButtons) {
				DC_INFO("input: keys from %s, mouse buttons from %s", keyboardRaw ? "Raw Input" : "window messages",
					buttonsRaw ? "Raw Input" : "window messages");
			}
			rawKeys = keyboardRaw;
			rawButtons = buttonsRaw;
			rawFlagsCheckedMs = ::GetTickCount64();
		}

		LRESULT CALLBACK WindowProc(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam)
		{
			switch (a_msg) {
			case WM_KEYDOWN:
			case WM_SYSKEYDOWN:
			case WM_KEYUP:
			case WM_SYSKEYUP:
				{
					const bool down = a_msg == WM_KEYDOWN || a_msg == WM_SYSKEYDOWN;
					const bool repeat = down && (a_lParam & (1 << 30));
					if (a_wParam == VK_F4 && (::GetKeyState(VK_MENU) & 0x8000)) {
						break;  // Alt+F4 always reaches the game
					}
					if (OnKey(static_cast<UINT>(a_wParam), ScanOf(a_lParam), down, repeat)) {
						return 0;
					}
					break;
				}
			case WM_CHAR:
			case WM_SYSCHAR:
				if (Routing()) {
					OnText(static_cast<wchar_t>(a_wParam));
					return 0;
				}
				break;
			case WM_LBUTTONDOWN:
			case WM_LBUTTONUP:
			case WM_RBUTTONDOWN:
			case WM_RBUTTONUP:
			case WM_MBUTTONDOWN:
			case WM_MBUTTONUP:
			case WM_XBUTTONDOWN:
			case WM_XBUTTONUP:
			case WM_LBUTTONDBLCLK:
			case WM_RBUTTONDBLCLK:
			case WM_MBUTTONDBLCLK:
			case WM_XBUTTONDBLCLK:
				{
					std::uint16_t button = 0;
					bool          down = false;
					switch (a_msg) {
					case WM_LBUTTONDOWN:
					case WM_LBUTTONDBLCLK: button = 1, down = true; break;
					case WM_LBUTTONUP: button = 1; break;
					case WM_RBUTTONDOWN:
					case WM_RBUTTONDBLCLK: button = 3, down = true; break;
					case WM_RBUTTONUP: button = 3; break;
					case WM_MBUTTONDOWN:
					case WM_MBUTTONDBLCLK: button = 2, down = true; break;
					case WM_MBUTTONUP: button = 2; break;
					default:
						button = GET_XBUTTON_WPARAM(a_wParam) == XBUTTON1 ? 4 : 5;
						down = a_msg != WM_XBUTTONUP;
						break;
					}
					if (OnButton(button, down)) {
						return a_msg == WM_XBUTTONDOWN || a_msg == WM_XBUTTONUP || a_msg == WM_XBUTTONDBLCLK ? TRUE : 0;
					}
					break;
				}
			case WM_MOUSEWHEEL:
				if (Routing() || Mirroring()) {
					Link::Get().PushInput(proto::kInScroll, 0, GET_WHEEL_DELTA_WPARAM(a_wParam));
					return 0;
				}
				break;
			case WM_MOUSEMOVE:
				if (Routing()) {
					return 0;
				}
				break;
			case WM_INPUT:
				OnRawInput(a_lParam);
				if (Routing()) {
					return ::DefWindowProcW(a_hwnd, a_msg, a_wParam, a_lParam);
				}
				break;
			case WM_KILLFOCUS:
			case WM_ACTIVATEAPP:
				ReleaseAll();
				break;
			default:
				break;
			}
			return ::CallWindowProcW(originalProc, a_hwnd, a_msg, a_wParam, a_lParam);
		}

		// ---- DirectInput ---------------------------------------------------------------------------
		using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
		using GetDeviceDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
		using GetCapabilitiesFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, LPDIDEVCAPS);

		struct DeviceVtable
		{
			void**            vtable;
			GetDeviceStateFn  getState;
			GetDeviceDataFn   getData;
			GetCapabilitiesFn getCaps;
		};
		std::mutex                       diLock;
		std::vector<DeviceVtable>        diVtables;
		std::unordered_map<void*, int>   diTypes;  // device -> DI8DEVTYPE_*

		const DeviceVtable* VtableOf(void* a_device)
		{
			void** vtable = *reinterpret_cast<void***>(a_device);
			for (const auto& v : diVtables) {
				if (v.vtable == vtable) {
					return &v;
				}
			}
			return nullptr;
		}

		int DeviceType(IDirectInputDevice8W* a_device, const DeviceVtable* a_v)
		{
			if (const auto it = diTypes.find(a_device); it != diTypes.end()) {
				return it->second;
			}
			DIDEVCAPS caps{};
			caps.dwSize = sizeof(caps);
			int type = 0;
			if (SUCCEEDED(a_v->getCaps(a_device, &caps))) {
				type = static_cast<int>(GET_DIDEVICE_TYPE(caps.dwDevType));
			}
			diTypes.emplace(a_device, type);
			return type;
		}

		HRESULT STDMETHODCALLTYPE GetDeviceStateHook(IDirectInputDevice8W* a_device, DWORD a_size, LPVOID a_data)
		{
			const DeviceVtable* v = nullptr;
			{
				std::lock_guard guard(diLock);
				v = VtableOf(a_device);
			}
			if (!v) {
				return DIERR_GENERIC;
			}
			const HRESULT hr = v->getState(a_device, a_size, a_data);
			const bool routing = !FAILED(hr) && a_data && Routing();
			if (FAILED(hr) || !a_data || (!routing && !Mirroring())) {
				return hr;
			}
			int type = 0;
			{
				std::lock_guard guard(diLock);
				type = DeviceType(a_device, v);
			}
			if (!routing) {
				// Mirroring: the game keeps the mouse movement, its buttons go to Minecraft only.
				if (type == DI8DEVTYPE_KEYBOARD && a_size == 256) {
					auto* bytes = static_cast<std::uint8_t*>(a_data);
					for (int k = 0; k < 256; ++k) {
						if (keys.mcScan[static_cast<std::size_t>(k)]) {
							bytes[k] = 0;
						}
					}
				}
				if (type == DI8DEVTYPE_MOUSE && a_size >= sizeof(DIMOUSESTATE)) {
					auto* ms = static_cast<DIMOUSESTATE*>(a_data);
					static constexpr std::uint16_t kSdl[4] = { 1, 3, 2, 4 };
					for (int b = 0; b < 4; ++b) {
						OnButton(kSdl[b], (ms->rgbButtons[b] & 0x80) != 0);
					}
					std::memset(ms->rgbButtons, 0, sizeof(ms->rgbButtons));
				}
				return hr;
			}
			if (type == DI8DEVTYPE_MOUSE && a_size >= sizeof(DIMOUSESTATE)) {
				const auto* ms = static_cast<const DIMOUSESTATE*>(a_data);
				AddDelta(ms->lX, ms->lY, Source::kDirectInput, a_device);
				std::memset(a_data, 0, a_size);
			} else if (type == DI8DEVTYPE_KEYBOARD && a_size == 256) {
				auto* bytes = static_cast<std::uint8_t*>(a_data);
				for (int k = 0; k < 256; ++k) {
					if (!keys.gameScan[static_cast<std::size_t>(k)] || State().mcScreenOpen) {
						bytes[k] = 0;
					}
				}
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE GetDeviceDataHook(IDirectInputDevice8W* a_device, DWORD a_size, LPDIDEVICEOBJECTDATA a_data, LPDWORD a_inOut, DWORD a_flags)
		{
			const DeviceVtable* v = nullptr;
			{
				std::lock_guard guard(diLock);
				v = VtableOf(a_device);
			}
			if (!v) {
				return DIERR_GENERIC;
			}
			const HRESULT hr = v->getData(a_device, a_size, a_data, a_inOut, a_flags);
			if (!FAILED(hr) && a_data && a_inOut && !(a_flags & DIGDD_PEEK) && !Routing() && Mirroring()) {
				// Mirroring: drop the mouse button events, keep everything else for the game.
				int mtype = 0;
				{
					std::lock_guard guard(diLock);
					mtype = DeviceType(a_device, v);
				}
				if (mtype == DI8DEVTYPE_KEYBOARD) {
					DWORD kept = 0;
					auto* bytes = reinterpret_cast<std::uint8_t*>(a_data);
					for (DWORD i = 0; i < *a_inOut; ++i) {
						const auto* e = reinterpret_cast<const DIDEVICEOBJECTDATA*>(bytes + std::size_t(i) * a_size);
						if (e->dwOfs < 256 && keys.mcScan[e->dwOfs]) {
							continue;
						}
						if (kept != i) {
							std::memmove(bytes + std::size_t(kept) * a_size, e, a_size);
						}
						++kept;
					}
					*a_inOut = kept;
				}
				if (mtype == DI8DEVTYPE_MOUSE) {
					DWORD kept = 0;
					auto* bytes = reinterpret_cast<std::uint8_t*>(a_data);
					for (DWORD i = 0; i < *a_inOut; ++i) {
						const auto* e = reinterpret_cast<const DIDEVICEOBJECTDATA*>(bytes + std::size_t(i) * a_size);
						if (e->dwOfs >= DIMOFS_BUTTON0 && e->dwOfs <= DIMOFS_BUTTON7) {
							static constexpr std::uint16_t kSdl[8] = { 1, 3, 2, 4, 5, 0, 0, 0 };
							if (const auto b = kSdl[e->dwOfs - DIMOFS_BUTTON0]) {
								OnButton(b, (e->dwData & 0x80) != 0);
							}
							continue;
						}
						if (kept != i) {
							std::memmove(bytes + std::size_t(kept) * a_size, e, a_size);
						}
						++kept;
					}
					*a_inOut = kept;
				}
				return hr;
			}
			if (FAILED(hr) || !a_data || !a_inOut || !Routing() || (a_flags & DIGDD_PEEK)) {
				return hr;
			}
			int type = 0;
			{
				std::lock_guard guard(diLock);
				type = DeviceType(a_device, v);
			}
			if (type != DI8DEVTYPE_MOUSE && type != DI8DEVTYPE_KEYBOARD) {
				return hr;
			}
			DWORD kept = 0;
			auto* bytes = reinterpret_cast<std::uint8_t*>(a_data);
			for (DWORD i = 0; i < *a_inOut; ++i) {
				auto* e = reinterpret_cast<DIDEVICEOBJECTDATA*>(bytes + std::size_t(i) * a_size);
				if (type == DI8DEVTYPE_MOUSE) {
					if (e->dwOfs == DIMOFS_X) {
						AddDelta(static_cast<long>(e->dwData), 0, Source::kDirectInput, reinterpret_cast<const char*>(a_device) + 1);
					} else if (e->dwOfs == DIMOFS_Y) {
						AddDelta(0, static_cast<long>(e->dwData), Source::kDirectInput, reinterpret_cast<const char*>(a_device) + 1);
					}
					continue;  // the game gets none of it
				}
				if (e->dwOfs < 256 && keys.gameScan[e->dwOfs] && !State().mcScreenOpen) {
					std::memmove(bytes + std::size_t(kept) * a_size, e, a_size);
					++kept;
				}
			}
			*a_inOut = kept;
			return hr;
		}

		void HookDeviceVtable(void* a_device)
		{
			std::lock_guard guard(diLock);
			void**          vtable = *reinterpret_cast<void***>(a_device);
			for (const auto& v : diVtables) {
				if (v.vtable == vtable) {
					return;
				}
			}
			DeviceVtable entry{ vtable, reinterpret_cast<GetDeviceStateFn>(vtable[9]), reinterpret_cast<GetDeviceDataFn>(vtable[10]),
				reinterpret_cast<GetCapabilitiesFn>(vtable[3]) };
			diVtables.push_back(entry);
			mem::WritePointer(&vtable[9], reinterpret_cast<void*>(&GetDeviceStateHook));
			mem::WritePointer(&vtable[10], reinterpret_cast<void*>(&GetDeviceDataHook));
		}
	}

	void InstallDirectInput()
	{
		LoadKeys();
		HMODULE dinput = ::LoadLibraryW(L"dinput8.dll");
		if (!dinput) {
			DC_WARN("input: dinput8.dll not available; DirectInput isn't filtered");
			return;
		}
		using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
		const auto create = reinterpret_cast<CreateFn>(reinterpret_cast<void*>(::GetProcAddress(dinput, "DirectInput8Create")));
		if (!create) {
			return;
		}
		int hooked = 0;
		for (const IID* iid : { &IID_IDirectInput8W, &IID_IDirectInput8A }) {
			IDirectInput8W* di = nullptr;
			if (FAILED(create(::GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, *iid, reinterpret_cast<void**>(&di), nullptr)) || !di) {
				continue;
			}
			for (const GUID* guid : { &GUID_SysMouse, &GUID_SysKeyboard }) {
				IDirectInputDevice8W* device = nullptr;
				if (SUCCEEDED(di->CreateDevice(*guid, &device, nullptr)) && device) {
					HookDeviceVtable(device);
					device->Release();
					++hooked;
				}
			}
			di->Release();
		}
		DC_INFO("input: DirectInput filter installed (%d device interfaces, %zu vtables)", hooked, diVtables.size());
	}

	void Attach(void* a_hwnd)
	{
		window = static_cast<HWND>(a_hwnd);
		originalProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc)));
		DC_INFO("input: attached to the game window");
		// If nothing in the game reads the mouse as Raw Input or DirectInput (we'd see deltas), ask
		// for Raw Input ourselves. Checked from the per-frame update via ConsumeLook.
		routingSince = 0;
	}

	void ConsumeLook(float& a_dx, float& a_dy)
	{
		{
			std::lock_guard guard(lookLock);
			a_dx = lookDx;
			a_dy = lookDy;
			lookDx = lookDy = 0.0f;
		}
		// Routing for 2 s without a single mouse delta from anywhere: register Raw Input ourselves.
		const ULONGLONG now = ::GetTickCount64();
		bool            mouseRegistered = false;
		if (now - rawFlagsCheckedMs > 2000) {
			CheckRawRegistrations(mouseRegistered);
		}
		if (Routing()) {
			if (!routingSince) {
				routingSince = now;
			}
			if (!registeredOwnRawInput && window && now - routingSince > 2000 && now - lastDeltaMs > 2000) {
				CheckRawRegistrations(mouseRegistered);
				if (!mouseRegistered) {
					RAWINPUTDEVICE mouse{ 1, 2, 0, window };
					registeredOwnRawInput = ::RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) != FALSE;
					DC_INFO("input: no mouse movement seen; registered Raw Input for the mouse (%s)", registeredOwnRawInput ? "ok" : "failed");
				} else {
					registeredOwnRawInput = true;  // the game has it; nothing to add
				}
			}
		} else {
			routingSince = 0;
		}
	}

	void ReleaseAll()
	{
		heldScan.fill(false);
		buttonDown.fill(false);
		Link::Get().PushInput(proto::kInReleaseAll, 0);
	}

	bool TakeUseRequest()
	{
		const bool r = useRequested;
		useRequested = false;
		return r;
	}
}
