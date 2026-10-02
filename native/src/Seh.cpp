#include "Seh.h"

#include "Log.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <set>
#include <vector>

#ifdef _WIN32
#	define WIN32_LEAN_AND_MEAN
#	include <windows.h>
#	ifdef _MSC_VER
#		include <intrin.h>
#	endif
#endif

namespace discraft::seh
{
	namespace
	{
		const char* volatile activity = nullptr;
	}

	void SetActivity(const char* a_what) { activity = a_what; }

	// ---- Run: DisCraft's own code ------------------------------------------------------------------
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
#else
	bool Run(Fn a_fn, void* a_context, Fault&)
	{
		a_fn(a_context);
		return true;
	}
#endif

	// ---- RunRecoverable: calls into the game -------------------------------------------------------
	// The game catches faults in its own code (and shows its crash dialog) before a __try of ours
	// further out could, so the taking back happens in a vectored handler, which runs first: it puts
	// the registers GuardedCall saved on entry back and continues at GuardedCall's exit, returning 0.
	// The SEH chain is cut back to where it was and the x87 state reset (a fault mid-calculation can
	// leave values on its stack).
#if defined(_MSC_VER) && defined(_M_IX86)
	namespace
	{
		struct RecoveryRegs  // the asm below uses these offsets
		{
			DWORD esp;   // 0
			DWORD ebp;   // 4
			DWORD ebx;   // 8
			DWORD esi;   // 12
			DWORD edi;   // 16
			DWORD seh;   // 20
			WORD  fpcw;  // 24
			WORD  pad;   // 26
		};

		struct RecoveryFrame
		{
			RecoveryRegs regs;
			Fault*       fault;
		};

		constexpr LONG kMaxDepth = 8;
		RecoveryFrame  recoveryFrames[kMaxDepth];
		volatile LONG  recoveryDepth = 0;
		DWORD          recoveryThread = 0;

		// fn(ctx), with the registers to come back with saved in *regs first; 1 when fn returned.
		__declspec(naked) int __cdecl GuardedCall(Fn /*fn*/, void* /*ctx*/, RecoveryRegs* /*regs*/)
		{
			__asm {
				push  ebp
				mov   ebp, esp
				push  ebx
				push  esi
				push  edi
				mov   ecx, [ebp + 16]
				mov   [ecx + 0], esp
				mov   [ecx + 4], ebp
				mov   [ecx + 8], ebx
				mov   [ecx + 12], esi
				mov   [ecx + 16], edi
				mov   eax, dword ptr fs:[0]
				mov   [ecx + 20], eax
				fnstcw word ptr [ecx + 24]
				push  dword ptr [ebp + 12]
				call  dword ptr [ebp + 8]
				add   esp, 4
				mov   eax, 1
				pop   edi
				pop   esi
				pop   ebx
				pop   ebp
				ret
			}
		}

		// Where a fault inside GuardedCall continues: the handler has set esp, ebp, ebx, esi and edi
		// as GuardedCall saved them, and ecx to the saved registers. GuardedCall's exit, with 0.
		__declspec(naked) void ResumeAfterFault()
		{
			__asm {
				cld
				mov   eax, [ecx + 20]
				mov   dword ptr fs:[0], eax
				fninit
				fldcw word ptr [ecx + 24]
				xor   eax, eax
				pop   edi
				pop   esi
				pop   ebx
				pop   ebp
				ret
			}
		}

		bool TakeBack(EXCEPTION_POINTERS* a_info, const Fault& a_fault)
		{
			const LONG depth = recoveryDepth;
			if (depth <= 0 || depth > kMaxDepth || ::GetCurrentThreadId() != recoveryThread) {
				return false;
			}
			auto& frame = recoveryFrames[depth - 1];
			if (frame.fault) {
				*frame.fault = a_fault;
			}
			auto* c = a_info->ContextRecord;
			c->Esp = frame.regs.esp;
			c->Ebp = frame.regs.ebp;
			c->Ebx = frame.regs.ebx;
			c->Esi = frame.regs.esi;
			c->Edi = frame.regs.edi;
			c->Ecx = reinterpret_cast<DWORD>(&frame.regs);
			c->Eip = reinterpret_cast<DWORD>(&ResumeAfterFault);
			return true;
		}
	}

	bool RunRecoverable(Fn a_fn, void* a_context, Fault& a_fault)
	{
		const LONG  depth = recoveryDepth;
		const DWORD thread = ::GetCurrentThreadId();
		if (depth >= kMaxDepth || (depth > 0 && recoveryThread != thread)) {
			a_fn(a_context);
			return true;
		}
		recoveryThread = thread;
		auto& frame = recoveryFrames[depth];
		frame.fault = &a_fault;
		recoveryDepth = depth + 1;
		const int returned = GuardedCall(a_fn, a_context, &frame.regs);
		recoveryDepth = depth;
		return returned != 0;
	}

	bool Guarded() { return true; }
#else
	bool RunRecoverable(Fn a_fn, void* a_context, Fault&)
	{
		a_fn(a_context);
		return true;
	}

#	ifdef _MSC_VER
	bool Guarded() { return true; }
#	else
	bool Guarded() { return false; }
#	endif
#endif

	// ---- describing faults -------------------------------------------------------------------------
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

	// ---- the first-chance handler ------------------------------------------------------------------
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

		// The end of the readable region a_address is in (0 if it isn't readable).
		std::uintptr_t ReadableEnd(std::uintptr_t a_address)
		{
			MEMORY_BASIC_INFORMATION info{};
			if (!::VirtualQuery(reinterpret_cast<LPCVOID>(a_address), &info, sizeof(info)) || info.State != MEM_COMMIT ||
				(info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
				return 0;
			}
			return reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
		}

		std::string Hex(std::uintptr_t a_from, std::size_t a_bytes)
		{
			std::string out;
			const auto  end = ReadableEnd(a_from);
			for (std::size_t i = 0; i < a_bytes && a_from + i < end; ++i) {
				char b[4];
				std::snprintf(b, sizeof(b), "%02X", *reinterpret_cast<const std::uint8_t*>(a_from + i));
				out += b;
			}
			return out.empty() ? "?" : out;
		}

		std::string Details(const EXCEPTION_POINTERS* a_info)
		{
			std::string out;
#	if defined(_M_IX86) || defined(__i386__)
			const auto* c = a_info->ContextRecord;
			char        regs[200];
			std::snprintf(regs, sizeof(regs), "eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX",
				static_cast<unsigned long>(c->Eax), static_cast<unsigned long>(c->Ebx), static_cast<unsigned long>(c->Ecx), static_cast<unsigned long>(c->Edx),
				static_cast<unsigned long>(c->Esi), static_cast<unsigned long>(c->Edi), static_cast<unsigned long>(c->Ebp), static_cast<unsigned long>(c->Esp));
			out += regs;
			const std::uintptr_t ip = c->Eip;
			out += "; code " + Where(ip - 32) + ": " + Hex(ip - 32, 32) + " | " + Hex(ip, 32);
			// Code addresses on the stack: the call chain, roughly (no frame pointers needed).
			std::uintptr_t sp = c->Esp;
			const auto     end = std::min<std::uintptr_t>(ReadableEnd(sp), sp + 0x2000);
			std::string    stack;
			int            found = 0;
			for (; sp && sp + 4 <= end && found < 14; sp += 4) {
				const auto value = *reinterpret_cast<const std::uint32_t*>(sp);
				if (value > 0x10000 && IsCode(value)) {
					stack += (stack.empty() ? "" : ", ") + Where(value);
					++found;
				}
			}
			out += "; code on the stack: " + stack;
			// The first words on the stack, for arguments.
			out += "; stack: " + Hex(c->Esp, 48);
#	else
			(void)a_info;
#	endif
			return out;
		}

		volatile LONG crashesLogged = 0;

		LONG CALLBACK FirstChance(EXCEPTION_POINTERS* a_info)
		{
			const DWORD code = a_info->ExceptionRecord->ExceptionCode;
			bool        recoverable = true;
			switch (code) {
			case EXCEPTION_ACCESS_VIOLATION:
			case EXCEPTION_ILLEGAL_INSTRUCTION:
			case EXCEPTION_PRIV_INSTRUCTION:
			case EXCEPTION_INT_DIVIDE_BY_ZERO:
			case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
			case EXCEPTION_IN_PAGE_ERROR:
				break;
			case EXCEPTION_STACK_OVERFLOW:
			case 0xC0000409:  // stack buffer overrun / fail fast
				recoverable = false;
				break;
			default:
				return EXCEPTION_CONTINUE_SEARCH;
			}
			(void)recoverable;
			Fault fault;
			fault.code = code;
			fault.address = reinterpret_cast<std::uintptr_t>(a_info->ExceptionRecord->ExceptionAddress);
			if (code == EXCEPTION_ACCESS_VIOLATION && a_info->ExceptionRecord->NumberParameters >= 2) {
				fault.access = static_cast<int>(a_info->ExceptionRecord->ExceptionInformation[0]);
				fault.data = static_cast<std::uintptr_t>(a_info->ExceptionRecord->ExceptionInformation[1]);
			}
			const char* what = activity;
			const bool  log = ::InterlockedIncrement(&crashesLogged) <= 12;
			if (code == EXCEPTION_STACK_OVERFLOW) {
				if (log) {
					DC_ERROR("crash (first chance): stack overflow at %s, thread %lu, while %s", Where(fault.address).c_str(), ::GetCurrentThreadId(),
						what ? what : "not in a DisCraft call");
				}
				return EXCEPTION_CONTINUE_SEARCH;
			}
#	if defined(_MSC_VER) && defined(_M_IX86)
			if (recoverable && TakeBack(a_info, fault)) {
				if (log) {
					DC_ERROR("recovered from %s while %s; %s", Describe(fault).c_str(), what ? what : "in a guarded call", Details(a_info).c_str());
				}
				activity = nullptr;
				return EXCEPTION_CONTINUE_EXECUTION;
			}
#	endif
			if (log) {
				DC_ERROR("crash (first chance): %s, thread %lu, while %s; %s", Describe(fault).c_str(), ::GetCurrentThreadId(),
					what ? what : "not in a DisCraft call", Details(a_info).c_str());
			}
			return EXCEPTION_CONTINUE_SEARCH;
		}
	}

	void InstallCrashLog()
	{
		if (::AddVectoredExceptionHandler(1, &FirstChance)) {
			DC_INFO("crash log armed (first-chance exceptions are logged%s)", Guarded() ? ", faults in calls into the game are taken back" : "");
		}
	}
#else
	void InstallCrashLog() {}
#endif

	// ---- calls that crashed the game before --------------------------------------------------------
	namespace
	{
		std::mutex            guardLock;
		std::set<std::string> crashed;  // blocked this run
		std::set<std::string> trying;   // in progress now
		std::string           guardPath;
		const char*           kBuild = __DATE__ " " __TIME__;

		void WriteGuard()
		{
			if (guardPath.empty()) {
				return;
			}
			std::FILE* f = nullptr;
#ifdef _WIN32
			f = ::_wfopen(std::wstring(guardPath.begin(), guardPath.end()).c_str(), L"w");
#else
			f = std::fopen(guardPath.c_str(), "w");
#endif
			if (!f) {
				return;
			}
			std::fprintf(f, "build %s\n", kBuild);
			for (const auto& c : crashed) {
				std::fprintf(f, "crashed %s\n", c.c_str());
			}
			for (const auto& t : trying) {
				std::fprintf(f, "trying %s\n", t.c_str());
			}
			std::fclose(f);
		}
	}

	void InitCrashGuard()
	{
		std::lock_guard guard(guardLock);
#ifdef _WIN32
		char base[MAX_PATH]{};
		const DWORD n = ::ExpandEnvironmentStringsA("%LOCALAPPDATA%\\DisCraft\\crashguard.txt", base, MAX_PATH);
		if (n == 0 || n > MAX_PATH) {
			return;
		}
		guardPath = base;
		std::FILE* f = std::fopen(base, "r");
#else
		guardPath.clear();
		std::FILE* f = nullptr;
#endif
		if (f) {
			char line[512];
			bool sameBuild = false;
			while (std::fgets(line, sizeof(line), f)) {
				std::string text(line);
				while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
					text.pop_back();
				}
				if (text.rfind("build ", 0) == 0) {
					sameBuild = text.substr(6) == kBuild;
				} else if (sameBuild && text.rfind("crashed ", 0) == 0) {
					crashed.insert(text.substr(8));
				} else if (sameBuild && text.rfind("trying ", 0) == 0) {
					crashed.insert(text.substr(7));  // the game died in it last time
				}
			}
			std::fclose(f);
		}
		for (const auto& c : crashed) {
			DC_WARN("crash guard: the game crashed in %s in an earlier run of this build; not doing it again", c.c_str());
		}
		WriteGuard();
	}

	bool Blocked(const std::string& a_what)
	{
		std::lock_guard guard(guardLock);
		return crashed.count(a_what) != 0;
	}

	void Trying(const std::string& a_what)
	{
		std::lock_guard guard(guardLock);
		trying.insert(a_what);
		WriteGuard();
	}

	void Survived(const std::string& a_what)
	{
		std::lock_guard guard(guardLock);
		if (trying.erase(a_what)) {
			WriteGuard();
		}
	}
}
