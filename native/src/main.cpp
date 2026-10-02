#include "Config.h"
#include "Game.h"
#include "Link.h"
#include "Log.h"
#include "Seh.h"
#include "ue3/UE3.h"

#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// DisCraft.asi: loaded into Dishonored.exe by an ASI loader at startup.
//
// The init thread sets up what must exist before the game gets going (shared memory, Minecraft's
// launch, the Direct3D 9 and DirectInput hooks), waits for the game's window, and then asks the
// game's own thread (through messages posted to its window, which the engine pumps between frames)
// to find the engine's data and install the per-frame hook. Nothing touches engine objects off that
// thread. The messages are caught by a WH_GETMESSAGE hook on that thread rather than in the window
// procedure: the game (or Direct3D, or an overlay) may replace the window procedure later.
namespace
{
	constexpr const char* kVersion = "0.1.0";
	constexpr UINT        kMsgInstall = WM_APP + 0x4443;
	constexpr UINT        kMsgMaintain = WM_APP + 0x4444;
	constexpr UINT        kMsgPing = WM_APP + 0x4445;
	constexpr LRESULT     kPong = 0x4443D15C;
	constexpr int         kDiscoveryAttempts = 90;  // one a second

	HMODULE self = nullptr;
	HWND    gameWindow = nullptr;
	WNDPROC chained = nullptr;
	HHOOK   messageHook = nullptr;
	DWORD   hookedThread = 0;
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
		discraft::Game::Install();  // retried until it works: the frame update may come with a later package
	}

	void Maintain()
	{
		if (discraft::State().installed) {
			discraft::Game::Install();  // hooks overrides from packages loaded since
		}
	}

	LRESULT CALLBACK GetMessageHook(int a_code, WPARAM a_remove, LPARAM a_msg)
	{
		if (a_code == HC_ACTION && a_remove == PM_REMOVE) {
			auto* msg = reinterpret_cast<MSG*>(a_msg);
			if (msg->message == kMsgInstall) {
				msg->message = WM_NULL;
				Install();
			} else if (msg->message == kMsgMaintain) {
				msg->message = WM_NULL;
				Maintain();
			}
		}
		return ::CallNextHookEx(nullptr, a_code, a_remove, a_msg);
	}

	LRESULT CALLBACK MainProc(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam)
	{
		if (a_msg == kMsgPing) {
			return kPong;
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

	void Subclass(HWND a_window)
	{
		discraft::Input::Attach(a_window);
		chained = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(a_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MainProc)));
	}

	void Attach(HWND a_window)
	{
		gameWindow = a_window;
		Subclass(a_window);
		const DWORD thread = ::GetWindowThreadProcessId(a_window, nullptr);
		if (thread != hookedThread) {
			if (messageHook) {
				::UnhookWindowsHookEx(messageHook);
			}
			messageHook = ::SetWindowsHookExW(WH_GETMESSAGE, &GetMessageHook, self, thread);
			hookedThread = messageHook ? thread : 0;
			if (!messageHook) {
				DC_ERROR("couldn't hook the game thread's messages (error %lu)", ::GetLastError());
			}
		}
		wchar_t cls[128]{};
		::GetClassNameW(a_window, cls, 128);
		DC_INFO("game window found (class %s)", discraft::config::Narrow(cls).c_str());
	}

	// Something replaced the game window's procedure without passing messages on to ours (input
	// would stop reaching DisCraft): subclass again. Only when the window's thread answers at all.
	void CheckSubclass()
	{
		DWORD_PTR answer = 0;
		if (!::SendMessageTimeoutW(gameWindow, kMsgPing, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &answer)) {
			return;
		}
		if (static_cast<LRESULT>(answer) != kPong) {
			DC_WARN("the game window's procedure was replaced; attaching again");
			Subclass(gameWindow);
		}
	}

	// Minecraft takes a silent Dishonored for a closed one. Beat from our own thread, so loading
	// screens, menus and a missing per-frame hook don't look like the game quitting.
	DWORD WINAPI HeartbeatThread(LPVOID)
	{
		for (;;) {
			discraft::Link::Get().Heartbeat();
			::Sleep(500);
		}
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
		if (config::Bool("Debug", "bCrashLog", true)) {
			seh::InstallCrashLog();
		}
		if (!Link::Get().Create()) {
			DC_ERROR("DisCraft disabled: could not create shared memory");
			return 0;
		}
		if (HANDLE beat = ::CreateThread(nullptr, 0, &HeartbeatThread, nullptr, 0, nullptr)) {
			::CloseHandle(beat);
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
				CheckSubclass();
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
		self = a_module;
		if (HANDLE thread = ::CreateThread(nullptr, 0, &InitThread, nullptr, 0, nullptr)) {
			::CloseHandle(thread);
		}
	}
	return TRUE;
}
