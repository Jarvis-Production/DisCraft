#include "Mem.h"

#include <algorithm>
#include <mutex>

#ifdef _WIN32
#	define WIN32_LEAN_AND_MEAN
#	include <windows.h>
#endif

namespace discraft::mem
{
	namespace
	{
		ReadableFn         readableOverride = nullptr;
		std::mutex         cacheLock;
		std::vector<Range> cache;  // sorted by begin, non-overlapping
		ModuleInfo         mainModule;
		bool               mainModuleLoaded = false;

#ifdef _WIN32
		bool ProtectReadable(DWORD a_protect)
		{
			if (a_protect & (PAGE_GUARD | PAGE_NOACCESS)) {
				return false;
			}
			return (a_protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
								   PAGE_EXECUTE_WRITECOPY)) != 0;
		}
#endif

#ifdef _WIN32
		bool InCache(std::uintptr_t a_begin, std::uintptr_t a_end)
		{
			auto it = std::upper_bound(cache.begin(), cache.end(), a_begin, [](std::uintptr_t a, const Range& r) { return a < r.begin; });
			if (it == cache.begin()) {
				return false;
			}
			--it;
			return a_begin >= it->begin && a_end <= it->end;
		}

		void AddToCache(Range a_range)
		{
			auto it = std::lower_bound(cache.begin(), cache.end(), a_range.begin, [](const Range& r, std::uintptr_t a) { return r.begin < a; });
			cache.insert(it, a_range);
			if (cache.size() > 4096) {
				cache.clear();
			}
		}
#endif
	}

	void SetReadableOverride(ReadableFn a_fn) { readableOverride = a_fn; }

	bool Readable(std::uintptr_t a_addr, std::size_t a_size)
	{
		if (a_addr < 0x10000 || a_addr + a_size < a_addr) {
			return false;
		}
		if (readableOverride) {
			return readableOverride(a_addr, a_size);
		}
#ifdef _WIN32
		const std::uintptr_t end = a_addr + a_size;
		std::lock_guard      guard(cacheLock);
		if (InCache(a_addr, end)) {
			return true;
		}
		std::uintptr_t at = a_addr;
		while (at < end) {
			MEMORY_BASIC_INFORMATION info{};
			if (!::VirtualQuery(reinterpret_cast<LPCVOID>(at), &info, sizeof(info))) {
				return false;
			}
			if (info.State != MEM_COMMIT || !ProtectReadable(info.Protect)) {
				return false;
			}
			const auto regionBegin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
			const auto regionEnd = regionBegin + info.RegionSize;
			AddToCache({ regionBegin, regionEnd });
			at = regionEnd;
		}
		return true;
#else
		return true;
#endif
	}

	void FlushCache()
	{
		std::lock_guard guard(cacheLock);
		cache.clear();
	}

	ModuleInfo MainModule()
	{
		if (mainModuleLoaded) {
			return mainModule;
		}
#ifdef _WIN32
		auto* base = reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr));
		auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
		auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		mainModule.base = reinterpret_cast<std::uintptr_t>(base);
		mainModule.size = nt->OptionalHeader.SizeOfImage;
		auto* section = IMAGE_FIRST_SECTION(nt);
		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
			const Range range{ mainModule.base + section->VirtualAddress,
				mainModule.base + section->VirtualAddress + std::max<DWORD>(section->Misc.VirtualSize, section->SizeOfRawData) };
			if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
				mainModule.code.push_back(range);
			} else if (section->Characteristics & IMAGE_SCN_MEM_WRITE) {
				mainModule.data.push_back(range);
			}
		}
#endif
		mainModuleLoaded = true;
		return mainModule;
	}

	bool InCode(std::uintptr_t a_addr)
	{
		const auto& module = MainModule();
		for (const auto& r : module.code) {
			if (r.Contains(a_addr)) {
				return true;
			}
		}
		return false;
	}

	bool WritePointer(void* a_slot, const void* a_value)
	{
#ifdef _WIN32
		DWORD old = 0;
		if (!::VirtualProtect(a_slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
			return false;
		}
		std::memcpy(a_slot, &a_value, sizeof(void*));
		DWORD ignored = 0;
		::VirtualProtect(a_slot, sizeof(void*), old, &ignored);
		::FlushInstructionCache(::GetCurrentProcess(), a_slot, sizeof(void*));
		return true;
#else
		std::memcpy(a_slot, &a_value, sizeof(void*));
		return true;
#endif
	}
}
