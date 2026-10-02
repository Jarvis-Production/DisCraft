#pragma once

#include <string>
#include <vector>

// DisCraft.ini, next to the plugin (Dishonored\Binaries\Win32\DisCraft.ini). Read once at start.
namespace discraft::config
{
	void Load();
	// Folder the plugin was loaded from (with a trailing backslash).
	std::wstring PluginDir();
	bool Found();

	std::string        String(const char* a_section, const char* a_key, const char* a_default);
	int                Int(const char* a_section, const char* a_key, int a_default);
	float              Float(const char* a_section, const char* a_key, float a_default);
	bool               Bool(const char* a_section, const char* a_key, bool a_default);
	// Comma-separated values, trimmed, empty ones dropped.
	std::vector<std::string> List(const char* a_section, const char* a_key, const char* a_default);

	std::wstring Widen(const std::string& a_utf8);
	std::string  Narrow(const std::wstring& a_wide);
}
