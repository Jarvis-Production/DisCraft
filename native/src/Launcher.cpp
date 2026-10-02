#include "Game.h"

#include "Config.h"
#include "Log.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <exdisp.h>
#include <servprov.h>
#include <shldisp.h>
#include <shlguid.h>
#include <shlobj.h>

// Minecraft starts with Dishonored. The DisCraft Fabric mod then waits on its title screen until the
// game is in a level, hides its own window, opens the DisCraft world by itself, and quits again when
// this Dishonored closes. What to start comes from DisCraft.ini; by default Prism Launcher's
// "DisCraft" instance from the bundle next to the plugin (Prism signs in to the player's Microsoft
// account). Ported from SkyCraft's Launcher (MIT).
namespace discraft::Launcher
{
	namespace
	{
		std::atomic<Status> status{ Status::kOff };

		std::wstring ExpandEnv(const std::wstring& a_path)
		{
			wchar_t     buf[MAX_PATH * 2];
			const DWORD n = ::ExpandEnvironmentStringsW(a_path.c_str(), buf, static_cast<DWORD>(std::size(buf)));
			return n > 0 && n <= std::size(buf) ? std::wstring(buf) : a_path;
		}

		std::filesystem::path InstallDir() { return ExpandEnv(L"%LOCALAPPDATA%\\DisCraft"); }
		std::filesystem::path Bundle() { return std::filesystem::path(config::PluginDir()) / L"DisCraft" / L"DisCraft-Minecraft.zip"; }

		template <class T>
		void Release(T*& a_p)
		{
			if (a_p) {
				a_p->Release();
				a_p = nullptr;
			}
		}

		// Runs a program the way double-clicking it would: started by the desktop's Explorer, not by
		// the game, so Minecraft isn't a child of Dishonored (Steam's job object, overlays, ...).
		bool OpenFromDesktop(const std::wstring& a_file, const std::wstring& a_args, const std::wstring& a_dir, int a_show)
		{
			IShellWindows*        windows = nullptr;
			IDispatch*            desktop = nullptr;
			IServiceProvider*     services = nullptr;
			IShellBrowser*        browser = nullptr;
			IShellView*           view = nullptr;
			IDispatch*            background = nullptr;
			IShellFolderViewDual* folderView = nullptr;
			IDispatch*            application = nullptr;
			IShellDispatch2*      shell = nullptr;
			bool                  ok = false;
			do {
				if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_IShellWindows, reinterpret_cast<void**>(&windows)))) {
					break;
				}
				VARIANT location{};
				location.vt = VT_I4;
				location.lVal = CSIDL_DESKTOP;
				VARIANT empty{};
				long    hwnd = 0;
				if (FAILED(windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop)) || !desktop) {
					break;
				}
				if (FAILED(desktop->QueryInterface(IID_IServiceProvider, reinterpret_cast<void**>(&services))) ||
					FAILED(services->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, reinterpret_cast<void**>(&browser))) ||
					FAILED(browser->QueryActiveShellView(&view)) ||
					FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_IDispatch, reinterpret_cast<void**>(&background))) ||
					FAILED(background->QueryInterface(IID_IShellFolderViewDual, reinterpret_cast<void**>(&folderView))) ||
					FAILED(folderView->get_Application(&application)) ||
					FAILED(application->QueryInterface(IID_IShellDispatch2, reinterpret_cast<void**>(&shell)))) {
					break;
				}
				BSTR    file = ::SysAllocString(a_file.c_str());
				VARIANT args{}, dir{}, operation{}, show{};
				args.vt = dir.vt = operation.vt = VT_BSTR;
				args.bstrVal = ::SysAllocString(a_args.c_str());
				dir.bstrVal = ::SysAllocString(a_dir.c_str());
				operation.bstrVal = ::SysAllocString(L"open");
				show.vt = VT_I4;
				show.lVal = a_show;
				ok = SUCCEEDED(shell->ShellExecute(file, args, dir, operation, show));
				::SysFreeString(file);
				::VariantClear(&args);
				::VariantClear(&dir);
				::VariantClear(&operation);
			} while (false);
			Release(shell);
			Release(application);
			Release(folderView);
			Release(background);
			Release(view);
			Release(browser);
			Release(services);
			Release(desktop);
			Release(windows);
			return ok;
		}

		std::filesystem::path FindPrism()
		{
			for (const wchar_t* candidate : { L"%LOCALAPPDATA%\\Programs\\PrismLauncher\\prismlauncher.exe", L"%ProgramFiles%\\PrismLauncher\\prismlauncher.exe" }) {
				std::filesystem::path p = ExpandEnv(candidate);
				if (std::filesystem::exists(p)) {
					return p;
				}
			}
			return {};
		}

		// Unpacks the bundle to %LOCALAPPDATA%\DisCraft the first time, and again whenever this
		// DisCraft brings a different one. Prism's own data there (the signed-in account, downloaded
		// Minecraft and Java, the DisCraft world) is kept; the instance and its mod jars are replaced.
		std::filesystem::path EnsureBundle()
		{
			const auto      dir = InstallDir();
			const auto      prism = dir / "Prism" / "prismlauncher.exe";
			const auto      bundle = Bundle();
			std::error_code ec;
			const auto      stamp = std::to_string(std::filesystem::file_size(bundle, ec)) + " " +
			                   std::to_string(std::filesystem::last_write_time(bundle, ec).time_since_epoch().count());
			std::string installed;
			if (std::ifstream in{ dir / "bundle.stamp" }; in) {
				std::getline(in, installed);
			}
			if (installed == stamp && std::filesystem::exists(prism)) {
				return prism;
			}
			DC_INFO("Minecraft: unpacking DisCraft's Minecraft to %s", dir.string().c_str());
			std::filesystem::create_directories(dir, ec);
			for (const auto& entry : std::filesystem::directory_iterator(dir / "Prism" / "instances" / "DisCraft" / ".minecraft" / "mods", ec)) {
				const auto name = entry.path().filename().string();
				if (name.rfind("discraft-", 0) == 0 || name.rfind("fabric-api-", 0) == 0 || name.rfind("e4mc-", 0) == 0) {
					std::filesystem::remove(entry.path(), ec);
				}
			}
			const auto copy = dir / "bundle.zip";
			if (!std::filesystem::copy_file(bundle, copy, std::filesystem::copy_options::overwrite_existing, ec)) {
				DC_WARN("Minecraft: couldn't copy %s (%s)", bundle.string().c_str(), ec.message().c_str());
				return {};
			}
			std::wstring command = L"\"" + ExpandEnv(L"%SystemRoot%\\System32\\tar.exe") + L"\" -xf \"" + copy.wstring() + L"\" -C \"" + dir.wstring() + L"\"";
			STARTUPINFOW        si{};
			si.cb = sizeof(si);
			PROCESS_INFORMATION pi{};
			DWORD               code = 1;
			if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
				::WaitForSingleObject(pi.hProcess, 5 * 60 * 1000);
				::GetExitCodeProcess(pi.hProcess, &code);
				::CloseHandle(pi.hThread);
				::CloseHandle(pi.hProcess);
			}
			std::filesystem::remove(copy, ec);
			if (code != 0 || !std::filesystem::exists(prism)) {
				DC_WARN("Minecraft: unpacking failed (tar exit code %lu)", code);
				return {};
			}
			const auto cfg = dir / "Prism" / "prismlauncher.cfg";
			if (!std::filesystem::exists(cfg)) {
				std::filesystem::copy_file(dir / "defaults" / "prismlauncher.cfg", cfg, ec);
			}
			std::ofstream(dir / "bundle.stamp") << stamp;
			return prism;
		}

		bool Start(const std::filesystem::path& a_program, const std::wstring& a_args)
		{
			const auto         ext = a_program.extension().wstring();
			const bool         script = _wcsicmp(ext.c_str(), L".bat") == 0 || _wcsicmp(ext.c_str(), L".cmd") == 0;
			const std::wstring dir = a_program.parent_path().wstring();
			const HRESULT      com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			const bool         viaDesktop = OpenFromDesktop(a_program.wstring(), a_args, dir, script ? SW_HIDE : SW_SHOWNORMAL);
			if (SUCCEEDED(com)) {
				::CoUninitialize();
			}
			if (viaDesktop) {
				DC_INFO("Minecraft: started %s", a_program.string().c_str());
				return true;
			}
			std::wstring command = script ? L"cmd.exe /c \"\"" + a_program.wstring() + L"\" " + a_args + L"\"" : L"\"" + a_program.wstring() + L"\" " + a_args;
			STARTUPINFOW        si{};
			si.cb = sizeof(si);
			PROCESS_INFORMATION pi{};
			if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, (script ? CREATE_NO_WINDOW : 0) | CREATE_BREAKAWAY_FROM_JOB, nullptr,
					dir.c_str(), &si, &pi) &&
				!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, script ? CREATE_NO_WINDOW : 0, nullptr, dir.c_str(), &si, &pi)) {
				DC_WARN("Minecraft: couldn't start %s (error %lu)", a_program.string().c_str(), ::GetLastError());
				return false;
			}
			::CloseHandle(pi.hThread);
			::CloseHandle(pi.hProcess);
			DC_INFO("Minecraft: started %s (directly)", a_program.string().c_str());
			return true;
		}
	}

	Status GetStatus() { return status.load(); }

	// A Minecraft with the DisCraft mod holds this mutex while it runs (DisLink.announceRunning).
	bool MinecraftRunning()
	{
		HANDLE mutex = ::OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\DisCraft_v1_minecraft");
		if (mutex) {
			::CloseHandle(mutex);
			return true;
		}
		return false;
	}

	bool PrismRunning()
	{
		HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) {
			return false;
		}
		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);
		bool found = false;
		for (BOOL more = ::Process32FirstW(snapshot, &entry); more && !found; more = ::Process32NextW(snapshot, &entry)) {
			found = _wcsicmp(entry.szExeFile, L"prismlauncher.exe") == 0;
		}
		::CloseHandle(snapshot);
		return found;
	}

	void StartMinecraft()
	{
		if (!config::Bool("Minecraft", "bStartWithGame", true)) {
			DC_INFO("Minecraft: not started with the game (bStartWithGame = 0)");
			status = Status::kOff;
			return;
		}
		if (MinecraftRunning()) {
			DC_INFO("Minecraft: already running");
			status = Status::kRunning;
			// It may be the last game's Minecraft on its way out: if it goes in the next minute,
			// start one for this game.
			std::thread([] {
				for (int i = 0; i < 60; ++i) {
					std::this_thread::sleep_for(std::chrono::seconds(1));
					if (!MinecraftRunning()) {
						DC_INFO("Minecraft: the one that was running has quit; starting another");
						std::this_thread::sleep_for(std::chrono::seconds(3));
						StartMinecraft();
						return;
					}
				}
			}).detach();
			return;
		}
		const std::filesystem::path chosen = ExpandEnv(config::Widen(config::String("Minecraft", "sLauncher", "")));
		const std::wstring          args = config::Widen(config::String("Minecraft", "sArguments", "--launch DisCraft"));
		const bool                  bundled = chosen.empty() && std::filesystem::exists(Bundle());
		const std::filesystem::path installed = chosen.empty() && !bundled ? FindPrism() : std::filesystem::path{};
		if (chosen.empty() && !bundled && installed.empty()) {
			DC_WARN("Minecraft: not started: no DisCraft\\DisCraft-Minecraft.zip next to the plugin and no Prism Launcher installed; set sLauncher in DisCraft.ini");
			status = Status::kNoLauncher;
			return;
		}
		if (!chosen.empty() && !std::filesystem::exists(chosen)) {
			DC_WARN("Minecraft: not started: %s doesn't exist (sLauncher in DisCraft.ini)", chosen.string().c_str());
			status = Status::kNoLauncher;
			return;
		}
		status = Status::kStarting;
		std::thread([chosen, installed, bundled, args] {
			std::filesystem::path program = !chosen.empty() ? chosen : installed;
			if (bundled) {
				program = EnsureBundle();
				if (program.empty()) {
					status = Status::kFailed;
					return;
				}
				if (!std::filesystem::exists(program.parent_path() / "accounts.json")) {
					status = Status::kSignIn;  // first time: Prism asks for the Microsoft account
				}
			}
			if (!Start(program, args)) {
				status = Status::kFailed;
			}
		}).detach();
	}
}
