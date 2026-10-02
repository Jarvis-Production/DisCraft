#include "Config.h"

#include <cstdlib>

#ifdef _WIN32
#	define WIN32_LEAN_AND_MEAN
#	include <windows.h>
#endif

namespace discraft::config
{
	namespace
	{
		std::wstring dir;
		std::wstring iniPath;
		bool         found = false;

		std::string Trim(std::string a_s)
		{
			const auto notSpace = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
			while (!a_s.empty() && !notSpace(static_cast<unsigned char>(a_s.back()))) {
				a_s.pop_back();
			}
			std::size_t i = 0;
			while (i < a_s.size() && !notSpace(static_cast<unsigned char>(a_s[i]))) {
				++i;
			}
			return a_s.substr(i);
		}
	}

	std::wstring Widen(const std::string& a_utf8)
	{
#ifdef _WIN32
		if (a_utf8.empty()) {
			return {};
		}
		const int n = ::MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()), nullptr, 0);
		std::wstring out(static_cast<std::size_t>(n), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()), out.data(), n);
		return out;
#else
		return std::wstring(a_utf8.begin(), a_utf8.end());
#endif
	}

	std::string Narrow(const std::wstring& a_wide)
	{
#ifdef _WIN32
		if (a_wide.empty()) {
			return {};
		}
		const int n = ::WideCharToMultiByte(CP_UTF8, 0, a_wide.data(), static_cast<int>(a_wide.size()), nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<std::size_t>(n), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, a_wide.data(), static_cast<int>(a_wide.size()), out.data(), n, nullptr, nullptr);
		return out;
#else
		std::string out;
		for (wchar_t c : a_wide) {
			out.push_back(static_cast<char>(c));
		}
		return out;
#endif
	}

	void Load()
	{
#ifdef _WIN32
		HMODULE self = nullptr;
		::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&Load), &self);
		wchar_t path[MAX_PATH]{};
		::GetModuleFileNameW(self, path, MAX_PATH);
		dir = path;
		const auto slash = dir.find_last_of(L"\\/");
		dir = slash == std::wstring::npos ? L".\\" : dir.substr(0, slash + 1);
		iniPath = dir + L"DisCraft.ini";
		found = ::GetFileAttributesW(iniPath.c_str()) != INVALID_FILE_ATTRIBUTES;
#endif
	}

	std::wstring PluginDir() { return dir; }
	bool         Found() { return found; }

	std::string String(const char* a_section, const char* a_key, const char* a_default)
	{
#ifdef _WIN32
		if (found) {
			wchar_t buf[1024]{};
			const auto def = Widen(a_default);
			::GetPrivateProfileStringW(Widen(a_section).c_str(), Widen(a_key).c_str(), def.c_str(), buf, 1024, iniPath.c_str());
			auto value = Narrow(buf);
			// Inline comments: "value ; comment".
			if (const auto semi = value.find(';'); semi != std::string::npos) {
				value = value.substr(0, semi);
			}
			return Trim(value);
		}
#else
		(void)a_section;
		(void)a_key;
#endif
		return a_default;
	}

	int Int(const char* a_section, const char* a_key, int a_default)
	{
		const auto s = String(a_section, a_key, "");
		if (s.empty()) {
			return a_default;
		}
		return static_cast<int>(std::strtol(s.c_str(), nullptr, 0));
	}

	float Float(const char* a_section, const char* a_key, float a_default)
	{
		const auto s = String(a_section, a_key, "");
		if (s.empty()) {
			return a_default;
		}
		return std::strtof(s.c_str(), nullptr);
	}

	bool Bool(const char* a_section, const char* a_key, bool a_default)
	{
		const auto s = String(a_section, a_key, "");
		if (s.empty()) {
			return a_default;
		}
		return s == "1" || s == "true" || s == "True" || s == "TRUE" || s == "yes" || s == "on";
	}

	std::vector<std::string> List(const char* a_section, const char* a_key, const char* a_default)
	{
		std::vector<std::string> out;
		const auto               s = String(a_section, a_key, a_default);
		std::size_t              start = 0;
		while (start <= s.size()) {
			auto end = s.find(',', start);
			if (end == std::string::npos) {
				end = s.size();
			}
			auto item = Trim(s.substr(start, end - start));
			if (!item.empty()) {
				out.push_back(std::move(item));
			}
			start = end + 1;
		}
		return out;
	}
}
