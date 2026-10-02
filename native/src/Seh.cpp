#include "Seh.h"

#include "Log.h"

#include <algorithm>
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

	namespace
	{
		const char* volatile activity = nullptr;
	}

	void SetActivity(const char* a_what) { activity = a_what; }

#ifdef _WIN32
	namespace
	{
		bool IsCode(std::uintptr_t a_address)
		{
			MEMORY_BASIC_INFORMATION info{};
			if (!::VirtualQuery(reinterpret_cast<LPCVOID>(a_address), &info, sizeof(info)) || info.State != MEM_COMMIT || info.Type != MEM_IMAGE) {
				return false;
			}
			return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
		}

		volatile LONG crashesLogged = 0;

		LONG CALLBACK FirstChance(EXCEPTION_POINTERS* a_info)
		{
			const DWORD code = a_info->ExceptionRecord->ExceptionCode;
			switch (code) {
			case EXCEPTION_ACCESS_VIOLATION:
			case EXCEPTION_ILLEGAL_INSTRUCTION:
			case EXCEPTION_PRIV_INSTRUCTION:
			case EXCEPTION_INT_DIVIDE_BY_ZERO:
			case EXCEPTION_STACK_OVERFLOW:
			case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
			case EXCEPTION_IN_PAGE_ERROR:
			case 0xC0000409:  // stack buffer overrun / fail fast
				break;
			default:
				return EXCEPTION_CONTINUE_SEARCH;
			}
			if (::InterlockedIncrement(&crashesLogged) > 6) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			Fault fault;
			fault.code = code;
			fault.address = reinterpret_cast<std::uintptr_t>(a_info->ExceptionRecord->ExceptionAddress);
			if (code == EXCEPTION_ACCESS_VIOLATION && a_info->ExceptionRecord->NumberParameters >= 2) {
				fault.access = static_cast<int>(a_info->ExceptionRecord->ExceptionInformation[0]);
				fault.data = static_cast<std::uintptr_t>(a_info->ExceptionRecord->ExceptionInformation[1]);
			}
			const char* what = activity;
			if (code == EXCEPTION_STACK_OVERFLOW) {
				DC_ERROR("crash (first chance): stack overflow at %s, thread %lu, while %s", Where(fault.address).c_str(), ::GetCurrentThreadId(),
					what ? what : "not in a DisCraft call");
				return EXCEPTION_CONTINUE_SEARCH;
			}
			// Code addresses on the stack: the call chain, roughly (no frame pointers needed).
			std::string stack;
#	if defined(_M_IX86) || defined(__i386__)
			std::uintptr_t sp = a_info->ContextRecord->Esp;
#	else
			std::uintptr_t sp = 0;
#	endif
			// Up to 8 KB of the stack, within the committed region it lives in.
			std::uintptr_t end = sp;
			MEMORY_BASIC_INFORMATION region{};
			if (sp && ::VirtualQuery(reinterpret_cast<LPCVOID>(sp), &region, sizeof(region)) && region.State == MEM_COMMIT &&
				!(region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
				end = std::min<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize, sp + 0x2000);
			}
			int found = 0;
			for (; sp + 4 <= end && found < 14; sp += 4) {
				const auto value = *reinterpret_cast<const std::uint32_t*>(sp);
				if (value > 0x10000 && IsCode(value)) {
					stack += (stack.empty() ? "" : ", ") + Where(value);
					++found;
				}
			}
			DC_ERROR("crash (first chance): %s, thread %lu, while %s; code on the stack: %s", Describe(fault).c_str(), ::GetCurrentThreadId(),
				what ? what : "not in a DisCraft call", stack.c_str());
			return EXCEPTION_CONTINUE_SEARCH;
		}
	}

	void InstallCrashLog()
	{
		if (::AddVectoredExceptionHandler(1, &FirstChance)) {
			DC_INFO("crash log armed (first-chance exceptions are logged)");
		}
	}
#else
	void InstallCrashLog() {}
#endif
}
