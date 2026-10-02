#pragma once

#include <cstdint>
#include <string>

// Structured-exception guard for calls into the game. DisCraft calls engine functions it found at
// runtime; when one of those calls goes wrong it should stop DisCraft, not take the game down. Only
// the MSVC build can catch these (MinGW has no __try); there Run just calls through.
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

	// a_fn(a_context); false (and a_fault filled in) if it raised a structured exception.
	bool Run(Fn a_fn, void* a_context, Fault& a_fault);
	// Whether Run can actually catch anything in this build.
	bool Guarded();
	// "access violation reading 0x00000010 at Dishonored.exe+0x12345".
	std::string Describe(const Fault& a_fault);
	// "Dishonored.exe+0x12345" (or the bare address).
	std::string Where(std::uintptr_t a_address);
}
