#pragma once

#include <cstdint>
#include <string>

// Faults in calls into the game. DisCraft calls engine functions it found at runtime; when one of
// those calls goes wrong it should stop that call, not take the game down.
namespace discraft::seh
{
	struct Fault
	{
		std::uint32_t  code{ 0 };
		std::uintptr_t address{ 0 };  // where it happened
		std::uintptr_t data{ 0 };     // for an access violation: the address read or written
		int            access{ -1 };  // 0 read, 1 write, 8 execute
	};

	using Fn = void (*)(void* a_context);

	// a_fn(a_context); false (and a_fault filled in) if it raised a structured exception that came
	// back up to here. Only DisCraft's own code: the game catches its faults deeper down itself.
	// The MSVC build only (MinGW has no __try); elsewhere it just calls through.
	bool Run(Fn a_fn, void* a_context, Fault& a_fault);

	// a_fn(a_context), for calls into the game: a serious fault anywhere inside it (in the game's
	// code included) is taken back before the game's own handlers see it, and execution continues
	// here as if a_fn had returned; false then, with a_fault filled in and the details logged. The
	// game's frames that were running are abandoned (whatever they held stays as it was). Same
	// thread only; nests. The MSVC x86 build only; elsewhere it just calls through.
	bool RunRecoverable(Fn a_fn, void* a_context, Fault& a_fault);

	// Whether Run / RunRecoverable can actually catch anything in this build.
	bool Guarded();
	// "access violation reading 0x00000010 at Dishonored.exe+0x12345".
	std::string Describe(const Fault& a_fault);
	// "Dishonored.exe+0x12345" (or the bare address).
	std::string Where(std::uintptr_t a_address);

	// What DisCraft is doing on this thread right now ("calling native Engine.Actor.Trace"), for the
	// crash log. The string must outlive the activity; nullptr when done.
	void SetActivity(const char* a_what);
	// Logs serious exceptions (access violations, illegal instructions, stack overflows, ...) as
	// they happen, before the game's own handlers: where, the registers, the code around it, what
	// DisCraft was doing and the code addresses on the stack; and takes back the ones inside
	// RunRecoverable.
	void InstallCrashLog();

	// Calls that crashed the game in an earlier run of this build are not made again: before the
	// first call of something risky its name is written to a file and taken off once it returned,
	// so a name still there at the next start is one the game died in.
	void InitCrashGuard();
	bool Blocked(const std::string& a_what);
	void Trying(const std::string& a_what);
	void Survived(const std::string& a_what);
}
