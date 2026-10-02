#include "Log.h"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <vector>

#ifdef _WIN32
#	define WIN32_LEAN_AND_MEAN
#	include <windows.h>
#endif

namespace discraft::log
{
	namespace
	{
		std::mutex        lock;
		std::FILE*        file = nullptr;
		std::atomic<bool> diagnostics{ false };
	}

	void Open()
	{
		std::lock_guard guard(lock);
		if (file) {
			return;
		}
#ifdef _WIN32
		wchar_t base[MAX_PATH]{};
		const DWORD n = ::ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\DisCraft", base, MAX_PATH);
		if (n > 0 && n <= MAX_PATH) {
			::CreateDirectoryW(base, nullptr);
			std::wstring path = std::wstring(base) + L"\\DisCraft.log";
			file = ::_wfopen(path.c_str(), L"w");
		}
#endif
		if (!file) {
			file = stderr;
		}
	}

	std::string FormatV(const char* a_fmt, va_list a_args)
	{
		va_list copy;
		va_copy(copy, a_args);
		const int n = std::vsnprintf(nullptr, 0, a_fmt, copy);
		va_end(copy);
		if (n <= 0) {
			return {};
		}
		std::string out(static_cast<std::size_t>(n), '\0');
		std::vsnprintf(out.data(), out.size() + 1, a_fmt, a_args);
		return out;
	}

	std::string Format(const char* a_fmt, ...)
	{
		va_list args;
		va_start(args, a_fmt);
		auto out = FormatV(a_fmt, args);
		va_end(args);
		return out;
	}

	void Write(const char* a_level, const char* a_fmt, ...)
	{
		va_list args;
		va_start(args, a_fmt);
		const auto text = FormatV(a_fmt, args);
		va_end(args);

		char stamp[32]{};
#ifdef _WIN32
		SYSTEMTIME t;
		::GetLocalTime(&t);
		std::snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
#else
		const std::time_t now = std::time(nullptr);
		std::strftime(stamp, sizeof(stamp), "%H:%M:%S", std::localtime(&now));
#endif
		std::lock_guard guard(lock);
		std::FILE* out = file ? file : stderr;
		std::fprintf(out, "[%s] [%s] %s\n", stamp, a_level, text.c_str());
		std::fflush(out);
	}

	bool Diagnostics() { return diagnostics.load(std::memory_order_relaxed); }
	void SetDiagnostics(bool a_on) { diagnostics = a_on; }
}
