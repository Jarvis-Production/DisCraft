#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Memory helpers: checked reads of the game's memory, its module sections, pointer patches.
namespace discraft::mem
{
	struct Range
	{
		std::uintptr_t begin{ 0 };
		std::uintptr_t end{ 0 };
		[[nodiscard]] bool Contains(std::uintptr_t a_addr) const { return a_addr >= begin && a_addr < end; }
	};

	struct ModuleInfo
	{
		std::uintptr_t     base{ 0 };
		std::uintptr_t     size{ 0 };
		std::vector<Range> code;  // executable sections
		std::vector<Range> data;  // writable sections (.data / .bss)
	};

	// True if [a_addr, a_addr + a_size) is committed, readable memory. Cached per frame.
	bool Readable(std::uintptr_t a_addr, std::size_t a_size);
	// Forget the cache (call once a frame: memory can be freed between frames).
	void FlushCache();

	// Tests replace the check (their fake engine memory is all plain heap memory).
	using ReadableFn = bool (*)(std::uintptr_t, std::size_t);
	void SetReadableOverride(ReadableFn a_fn);

	// The game's executable: base, executable and writable sections.
	ModuleInfo MainModule();
	bool       InCode(std::uintptr_t a_addr);

	// Overwrites a pointer-sized slot that may sit in read-only memory (vtables, ...).
	bool WritePointer(void* a_slot, const void* a_value);

	template <class T>
	T Read(std::uintptr_t a_addr)
	{
		T value;
		std::memcpy(&value, reinterpret_cast<const void*>(a_addr), sizeof(T));
		return value;
	}

	template <class T>
	bool TryRead(std::uintptr_t a_addr, T& a_out)
	{
		if (!Readable(a_addr, sizeof(T))) {
			return false;
		}
		std::memcpy(&a_out, reinterpret_cast<const void*>(a_addr), sizeof(T));
		return true;
	}

	template <class T>
	void Write(std::uintptr_t a_addr, const T& a_value)
	{
		std::memcpy(reinterpret_cast<void*>(a_addr), &a_value, sizeof(T));
	}
}
