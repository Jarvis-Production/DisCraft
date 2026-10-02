#include "Seh.h"

#include <cstdio>

#ifdef _WIN32
#	define WIN32_LEAN_AND_MEAN
#	include <windows.h>
#endif

namespace discraft::seh
{
#ifdef _MSC_VER
	namespace
	{
		int Filter(EXCEPTION_POINTERS* a_info, Fault* a_fault)
		{
			const auto* record = a_info->ExceptionRecord;
			a_fault->code = record->ExceptionCode;
			a_fault->address = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
			if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
				a_fault->access = static_cast<int>(record->ExceptionInformation[0]);
				a_fault->data = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
			}
			return EXCEPTION_EXECUTE_HANDLER;
		}
	}

	bool Run(Fn a_fn, void* a_context, Fault& a_fault)
	{
		Fault* fault = &a_fault;
		__try {
			a_fn(a_context);
			return true;
		} __except (Filter(GetExceptionInformation(), fault)) {
			return false;
		}
	}

	bool Guarded() { return true; }
#else
	bool Run(Fn a_fn, void* a_context, Fault&)
	{
		a_fn(a_context);
		return true;
	}

	bool Guarded() { return false; }
#endif

	std::string Where(std::uintptr_t a_address)
	{
		char buf[320];
#ifdef _WIN32
		HMODULE module = nullptr;
		if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(a_address), &module) &&
			module) {
			char path[MAX_PATH]{};
			::GetModuleFileNameA(module, path, MAX_PATH);
			const char* name = path;
			for (const char* p = path; *p; ++p) {
				if (*p == '\\' || *p == '/') {
					name = p + 1;
				}
			}
			std::snprintf(buf, sizeof(buf), "%s+0x%X", name, static_cast<unsigned>(a_address - reinterpret_cast<std::uintptr_t>(module)));
			return buf;
		}
#endif
		std::snprintf(buf, sizeof(buf), "0x%08X (no module)", static_cast<unsigned>(a_address));
		return buf;
	}

	std::string Describe(const Fault& a_fault)
	{
		char what[160];
		if (a_fault.code == 0xC0000005u) {
			const char* how = a_fault.access == 1 ? "writing" : a_fault.access == 8 ? "executing" : "reading";
			std::snprintf(what, sizeof(what), "access violation %s 0x%08X", how, static_cast<unsigned>(a_fault.data));
		} else {
			std::snprintf(what, sizeof(what), "exception 0x%08X", static_cast<unsigned>(a_fault.code));
		}
		return std::string(what) + " at " + Where(a_fault.address);
	}
}
