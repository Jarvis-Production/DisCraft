#include "Config.h"
#include "Game.h"
#include "Link.h"
#include "Log.h"
#include "ue3/UE3.h"

#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// DisCraft.asi: loaded into Dishonored.exe by an ASI loader at startup.
//
// The init thread sets up what must exist before the game gets going (shared memory, Minecraft's
// launch, the Direct3D 9 and DirectInput hooks), waits for the game's window, and then asks the
// game's own thread (through window messages, which the engine pumps between frames) to find the
// engine's data and install the per-frame hook. Nothing touches engine objects off that thread.
namespace
{
	constexpr const char* kVersion = "0.1.0";
	constexpr UINT        kMsgInstall = WM_APP + 0x4443;
	constexpr UINT        kMsgMaintain = WM_APP + 0x4444;
	constexpr int         kDiscoveryAttempts = 90;  // one a second

	HWND    gameWindow = nullptr;
	WNDPROC chained = nullptr;
	int     attempts = 0;
	bool    gaveUp = false;

	void Install()
	{
		auto& st = discraft::State();
		if (st.installed || gaveUp) {
			return;
		}
		if (!st.discovered) {
			if (!discraft::ue3::Discover()) {
				if (++attempts >= kDiscoveryAttempts) {
					gaveUp = true;
					discraft::Game::SetDiscoveryFailed(true);
					DC_ERROR("UE3: gave up looking for the engine's data after %d attempts", attempts);
				}
				return;
			}
			st.discovered = true;
		}
		if (!discraft::Game::Install() && ++attempts >= kDiscoveryAttempts) {
			gaveUp = true;
			discraft::Game::SetDiscoveryFailed(true);
			DC_ERROR("couldn't install into the game (see above)");
		}
	}

	LRESULT CALLBACK MainProc(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam)
	{
		if (a_msg == kMsgInstall) {
			Install();
			return 0;
		}
		if (a_msg == kMsgMaintain) {
			if (discraft::State().installed) {
				discraft::Game::Install();  // hooks overrides from packages loaded since
			}
			return 0;
		}
		return ::CallWindowProcW(chained, a_hwnd, a_msg, a_wParam, a_lParam);
	}

	struct Search
	{
		DWORD pid;
		HWND  best;
		long  bestArea;
	};

	BOOL CALLBACK EnumProc(HWND a_hwnd, LPARAM a_param)
	{
		auto* s = reinterpret_cast<Search*>(a_param);
		DWORD pid = 0;
		::GetWindowThreadProcessId(a_hwnd, &pid);
		if (pid != s->pid || !::IsWindowVisible(a_hwnd) || ::GetWindow(a_hwnd, GW_OWNER)) {
			return TRUE;
		}
		wchar_t cls[128]{};
		::GetClassNameW(a_hwnd, cls, 128);
		RECT r{};
		::GetClientRect(a_hwnd, &r);
		long area = (r.right - r.left) * (r.bottom - r.top);
		if (std::wstring(cls).find(L"Unreal") != std::wstring::npos) {
			area += 1 << 30;  // UE3's own viewport window wins
		}
		if (area > s->bestArea) {
			s->best = a_hwnd;
			s->bestArea = area;
		}
		return TRUE;
	}

	HWND FindGameWindow()
	{
		Search s{ ::GetCurrentProcessId(), nullptr, 64 * 64 };
		::EnumWindows(&EnumProc, reinterpret_cast<LPARAM>(&s));
		return s.best;
	}

	void Attach(HWND a_window)
	{
		gameWindow = a_window;
		discraft::Input::Attach(a_window);
		chained = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(a_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MainProc)));
		wchar_t cls[128]{};
		::GetClassNameW(a_window, cls, 128);
		DC_INFO("game window found (class %s)", discraft::config::Narrow(cls).c_str());
	}

	DWORD WINAPI InitThread(LPVOID)
	{
		using namespace discraft;
		log::Open();
		config::Load();
		log::SetDiagnostics(config::Bool("Debug", "bDiagnostics", false));
		wchar_t exe[MAX_PATH]{};
		::GetModuleFileNameW(nullptr, exe, MAX_PATH);
		DC_INFO("DisCraft %s loading into %s (DisCraft.ini %s)", kVersion, config::Narrow(exe).c_str(), config::Found() ? "found" : "not found: defaults");
		if (!Link::Get().Create()) {
			DC_ERROR("DisCraft disabled: could not create shared memory");
			return 0;
		}
		Launcher::StartMinecraft();
		Render::InstallEarly();
		Input::InstallDirectInput();

		for (;;) {
			if (!gameWindow || !::IsWindow(gameWindow)) {
				if (HWND w = FindGameWindow()) {
					Attach(w);
				}
			}
			if (gameWindow) {
				if (!State().installed && !gaveUp) {
					::PostMessageW(gameWindow, kMsgInstall, 0, 0);
					::Sleep(1000);
				} else {
					::PostMessageW(gameWindow, kMsgMaintain, 0, 0);
					::Sleep(5000);
				}
			} else {
				::Sleep(250);
			}
		}
	}
}

BOOL APIENTRY DllMain(HMODULE a_module, DWORD a_reason, LPVOID)
{
	if (a_reason == DLL_PROCESS_ATTACH) {
		::DisableThreadLibraryCalls(a_module);
		if (HANDLE thread = ::CreateThread(nullptr, 0, &InitThread, nullptr, 0, nullptr)) {
			::CloseHandle(thread);
		}
	}
	return TRUE;
}
