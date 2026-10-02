#pragma once

#include <cstdarg>
#include <string>

// A small thread-safe file log: %LOCALAPPDATA%\DisCraft\DisCraft.log (stderr in host tests).
namespace discraft::log
{
	void Open();
	void Write(const char* a_level, const char* a_fmt, ...);
	std::string Format(const char* a_fmt, ...);
	std::string FormatV(const char* a_fmt, va_list a_args);
	// [Debug] bDiagnostics: the detailed per-frame logs. Off by default.
	bool Diagnostics();
	void SetDiagnostics(bool a_on);
}

#define DC_INFO(...) ::discraft::log::Write("info", __VA_ARGS__)
#define DC_WARN(...) ::discraft::log::Write("warn", __VA_ARGS__)
#define DC_ERROR(...) ::discraft::log::Write("error", __VA_ARGS__)
#define DC_DIAG(...)                                     \
	do {                                                 \
		if (::discraft::log::Diagnostics()) {            \
			::discraft::log::Write("diag", __VA_ARGS__); \
		}                                                \
	} while (0)
