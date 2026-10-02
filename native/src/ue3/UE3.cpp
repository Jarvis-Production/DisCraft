#include "UE3.h"

#include "Seh.h"

#include "../Config.h"
#include "../Log.h"

#include <algorithm>
#include <cstdio>
#include <array>
#include <unordered_map>

namespace discraft::ue3
{
	namespace
	{
		Arrays         arrays;
		Layout         L;
		bool           ready = false;
		std::uintptr_t processInternal = 0;
		int            processEventIndex = 0;
		// Names: all ANSI, all UTF-16, or per entry by the index field's low bit (UE3's NAME_UNICODE).
		bool wideNames = false;
		int  nameIndexField = -1;

		std::unordered_map<std::string, int> nameCache;
		std::unordered_map<std::string, Obj> objectCache;

		constexpr std::uint32_t kFuncNative = 0x400;

		template <class T>
		T Rd(std::uintptr_t a_addr)
		{
			return mem::Read<T>(a_addr);
		}

		bool Ok(std::uintptr_t a_addr, std::size_t a_size) { return mem::Readable(a_addr, a_size); }

		struct ArrayHeader
		{
			Addr         data;
			std::int32_t num;
			std::int32_t max;
		};

		ArrayHeader ReadArray(std::uintptr_t a_at)
		{
			return { Rd<Addr>(a_at), Rd<std::int32_t>(a_at + 4), Rd<std::int32_t>(a_at + 8) };
		}

		// FNames are case-insensitive (the table keeps one entry per name, in its first casing).
		constexpr std::uint32_t Fold(std::uint32_t a_c) { return a_c >= 'A' && a_c <= 'Z' ? a_c + 32 : a_c; }

		bool MatchAnsi(std::uintptr_t a_at, std::string_view a_text)
		{
			if (!Ok(a_at, a_text.size() + 1)) {
				return false;
			}
			for (std::size_t i = 0; i < a_text.size(); ++i) {
				if (Fold(Rd<std::uint8_t>(a_at + i)) != Fold(static_cast<std::uint8_t>(a_text[i]))) {
					return false;
				}
			}
			return Rd<char>(a_at + a_text.size()) == 0;
		}

		bool MatchWide(std::uintptr_t a_at, std::string_view a_text)
		{
			if (!Ok(a_at, (a_text.size() + 1) * 2)) {
				return false;
			}
			for (std::size_t i = 0; i < a_text.size(); ++i) {
				if (Fold(Rd<std::uint16_t>(a_at + i * 2)) != Fold(static_cast<std::uint8_t>(a_text[i]))) {
					return false;
				}
			}
			return Rd<std::uint16_t>(a_at + a_text.size() * 2) == 0;
		}

		bool SameName(const std::string& a_a, std::string_view a_b)
		{
			if (a_a.size() != a_b.size()) {
				return false;
			}
			for (std::size_t i = 0; i < a_a.size(); ++i) {
				if (Fold(static_cast<std::uint8_t>(a_a[i])) != Fold(static_cast<std::uint8_t>(a_b[i]))) {
					return false;
				}
			}
			return true;
		}

		bool EntryIsWide(std::uintptr_t a_entry)
		{
			if (nameIndexField >= 0) {
				return (Rd<std::int32_t>(a_entry + nameIndexField) & 1) != 0;
			}
			return wideNames;
		}

		std::string EntryText(std::uintptr_t a_entry)
		{
			std::string out;
			if (!a_entry || L.nameText < 0) {
				return out;
			}
			const std::uintptr_t at = a_entry + L.nameText;
			const bool           wide = EntryIsWide(a_entry);
			for (int i = 0; i < 1024; ++i) {
				const std::uintptr_t p = at + (wide ? i * 2 : i);
				if (!Ok(p, wide ? 2 : 1)) {
					break;
				}
				const std::uint32_t c = wide ? Rd<std::uint16_t>(p) : Rd<std::uint8_t>(p);
				if (!c) {
					break;
				}
				out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
			}
			return out;
		}

		std::uintptr_t NameEntry(int a_index)
		{
			if (!arrays.names) {
				return 0;
			}
			const auto h = ReadArray(arrays.names);
			if (a_index < 0 || a_index >= h.num) {
				return 0;
			}
			return Rd<Addr>(h.data + static_cast<std::uintptr_t>(a_index) * 4);
		}

		// The hard-coded names UE3 registers first, right after "None".
		constexpr std::array<std::string_view, 8> kFirstNames{ "ByteProperty", "IntProperty", "BoolProperty", "FloatProperty", "ObjectProperty",
			"NameProperty", "StructProperty", "ArrayProperty" };
		constexpr std::array<int, 7>              kNameTextOffsets{ 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20 };

		bool IsNameArray(const ArrayHeader& a_h, Layout& a_layout)
		{
			if (a_h.num < 1000 || a_h.num > 4000000 || a_h.max < a_h.num || a_h.max > 16000000 || (a_h.data & 3) || !Ok(a_h.data, 17 * 4)) {
				return false;
			}
			const Addr e0 = Rd<Addr>(a_h.data);
			if (!e0 || (e0 & 3) || !Ok(e0, 0x30)) {
				return false;
			}
			for (const int off : kNameTextOffsets) {
				bool wide = false;
				if (MatchAnsi(e0 + off, "None")) {
					wide = false;
				} else if (MatchWide(e0 + off, "None")) {
					wide = true;
				} else {
					continue;
				}
				int hits = 0;
				for (int i = 1; i <= 16; ++i) {
					const Addr e = Rd<Addr>(a_h.data + static_cast<std::uintptr_t>(i) * 4);
					if (!e) {
						continue;
					}
					for (const auto name : kFirstNames) {
						if (wide ? MatchWide(e + off, name) : MatchAnsi(e + off, name)) {
							++hits;
							break;
						}
					}
				}
				if (hits < 4) {
					continue;
				}
				a_layout.nameText = off;
				wideNames = wide;
				// The entry's own index (shifted left once, low bit = UTF-16) if the entry keeps one.
				nameIndexField = -1;
				for (int k = 0; k + 4 <= off; k += 4) {
					bool all = true;
					for (int i = 1; i <= 16 && all; ++i) {
						const Addr e = Rd<Addr>(a_h.data + static_cast<std::uintptr_t>(i) * 4);
						if (e && (Rd<std::int32_t>(e + k) >> 1) != i) {
							all = false;
						}
					}
					if (all) {
						nameIndexField = k;
						break;
					}
				}
				return true;
			}
			return false;
		}

		bool IsObjectArray(const ArrayHeader& a_h, Layout& a_layout)
		{
			if (a_h.num < 5000 || a_h.num > 4000000 || a_h.max < a_h.num || a_h.max > 16000000 || (a_h.data & 3)) {
				return false;
			}
			if (!Ok(a_h.data, static_cast<std::size_t>(a_h.num) * 4)) {
				return false;
			}
			std::vector<std::pair<int, std::uintptr_t>> samples;
			for (int k = 0; k < 96; ++k) {
				const int  i = k < 32 ? k + 1 : static_cast<int>(static_cast<std::int64_t>(a_h.num - 1) * (k - 32) / 63);
				const Addr o = Rd<Addr>(a_h.data + static_cast<std::uintptr_t>(i) * 4);
				if (!o) {
					continue;
				}
				if ((o & 3) || !Ok(o, 0x24)) {
					return false;
				}
				samples.emplace_back(i, o);
			}
			if (samples.size() < 32) {
				return false;
			}
			for (int off = 4; off < 0x80; off += 4) {
				bool all = true;
				for (const auto& [i, o] : samples) {
					if (!Ok(o + off, 4) || Rd<std::int32_t>(o + off) != i) {
						all = false;
						break;
					}
				}
				if (all) {
					a_layout.index = off;
					return true;
				}
			}
			return false;
		}

		// Name indices of several names in one pass over the name table (-1: not there).
		std::vector<int> NameIndices(const std::vector<std::string_view>& a_texts)
		{
			std::vector<int> out(a_texts.size(), -1);
			const int        n = NameCount();
			std::size_t      left = a_texts.size();
			for (int i = 0; i < n && left; ++i) {
				const auto e = NameEntry(i);
				if (!e) {
					continue;
				}
				const auto text = EntryText(e);
				for (std::size_t t = 0; t < a_texts.size(); ++t) {
					if (out[t] < 0 && SameName(text, a_texts[t])) {
						out[t] = i;
						--left;
					}
				}
			}
			return out;
		}

		// A child field of one struct (not its supers) by name index, using only next/children.
		Obj ChildNamed(Obj a_struct, int a_nameIndex)
		{
			if (!a_struct || a_nameIndex < 0) {
				return 0;
			}
			int guard = 0;
			for (Obj f = Rd<Addr>(a_struct + L.children); f && guard < 100000; f = Rd<Addr>(f + L.next), ++guard) {
				if (!IsObject(f)) {
					return 0;
				}
				if (NameIndexOf(f) == a_nameIndex) {
					return f;
				}
			}
			return 0;
		}

		template <class Pred>
		int FirstOffset(int a_from, int a_to, const std::vector<int>& a_taken, Pred a_pred)
		{
			for (int off = a_from; off < a_to; off += 4) {
				if (std::find(a_taken.begin(), a_taken.end(), off) != a_taken.end()) {
					continue;
				}
				if (a_pred(off)) {
					return off;
				}
			}
			return -1;
		}

		bool InRanges(std::uintptr_t a_addr, const std::vector<mem::Range>& a_ranges)
		{
			for (const auto& r : a_ranges) {
				if (r.Contains(a_addr)) {
					return true;
				}
			}
			return false;
		}

		void ApplyOverride(const char* a_key, int& a_field)
		{
			const int v = config::Int("Engine", a_key, 0);
			if (v > 0) {
				DC_INFO("UE3: %s forced to 0x%X by DisCraft.ini (detected 0x%X)", a_key, v, a_field);
				a_field = v;
			}
		}
	}

	std::string Layout::Describe() const
	{
		return log::Format(
			"UObject{index 0x%X, outer 0x%X, name 0x%X, class 0x%X} UField{next 0x%X} UStruct{super 0x%X, children 0x%X, size 0x%X} "
			"UProperty{arrayDim 0x%X, elementSize 0x%X, offset 0x%X, bitMask 0x%X, struct 0x%X} UFunction{func 0x%X, flags 0x%X} "
			"UEnum{names 0x%X} FNameEntry{text 0x%X}",
			index, outer, name, cls, next, superField, children, propertySize, arrayDim, elementSize, offset, bitMask, structType, func, funcFlags, enumNames,
			nameText);
	}

	// ---- discovery -------------------------------------------------------------------------------

	bool FindArrays(const std::vector<mem::Range>& a_ranges, Arrays& a_out, Layout& a_layout)
	{
		a_out = {};
		for (const auto& r : a_ranges) {
			for (std::uintptr_t a = (r.begin + 3) & ~std::uintptr_t(3); a + 12 <= r.end; a += 4) {
				if (!Ok(a, 12)) {
					// Skip to the next page.
					const std::uintptr_t next = (a | 0xFFF) + 1;
					a = next >= 4 ? next - 4 : next;
					continue;
				}
				const auto h = ReadArray(a);
				if (!a_out.names && IsNameArray(h, a_layout)) {
					a_out.names = a;
				} else if (!a_out.objects && IsObjectArray(h, a_layout)) {
					a_out.objects = a;
				}
				if (a_out.names && a_out.objects) {
					return true;
				}
			}
		}
		return a_out.names && a_out.objects;
	}

	bool DetectLayout(const Arrays& a_arrays, Layout& a_layout, const std::vector<mem::Range>& a_code)
	{
		arrays = a_arrays;
		L = a_layout;
		ready = false;
		nameCache.clear();
		objectCache.clear();

		const int count = ObjectCount();
		if (count <= 0 || L.index < 0 || L.nameText < 0) {
			return false;
		}
		std::vector<Obj> sample;
		for (int k = 0; k < 512; ++k) {
			const Obj o = ObjectAt(static_cast<int>(static_cast<std::int64_t>(count - 1) * k / 511));
			if (o && Ok(o, 0x100)) {
				sample.push_back(o);
			}
		}
		if (sample.size() < 64) {
			DC_WARN("UE3: too few objects to sample (%zu)", sample.size());
			return false;
		}
		const int nameCount = NameCount();

		const std::vector<std::string_view> targets{ "Core", "Engine", "Object", "Class", "Actor", "Pawn", "PlayerController", "Function", "Package" };
		const auto                          ids = NameIndices(targets);
		for (std::size_t t = 0; t < targets.size(); ++t) {
			if (ids[t] < 0) {
				DC_WARN("UE3: the name \"%.*s\" isn't loaded yet", static_cast<int>(targets[t].size()), targets[t].data());
				return false;
			}
		}
		const int idCore = ids[0], idEngine = ids[1], idObject = ids[2], idClass = ids[3], idActor = ids[4], idPackage = ids[8];

		// UObject::Name: an FName (index, number) that, across all objects, names the core classes.
		for (int off = 4; off < 0x80 && L.name < 0; off += 4) {
			if (off == L.index) {
				continue;
			}
			std::size_t valid = 0;
			for (const Obj o : sample) {
				const int v = Rd<std::int32_t>(o + off);
				const int number = Rd<std::int32_t>(o + off + 4);
				if (v >= 0 && v < nameCount && NameEntry(v) && number >= 0 && number < 1000000) {
					++valid;
				}
			}
			if (valid < sample.size() * 98 / 100) {
				continue;
			}
			std::vector<bool> seen(ids.size(), false);
			for (int i = 0; i < count; ++i) {
				const Obj o = ObjectAt(i);
				if (!o || !Ok(o + off, 4)) {
					continue;
				}
				const int v = Rd<std::int32_t>(o + off);
				for (std::size_t t = 0; t < ids.size(); ++t) {
					if (v == ids[t]) {
						seen[t] = true;
					}
				}
			}
			if (std::all_of(seen.begin(), seen.end(), [](bool b) { return b; })) {
				L.name = off;
			}
		}
		if (L.name < 0) {
			DC_WARN("UE3: couldn't find UObject::Name");
			return false;
		}

		// UObject::Class: an object pointer; Core.Class is its own class.
		std::vector<Obj> classNamed;
		std::vector<Obj> actorNamed;
		std::vector<Obj> objectNamed;
		for (int i = 0; i < count; ++i) {
			const Obj o = ObjectAt(i);
			if (!o || !Ok(o + L.name, 4)) {
				continue;
			}
			const int n = Rd<std::int32_t>(o + L.name);
			if (n == idClass) {
				classNamed.push_back(o);
			} else if (n == idActor) {
				actorNamed.push_back(o);
			} else if (n == idObject) {
				objectNamed.push_back(o);
			}
		}
		L.cls = FirstOffset(4, 0x80, { L.index, L.name, L.name + 4 }, [&](int off) {
			std::size_t valid = 0;
			for (const Obj o : sample) {
				if (IsObject(Rd<Addr>(o + off))) {  // the sample is readable to 0x100
					++valid;
				}
			}
			if (valid < sample.size() * 98 / 100) {
				return false;
			}
			return std::any_of(classNamed.begin(), classNamed.end(), [&](Obj c) { return Ok(c + off, 4) && Rd<Addr>(c + off) == c; });
		});
		if (L.cls < 0) {
			DC_WARN("UE3: couldn't find UObject::Class");
			return false;
		}

		// UObject::Outer: Class Engine.Actor lives in Package Engine, Class Core.Object in Package Core.
		const auto isClass = [&](Obj o) { return NameIndexOf(ClassOf(o)) == idClass; };
		L.outer = FirstOffset(4, 0x80, { L.index, L.name, L.name + 4, L.cls }, [&](int off) {
			const auto inPackage = [&](const std::vector<Obj>& a_named, int a_package) {
				return std::any_of(a_named.begin(), a_named.end(), [&](Obj o) {
					if (!isClass(o)) {
						return false;
					}
					if (!Ok(o + off, 4)) {
						return false;
					}
					const Obj p = Rd<Addr>(o + off);
					return IsObject(p) && Ok(p + off, 4) && NameIndexOf(p) == a_package && NameIndexOf(ClassOf(p)) == idPackage && Rd<Addr>(p + off) == 0;
				});
			};
			return inPackage(actorNamed, idEngine) && inPackage(objectNamed, idCore);
		});
		if (L.outer < 0) {
			DC_WARN("UE3: couldn't find UObject::Outer");
			return false;
		}

		const Obj objectCls = FindObject("Class", "Core.Object");
		const Obj actorCls = FindObject("Class", "Engine.Actor");
		const Obj pawnCls = FindObject("Class", "Engine.Pawn");
		const Obj controllerCls = FindObject("Class", "Engine.Controller");
		const Obj functionCls = FindObject("Class", "Core.Function");
		const Obj vectorStruct = FindObject("ScriptStruct", "Core.Object.Vector");
		const Obj rotatorStruct = FindObject("ScriptStruct", "Core.Object.Rotator");
		const Obj planeStruct = FindObject("ScriptStruct", "Core.Object.Plane");
		if (!objectCls || !actorCls || !pawnCls || !controllerCls || !functionCls || !vectorStruct || !rotatorStruct || !planeStruct) {
			DC_WARN("UE3: core classes missing (Object %d Actor %d Pawn %d Controller %d Function %d Vector %d Rotator %d Plane %d)", objectCls != 0,
				actorCls != 0, pawnCls != 0, controllerCls != 0, functionCls != 0, vectorStruct != 0, rotatorStruct != 0, planeStruct != 0);
			return false;
		}

		// UStruct::SuperField
		L.superField = FirstOffset(0x20, 0x100, {}, [&](int off) {
			return Rd<Addr>(actorCls + off) == objectCls && Rd<Addr>(pawnCls + off) == actorCls && Rd<Addr>(controllerCls + off) == actorCls;
		});
		// UStruct::Children: the first field, whose outer is the struct itself.
		L.children = FirstOffset(0x20, 0x100, { L.superField }, [&](int off) {
			for (const Obj s : { actorCls, pawnCls, objectCls, vectorStruct }) {
				const Obj f = Rd<Addr>(s + off);
				if (!IsObject(f) || OuterOf(f) != s) {
					return false;
				}
			}
			return true;
		});
		if (L.superField < 0 || L.children < 0) {
			DC_WARN("UE3: couldn't find UStruct::SuperField/Children (0x%X, 0x%X)", L.superField, L.children);
			return false;
		}
		// UField::Next: the chain through Actor's fields; Vector has exactly X, Y, Z.
		{
			int bestLen = 0;
			for (int off = 0x20; off < 0xA0; off += 4) {
				if (off == L.outer || off == L.cls || off == L.name || off == L.index) {
					continue;
				}
				const auto chain = [&](Obj s) {
					int len = 0;
					Obj f = Rd<Addr>(s + L.children);
					while (f && len < 100000) {
						if (!IsObject(f) || OuterOf(f) != s) {
							return -1;
						}
						++len;
						f = Rd<Addr>(f + off);
					}
					return len;
				};
				const int len = chain(actorCls);
				if (len > bestLen && len >= 10 && chain(vectorStruct) == 3) {
					bestLen = len;
					L.next = off;
				}
			}
		}
		if (L.next < 0) {
			DC_WARN("UE3: couldn't find UField::Next");
			return false;
		}
		// UStruct::PropertySize: Vector 12, Rotator 12, Plane 16.
		L.propertySize = FirstOffset(L.children + 4, L.children + 0x48, { L.superField }, [&](int off) {
			return Rd<std::int32_t>(vectorStruct + off) == 12 && Rd<std::int32_t>(rotatorStruct + off) == 12 && Rd<std::int32_t>(planeStruct + off) == 16;
		});
		if (L.propertySize < 0) {
			DC_WARN("UE3: couldn't find UStruct::PropertySize");
			return false;
		}

		// UProperty: Vector's X/Y/Z and Rotator's Pitch/Yaw/Roll sit at 0, 4, 8; Actor.Location is 12 bytes.
		const auto pn = NameIndices({ "X", "Y", "Z", "Pitch", "Yaw", "Roll", "Location", "BoolProperty", "PHYS_None" });
		const Obj  px = ChildNamed(vectorStruct, pn[0]), py = ChildNamed(vectorStruct, pn[1]), pz = ChildNamed(vectorStruct, pn[2]);
		const Obj  pp = ChildNamed(rotatorStruct, pn[3]), pyaw = ChildNamed(rotatorStruct, pn[4]), pr = ChildNamed(rotatorStruct, pn[5]);
		const Obj  ploc = ChildNamed(actorCls, pn[6]);
		if (!px || !py || !pz || !pp || !pyaw || !pr || !ploc) {
			DC_WARN("UE3: Vector/Rotator/Actor.Location fields not found");
			return false;
		}
		const int from = L.next + 4, to = L.next + 0x70;
		L.offset = FirstOffset(from, to, {}, [&](int off) {
			const int loc = Rd<std::int32_t>(ploc + off);
			return Rd<std::int32_t>(px + off) == 0 && Rd<std::int32_t>(py + off) == 4 && Rd<std::int32_t>(pz + off) == 8 &&
			       Rd<std::int32_t>(pp + off) == 0 && Rd<std::int32_t>(pyaw + off) == 4 && Rd<std::int32_t>(pr + off) == 8 && loc >= 0x30 && loc < 0x1000;
		});
		L.elementSize = FirstOffset(from, to, { L.offset }, [&](int off) {
			return Rd<std::int32_t>(px + off) == 4 && Rd<std::int32_t>(py + off) == 4 && Rd<std::int32_t>(pz + off) == 4 && Rd<std::int32_t>(pyaw + off) == 4 &&
			       Rd<std::int32_t>(ploc + off) == 12;
		});
		L.arrayDim = FirstOffset(from, to, { L.offset, L.elementSize }, [&](int off) {
			return Rd<std::int32_t>(px + off) == 1 && Rd<std::int32_t>(py + off) == 1 && Rd<std::int32_t>(pz + off) == 1 && Rd<std::int32_t>(pyaw + off) == 1 &&
			       Rd<std::int32_t>(ploc + off) == 1;
		});
		if (L.offset < 0 || L.elementSize < 0 || L.arrayDim < 0) {
			DC_WARN("UE3: couldn't find UProperty offsets (offset 0x%X, elementSize 0x%X, arrayDim 0x%X)", L.offset, L.elementSize, L.arrayDim);
			return false;
		}

		// UStructProperty::Struct: Actor.Location's is Vector.
		L.structType = FirstOffset(L.offset + 4, L.offset + 0x50, {}, [&](int off) { return Rd<Addr>(ploc + off) == vectorStruct; });

		// UBoolProperty::BitMask: single bits, different for bools sharing a dword.
		{
			std::vector<Obj> bools;
			for (Obj f = Rd<Addr>(actorCls + L.children); f && bools.size() < 512; f = Rd<Addr>(f + L.next)) {
				if (NameIndexOf(ClassOf(f)) == pn[7]) {
					bools.push_back(f);
				}
			}
			if (bools.size() >= 4) {
				// (Usually the same offset as UStructProperty::Struct: both are the first field after UProperty.)
				L.bitMask = FirstOffset(L.offset + 4, L.offset + 0x50, {}, [&](int off) {
					std::unordered_map<std::int32_t, std::uint32_t> used;  // property offset -> masks seen
					for (const Obj b : bools) {
						const auto mask = Rd<std::uint32_t>(b + off);
						if (!mask || (mask & (mask - 1))) {
							return false;
						}
						auto& seen = used[Rd<std::int32_t>(b + L.offset)];
						if (seen & mask) {
							return false;
						}
						seen |= mask;
					}
					return true;
				});
			}
		}

		// UFunction::Func: every script function shares one native entry (ProcessInternal).
		{
			std::vector<Obj> functions;
			for (int i = 0; i < count && functions.size() < 40000; ++i) {
				const Obj o = ObjectAt(i);
				if (o && Ok(o + L.cls, 4) && ClassOf(o) == functionCls) {
					functions.push_back(o);
				}
			}
			std::size_t bestTop = 0;
			for (int off = L.propertySize + 4; off < L.propertySize + 0xC0 && !functions.empty(); off += 4) {
				std::size_t                                  inCode = 0;
				std::unordered_map<std::uint32_t, std::size_t> histogram;
				for (const Obj f : functions) {
					if (!Ok(f + off, 4)) {
						continue;
					}
					const Addr v = Rd<Addr>(f + off);
					if (InRanges(v, a_code)) {
						++inCode;
						++histogram[v];
					}
				}
				if (inCode < functions.size() * 95 / 100) {
					continue;
				}
				for (const auto& [value, n] : histogram) {
					if (n > bestTop && n >= functions.size() / 5) {
						bestTop = n;
						L.func = off;
						processInternal = value;
					}
				}
			}
			if (L.func >= 0) {
				L.funcFlags = FirstOffset(L.propertySize + 4, L.func + 0x10, { L.func }, [&](int off) {
					std::size_t agree = 0;
					for (const Obj f : functions) {
						if (!Ok(f + off, 4)) {
							continue;
						}
						const bool native = (Rd<std::uint32_t>(f + off) & kFuncNative) != 0;
						if (native == (Rd<Addr>(f + L.func) != processInternal)) {
							++agree;
						}
					}
					return agree >= functions.size() * 97 / 100;
				});
			}
		}
		if (L.func < 0) {
			DC_WARN("UE3: couldn't find UFunction::Func");
			return false;
		}

		// UEnum::Names: Engine.Actor.EPhysics starts with PHYS_None.
		if (const Obj physics = FindObject("Enum", "Engine.Actor.EPhysics")) {
			L.enumNames = FirstOffset(L.next + 4, L.next + 0x40, {}, [&](int off) {
				const auto h = ReadArray(physics + off);
				return h.num >= 3 && h.num <= 256 && h.max >= h.num && h.data && Ok(h.data, 8) && Rd<std::int32_t>(h.data) == pn[8];
			});
		}

		a_layout = L;
		return true;
	}

	bool Discover()
	{
		if (ready) {
			return true;
		}
		mem::FlushCache();
		const auto module = mem::MainModule();
		Arrays     found;
		Layout     layout;
		const int  namesRva = config::Int("Engine", "iGNamesRva", 0);
		const int  objectsRva = config::Int("Engine", "iGObjectsRva", 0);
		if (!FindArrays(module.data, found, layout)) {
			return false;
		}
		if (namesRva > 0) {
			found.names = module.base + static_cast<std::uintptr_t>(namesRva);
		}
		if (objectsRva > 0) {
			found.objects = module.base + static_cast<std::uintptr_t>(objectsRva);
		}
		DC_INFO("UE3: GNames at %s+0x%X (%d names), GObjects at %s+0x%X (%d objects)", "game", static_cast<unsigned>(found.names - module.base),
			ReadArray(found.names).num, "game", static_cast<unsigned>(found.objects - module.base), ReadArray(found.objects).num);
		if (!DetectLayout(found, layout, module.code)) {
			return false;
		}
		ApplyOverride("iLayoutIndex", L.index);
		ApplyOverride("iLayoutOuter", L.outer);
		ApplyOverride("iLayoutName", L.name);
		ApplyOverride("iLayoutClass", L.cls);
		ApplyOverride("iLayoutNext", L.next);
		ApplyOverride("iLayoutSuperField", L.superField);
		ApplyOverride("iLayoutChildren", L.children);
		ApplyOverride("iLayoutPropertySize", L.propertySize);
		ApplyOverride("iLayoutArrayDim", L.arrayDim);
		ApplyOverride("iLayoutElementSize", L.elementSize);
		ApplyOverride("iLayoutOffset", L.offset);
		ApplyOverride("iLayoutBitMask", L.bitMask);
		ApplyOverride("iLayoutStruct", L.structType);
		ApplyOverride("iLayoutFunc", L.func);
		ApplyOverride("iLayoutFuncFlags", L.funcFlags);
		ApplyOverride("iLayoutEnumNames", L.enumNames);
		DC_INFO("UE3: layout %s", L.Describe().c_str());
		DC_INFO("UE3: ProcessInternal at game+0x%X", static_cast<unsigned>(processInternal - module.base));
		if (const int pe = config::Int("Engine", "iProcessEventIndex", 0); pe > 0) {
			processEventIndex = pe;
			DC_INFO("UE3: ProcessEvent vtable slot %d (DisCraft.ini)", pe);
		}
		ready = true;
		return true;
	}

	void Use(const Arrays& a_arrays, const Layout& a_layout)
	{
		arrays = a_arrays;
		L = a_layout;
		nameCache.clear();
		objectCache.clear();
		ready = true;
	}

	bool           Ready() { return ready; }
	const Layout&  GetLayout() { return L; }
	std::uintptr_t ProcessInternal() { return processInternal; }

	// ---- names & objects ---------------------------------------------------------------------

	int NameCount() { return arrays.names ? ReadArray(arrays.names).num : 0; }

	std::string NameString(int a_index) { return EntryText(NameEntry(a_index)); }

	int FindName(std::string_view a_text)
	{
		std::string key(a_text);
		for (auto& c : key) {
			c = static_cast<char>(Fold(static_cast<std::uint8_t>(c)));
		}
		const int         n = NameCount();
		// Hits are final (names are never removed). Misses are kept as -(names checked) - 1 and
		// only retried once the table has grown (a map load adds names).
		int from = 0;
		if (const auto it = nameCache.find(key); it != nameCache.end()) {
			if (it->second >= 0) {
				return it->second;
			}
			from = -it->second - 1;
			if (from >= n) {
				return -1;
			}
		}
		int found = -1;
		for (int i = from; i < n; ++i) {
			const auto e = NameEntry(i);
			if (!e) {
				continue;
			}
			const auto at = e + L.nameText;
			if (EntryIsWide(e) ? MatchWide(at, a_text) : MatchAnsi(at, a_text)) {
				found = i;
				break;
			}
		}
		nameCache[key] = found >= 0 ? found : -n - 1;
		return found;
	}

	int ObjectCount() { return arrays.objects ? ReadArray(arrays.objects).num : 0; }

	Obj ObjectAt(int a_index)
	{
		if (!arrays.objects) {
			return 0;
		}
		const auto h = ReadArray(arrays.objects);
		if (a_index < 0 || a_index >= h.num) {
			return 0;
		}
		return Rd<Addr>(h.data + static_cast<std::uintptr_t>(a_index) * 4);
	}

	bool IsObject(Obj a_object)
	{
		if (!a_object || (a_object & 3) || L.index < 0 || !Ok(a_object, static_cast<std::size_t>(L.index) + 4)) {
			return false;
		}
		const int i = Rd<std::int32_t>(a_object + L.index);
		return ObjectAt(i) == a_object;
	}

	int  IndexOf(Obj a_object) { return a_object ? Rd<std::int32_t>(a_object + L.index) : -1; }
	Obj  OuterOf(Obj a_object) { return a_object ? Rd<Addr>(a_object + L.outer) : 0; }
	Obj  ClassOf(Obj a_object) { return a_object ? Rd<Addr>(a_object + L.cls) : 0; }
	int  NameIndexOf(Obj a_object) { return a_object ? Rd<std::int32_t>(a_object + L.name) : -1; }

	std::string NameOf(Obj a_object)
	{
		if (!a_object) {
			return "None";
		}
		auto       text = NameString(NameIndexOf(a_object));
		const int  number = Rd<std::int32_t>(a_object + L.name + 4);
		if (number > 0) {
			text += "_" + std::to_string(number - 1);
		}
		return text;
	}

	std::string PathOf(Obj a_object)
	{
		std::string path = NameOf(a_object);
		int         guard = 0;
		for (Obj o = OuterOf(a_object); o && guard < 64; o = OuterOf(o), ++guard) {
			path = NameOf(o) + "." + path;
		}
		return path;
	}

	std::string FullNameOf(Obj a_object) { return a_object ? NameOf(ClassOf(a_object)) + " " + PathOf(a_object) : "None"; }

	Obj FindObject(std::string_view a_className, std::string_view a_path)
	{
		std::string key = std::string(a_className) + " " + std::string(a_path);
		if (const auto it = objectCache.find(key); it != objectCache.end()) {
			if (IsObject(it->second)) {
				return it->second;
			}
			objectCache.erase(it);
		}
		std::vector<std::string_view> parts;
		std::size_t                   start = 0;
		while (start <= a_path.size()) {
			auto end = a_path.find('.', start);
			if (end == std::string_view::npos) {
				end = a_path.size();
			}
			parts.push_back(a_path.substr(start, end - start));
			start = end + 1;
		}
		std::vector<int> ids;
		for (const auto p : parts) {
			const int id = FindName(p);
			if (id < 0) {
				return 0;
			}
			ids.push_back(id);
		}
		const int classId = a_className.empty() ? -1 : FindName(a_className);
		if (!a_className.empty() && classId < 0) {
			return 0;
		}
		const int count = ObjectCount();
		for (int i = 0; i < count; ++i) {
			const Obj o = ObjectAt(i);
			if (!o || NameIndexOf(o) != ids.back()) {
				continue;
			}
			if (classId >= 0 && NameIndexOf(ClassOf(o)) != classId) {
				continue;
			}
			Obj  outer = OuterOf(o);
			bool match = true;
			for (int k = static_cast<int>(ids.size()) - 2; k >= 0; --k) {
				if (!outer || NameIndexOf(outer) != ids[static_cast<std::size_t>(k)]) {
					match = false;
					break;
				}
				outer = OuterOf(outer);
			}
			if (match && outer == 0) {
				objectCache.emplace(std::move(key), o);
				return o;
			}
		}
		return 0;
	}

	Obj FindClass(std::string_view a_path) { return FindObject("Class", a_path); }

	Obj SuperOf(Obj a_struct) { return a_struct ? Rd<Addr>(a_struct + L.superField) : 0; }

	bool IsChildOf(Obj a_class, Obj a_base)
	{
		int guard = 0;
		for (Obj c = a_class; c && guard < 256; c = SuperOf(c), ++guard) {
			if (c == a_base) {
				return true;
			}
		}
		return false;
	}

	bool IsA(Obj a_object, Obj a_class) { return a_object && a_class && IsChildOf(ClassOf(a_object), a_class); }

	bool IsDefaultObject(Obj a_object) { return NameString(NameIndexOf(a_object)).starts_with("Default__"); }

	// ---- reflection --------------------------------------------------------------------------

	Field FindField(Obj a_struct, std::string_view a_name)
	{
		Field     out;
		const int id = FindName(a_name);
		if (id < 0) {
			return out;
		}
		int guard = 0;
		for (Obj s = a_struct; s && guard < 256; s = SuperOf(s), ++guard) {
			const Obj f = ChildNamed(s, id);
			if (!f) {
				continue;
			}
			out.kind = NameOf(ClassOf(f));
			if (out.kind.size() < 8 || out.kind.compare(out.kind.size() - 8, 8, "Property") != 0) {
				continue;  // a function or const with that name
			}
			out.prop = f;
			out.offset = Rd<std::int32_t>(f + L.offset);
			out.size = Rd<std::int32_t>(f + L.elementSize);
			out.dim = Rd<std::int32_t>(f + L.arrayDim);
			if (out.kind == "BoolProperty") {
				out.mask = L.bitMask >= 0 ? Rd<std::uint32_t>(f + L.bitMask) : 1u;
			} else if (out.kind == "StructProperty" && L.structType >= 0) {
				out.type = Rd<Addr>(f + L.structType);
			}
			return out;
		}
		return Field{};
	}

	Obj FindFunction(Obj a_class, std::string_view a_name)
	{
		const int id = FindName(a_name);
		if (id < 0) {
			return 0;
		}
		int guard = 0;
		for (Obj s = a_class; s && guard < 256; s = SuperOf(s), ++guard) {
			const Obj f = ChildNamed(s, id);
			if (f && NameOf(ClassOf(f)) == "Function") {
				return f;
			}
		}
		return 0;
	}

	int StructSize(Obj a_struct) { return a_struct ? Rd<std::int32_t>(a_struct + L.propertySize) : 0; }

	int EnumValue(Obj a_enum, std::string_view a_name)
	{
		if (!a_enum || L.enumNames < 0) {
			return -1;
		}
		const int  id = FindName(a_name);
		const auto h = ReadArray(a_enum + L.enumNames);
		for (int i = 0; i < h.num && i < 256; ++i) {
			if (Rd<std::int32_t>(h.data + static_cast<std::uintptr_t>(i) * 8) == id) {
				return i;
			}
		}
		return -1;
	}

	std::vector<Obj> FunctionsNamed(std::string_view a_name)
	{
		std::vector<Obj> out;
		const int        id = FindName(a_name);
		const Obj        functionCls = FindObject("Class", "Core.Function");
		if (id < 0 || !functionCls) {
			return out;
		}
		const int count = ObjectCount();
		for (int i = 0; i < count; ++i) {
			const Obj o = ObjectAt(i);
			if (o && NameIndexOf(o) == id && ClassOf(o) == functionCls) {
				out.push_back(o);
			}
		}
		return out;
	}

	std::vector<Obj> ChildrenOf(Obj a_struct)
	{
		std::vector<Obj> out;
		if (!a_struct) {
			return out;
		}
		int guard = 0;
		for (Obj f = Rd<Addr>(a_struct + L.children); f && guard < 100000; f = Rd<Addr>(f + L.next), ++guard) {
			if (!IsObject(f)) {
				break;
			}
			out.push_back(f);
		}
		return out;
	}

	bool GetBool(Obj a_object, const Field& a_field)
	{
		return a_object && a_field && (Rd<std::uint32_t>(a_object + a_field.offset) & (a_field.mask ? a_field.mask : 1u)) != 0;
	}

	void SetBool(Obj a_object, const Field& a_field, bool a_value)
	{
		if (!a_object || !a_field) {
			return;
		}
		const std::uint32_t mask = a_field.mask ? a_field.mask : 1u;
		std::uint32_t       v = Rd<std::uint32_t>(a_object + a_field.offset);
		v = a_value ? (v | mask) : (v & ~mask);
		mem::Write<std::uint32_t>(a_object + a_field.offset, v);
	}

	// ---- calling functions -------------------------------------------------------------------

	void SetProcessEventIndex(int a_index) { processEventIndex = a_index; }
	int  ProcessEventIndex() { return processEventIndex; }

	namespace
	{
		using ProcessEventFn = void(DC_THISCALL*)(void*, void*, void*, void*);

		struct PendingCall
		{
			ProcessEventFn pe;
			void*          object;
			void*          function;
			void*          parms;
		};

		void DoCall(void* a_call)
		{
			const auto* c = static_cast<PendingCall*>(a_call);
			c->pe(c->object, c->function, c->parms, nullptr);
		}

		// How a native function's FUNC_Native flag is set while ProcessEvent runs it ([Engine]
		// iNativeCallMode): 0 as it is, 1 set, 2 cleared, 3 every flag set (what UE3 SDK generators
		// do). When a call faults, the next mode is tried; when all have, engine calls stop.
		std::vector<int> callModes;
		std::size_t      callMode = 0;
		bool             callsBroken = false;

		std::uint32_t FlagsFor(int a_mode, std::uint32_t a_flags)
		{
			switch (a_mode) {
			case 1:
				return a_flags | kFuncNative;
			case 2:
				return a_flags & ~kFuncNative;
			case 3:
				return a_flags | ~kFuncNative;
			default:
				return a_flags;
			}
		}
	}

	// ---- calling native functions the way script does --------------------------------------------
	// ProcessEvent hands a native function an FFrame whose Code is the function's own script, and
	// Dishonored's natives read nothing useful from it (Trace traced nowhere, SetLocation moved
	// nothing). Script calls a native with its arguments as bytecode in the caller's frame: one
	// EX_LocalVariable <property> per parameter, then EX_EndFunctionParms. DisCraft builds such a
	// frame over the parameter buffer (Locals) and calls the native's Func directly.
	namespace
	{
		constexpr std::uint8_t  kExLocalVariable = 0x00;
		constexpr std::uint8_t  kExNothing = 0x0B;
		constexpr std::uint8_t  kExEndFunctionParms = 0x16;
		constexpr std::uint64_t kCpfParm = 0x80;
		constexpr std::uint64_t kCpfReturnParm = 0x400;

		std::array<std::uint8_t, 0x100> frameTemplate{};
		bool                            haveFrameTemplate = false;
		bool                            nativeDirectBroken = false;

		struct NativeCall
		{
			NativeFn      fn;
			void*         self;
			std::uint8_t* frame;
			void*         result;
		};

		void DoNative(void* a_call)
		{
			const auto* c = static_cast<NativeCall*>(a_call);
			c->fn(c->self, c->frame, c->result);
		}

		// 1 called, 0 not possible here (use ProcessEvent), -1 faulted.
		int CallNative(Obj a_object, Obj a_function, std::uint8_t* a_parms, const std::vector<int>* a_passed)
		{
			if (!haveFrameTemplate || nativeDirectBroken || L.frameNode < 0 || L.elementSize < 0 || L.offset < 0) {
				return 0;
			}
			const auto func = reinterpret_cast<std::uintptr_t>(GetFunc(a_function));
			if (!mem::InCode(func) || func == processInternal) {
				return 0;
			}
			// The parameter list of each native, worked out once.
			struct NativeInfo
			{
				std::vector<std::pair<int, Obj>> ordered;  // (offset, property), in offset order
				int                              returnOffset{ -1 };
				bool                             flagsKnown{ false };
				std::string                      activity;  // for the crash log
			};
			static std::unordered_map<Obj, NativeInfo> infos;
			auto it = infos.find(a_function);
			if (it == infos.end()) {
				NativeInfo info;
				struct Parm
				{
					Obj           prop;
					std::uint64_t flags;
					bool          isReturn;
				};
				std::vector<Parm> parms;
				for (const Obj p : ChildrenOf(a_function)) {
					const auto kind = NameOf(ClassOf(p));
					if (kind.size() < 8 || kind.compare(kind.size() - 8, 8, "Property") != 0) {
						continue;
					}
					const auto flags = Rd<std::uint64_t>(p + L.elementSize + 4);
					parms.push_back({ p, flags, NameOf(p) == "ReturnValue" || (flags & kCpfReturnParm) != 0 });
				}
				// PropertyFlags sit right after ElementSize in every UE3 build seen; if the flags
				// don't look like parameter flags, a native's properties are all parameters anyway.
				for (const auto& p : parms) {
					info.flagsKnown = info.flagsKnown || (p.flags & kCpfParm) != 0;
				}
				// The native reads its parameters in declaration order, which is the order of their
				// offsets in the frame (the Children list needn't be: UE3 SDK generators sort by
				// offset too). Locals, if any, come after the parameters.
				for (const auto& p : parms) {
					const int offset = Rd<std::int32_t>(p.prop + L.offset);
					if (p.isReturn) {
						info.returnOffset = offset;
					} else if (!info.flagsKnown || (p.flags & kCpfParm)) {
						info.ordered.emplace_back(offset, p.prop);
					}
				}
				std::sort(info.ordered.begin(), info.ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
				info.activity = "calling native " + PathOf(a_function);
				it = infos.emplace(a_function, std::move(info)).first;
			}
			const auto& ordered = it->second.ordered;
			const int   returnOffset = it->second.returnOffset;
			const bool  flagsKnown = it->second.flagsKnown;
			// A parameter the call doesn't set is passed as EX_Nothing, the way an omitted optional
			// parameter reaches a native: it keeps its default (and an optional out parameter, such
			// as Trace's HitInfo, is absent). Ending the list early instead is not understood by this
			// engine: the native reads on past EX_EndFunctionParms.
			static std::vector<std::uint8_t> code;  // game thread only
			code.clear();
			std::size_t passed = 0;
			const auto leftOut = [&](int a_offset) { return a_passed && std::find(a_passed->begin(), a_passed->end(), a_offset) == a_passed->end(); };
			for (const auto& [offset, prop] : ordered) {
				if (leftOut(offset)) {
					code.push_back(kExNothing);
					continue;
				}
				code.push_back(kExLocalVariable);
				const Addr ref = prop;
				const auto* bytes = reinterpret_cast<const std::uint8_t*>(&ref);
				code.insert(code.end(), bytes, bytes + sizeof(ref));
				++passed;
			}
			// What was passed, once per function, for the log.
			static std::vector<Obj> described;
			if (described.size() < 8 && std::find(described.begin(), described.end(), a_function) == described.end()) {
				described.push_back(a_function);
				std::string list;
				for (const auto& [offset, prop] : ordered) {
					list += (list.empty() ? "" : ", ") + NameOf(prop) + "@0x" + [](int v) {
						char b[16];
						std::snprintf(b, sizeof(b), "%X", static_cast<unsigned>(v));
						return std::string(b);
					}(offset) + (leftOut(offset) ? " (left out)" : "");
				}
				DC_INFO("UE3: native %s gets (%s), returns at 0x%X", PathOf(a_function).c_str(), list.c_str(), static_cast<unsigned>(returnOffset));
			}
			code.push_back(kExEndFunctionParms);
			code.insert(code.end(), 16, kExNothing);  // never read; harmless if it were

			std::array<std::uint8_t, 0x100> frame{};
			std::memcpy(frame.data(), frameTemplate.data(), static_cast<std::size_t>(L.frameNode));
			const auto put = [&](int a_at, std::uintptr_t a_value) {
				const Addr v = static_cast<Addr>(a_value);
				std::memcpy(frame.data() + a_at, &v, sizeof(v));
			};
			put(L.frameNode, a_function);
			put(L.frameNode + 4, a_object);
			put(L.frameNode + 8, reinterpret_cast<std::uintptr_t>(code.data()));
			put(L.frameNode + 12, reinterpret_cast<std::uintptr_t>(a_parms));
			std::uint8_t scratch[64]{};
			NativeCall   call{ reinterpret_cast<NativeFn>(func), reinterpret_cast<void*>(a_object), frame.data(),
                returnOffset >= 0 ? static_cast<void*>(a_parms + returnOffset) : static_cast<void*>(scratch) };
			seh::SetActivity(it->second.activity.c_str());
			seh::Fault fault;
			const bool ran = seh::Run(&DoNative, &call, fault);
			seh::SetActivity(nullptr);
			if (!ran) {
				DC_ERROR("UE3: calling native %s on %s directly failed: %s; back to ProcessEvent", PathOf(a_function).c_str(), FullNameOf(a_object).c_str(),
					seh::Describe(fault).c_str());
				nativeDirectBroken = true;
				return -1;
			}
			static bool logged = false;
			if (!logged) {
				logged = true;
				DC_INFO("UE3: natives are called directly (first: %s, %zu of %zu parameter(s) passed, flags %s)", PathOf(a_function).c_str(), passed,
					ordered.size(), flagsKnown ? "known" : "not recognised");
			}
			return 1;
		}
	}

	void CaptureFrameTemplate(void* a_frame)
	{
		if (haveFrameTemplate || L.frameNode < 0 || L.frameNode > 0x40) {
			return;
		}
		const auto frame = reinterpret_cast<std::uintptr_t>(a_frame);
		if (!frame || !Ok(frame, static_cast<std::size_t>(L.frameNode))) {
			return;
		}
		std::memcpy(frameTemplate.data(), a_frame, static_cast<std::size_t>(L.frameNode));
		haveFrameTemplate = true;
	}

	bool CallsWork() { return !callsBroken; }

	bool CallFunction(Obj a_object, Obj a_function, void* a_parms, const std::vector<int>* a_passed)
	{
#if defined(_WIN32) && (defined(_M_IX86) || defined(__i386__))
		if (!a_object || !a_function || processEventIndex <= 0 || callsBroken) {
			return false;
		}
		if (callModes.empty()) {
			const int configured = std::clamp(config::Int("Engine", "iNativeCallMode", 0), 0, 3);
			callModes.push_back(configured);
			for (const int m : { 0, 3, 1, 2 }) {
				if (m != configured) {
					callModes.push_back(m);
				}
			}
		}
		if (IsNativeFunction(a_function)) {
			const int direct = CallNative(a_object, a_function, static_cast<std::uint8_t*>(a_parms), a_passed);
			if (direct > 0) {
				return true;
			}
		}
		const Addr vtable = Rd<Addr>(a_object);
		const auto pe = reinterpret_cast<ProcessEventFn>(static_cast<std::uintptr_t>(Rd<Addr>(vtable + static_cast<std::uintptr_t>(processEventIndex) * 4)));
		if (!mem::InCode(reinterpret_cast<std::uintptr_t>(pe))) {
			return false;
		}
		const int           mode = callModes[callMode];
		const bool          toggle = mode != 0 && L.funcFlags >= 0 && IsNativeFunction(a_function);
		const std::uint32_t savedFlags = L.funcFlags >= 0 ? Rd<std::uint32_t>(a_function + L.funcFlags) : 0;
		if (toggle) {
			mem::Write<std::uint32_t>(a_function + L.funcFlags, FlagsFor(mode, savedFlags));
		}
		PendingCall call{ pe, reinterpret_cast<void*>(a_object), reinterpret_cast<void*>(a_function), a_parms };
		seh::Fault  fault;
		const bool  ok = seh::Run(&DoCall, &call, fault);
		if (toggle) {
			mem::Write<std::uint32_t>(a_function + L.funcFlags, savedFlags);
		}
		if (!ok) {
			DC_ERROR("UE3: %s on %s through ProcessEvent (slot %d, %s, flags 0x%08X, native call mode %d) failed: %s", PathOf(a_function).c_str(),
				FullNameOf(a_object).c_str(), processEventIndex, Where(reinterpret_cast<std::uintptr_t>(pe)).c_str(), savedFlags, mode,
				seh::Describe(fault).c_str());
			if (++callMode >= callModes.size()) {
				callsBroken = true;
				DC_ERROR("UE3: every way of calling engine functions failed; DisCraft makes no more engine calls");
			} else {
				DC_WARN("UE3: trying native call mode %d next", callModes[callMode]);
			}
			return false;
		}
		return true;
#else
		(void)a_object;
		(void)a_function;
		(void)a_parms;
		return false;
#endif
	}

	std::string Where(std::uintptr_t a_address) { return seh::Where(a_address); }

	Params::Params(Obj a_function) :
		function_(a_function), buffer_(static_cast<std::size_t>(std::max(StructSize(a_function), 0)) + 16, 0)
	{}

	Params& Params::SetBool(std::string_view a_name, bool a_value)
	{
		const auto f = FindField(function_, a_name);
		if (f && f.offset + 4 <= static_cast<int>(buffer_.size())) {
			std::uint32_t v;
			std::memcpy(&v, buffer_.data() + f.offset, 4);
			const std::uint32_t mask = f.mask ? f.mask : 1u;
			v = a_value ? (v | mask) : (v & ~mask);
			std::memcpy(buffer_.data() + f.offset, &v, 4);
			MarkSet(f.offset);
		}
		return *this;
	}

	bool Params::GetBool(std::string_view a_name) const
	{
		const auto f = FindField(function_, a_name);
		if (!f || f.offset + 4 > static_cast<int>(buffer_.size())) {
			return false;
		}
		std::uint32_t v;
		std::memcpy(&v, buffer_.data() + f.offset, 4);
		return (v & (f.mask ? f.mask : 1u)) != 0;
	}

	Params& Params::SetNumber(std::string_view a_name, double a_value)
	{
		const auto f = FindField(function_, a_name);
		if (!f || f.offset + 4 > static_cast<int>(buffer_.size())) {
			return *this;
		}
		MarkSet(f.offset);
		if (f.kind == "FloatProperty") {
			const float v = static_cast<float>(a_value);
			std::memcpy(buffer_.data() + f.offset, &v, 4);
		} else if (f.kind == "IntProperty") {
			const std::int32_t v = static_cast<std::int32_t>(a_value >= 0 ? a_value + 0.5 : a_value - 0.5);
			std::memcpy(buffer_.data() + f.offset, &v, 4);
		} else if (f.kind == "ByteProperty") {
			buffer_[static_cast<std::size_t>(f.offset)] = static_cast<std::uint8_t>(a_value);
		} else if (f.kind == "BoolProperty") {
			SetBool(a_name, a_value != 0.0);
		}
		return *this;
	}

	double Params::GetNumber(std::string_view a_name) const
	{
		const auto f = FindField(function_, a_name);
		if (!f || f.offset + 4 > static_cast<int>(buffer_.size())) {
			return 0.0;
		}
		if (f.kind == "FloatProperty") {
			float v;
			std::memcpy(&v, buffer_.data() + f.offset, 4);
			return v;
		}
		if (f.kind == "IntProperty") {
			std::int32_t v;
			std::memcpy(&v, buffer_.data() + f.offset, 4);
			return v;
		}
		if (f.kind == "ByteProperty") {
			return buffer_[static_cast<std::size_t>(f.offset)];
		}
		if (f.kind == "BoolProperty") {
			return GetBool(a_name) ? 1.0 : 0.0;
		}
		return 0.0;
	}

	bool Params::Invoke(Obj a_object) { return CallFunction(a_object, function_, buffer_.data(), &set_); }

	// ---- native hooks ------------------------------------------------------------------------

	void* GetFunc(Obj a_function)
	{
		return a_function && L.func >= 0 ? reinterpret_cast<void*>(static_cast<std::uintptr_t>(Rd<Addr>(a_function + L.func))) : nullptr;
	}

	bool SetFunc(Obj a_function, void* a_func)
	{
		if (!a_function || L.func < 0) {
			return false;
		}
		mem::Write<Addr>(a_function + L.func, static_cast<Addr>(reinterpret_cast<std::uintptr_t>(a_func)));
		return true;
	}

	bool IsNativeFunction(Obj a_function)
	{
		if (L.funcFlags >= 0) {
			return (Rd<std::uint32_t>(a_function + L.funcFlags) & kFuncNative) != 0;
		}
		return reinterpret_cast<std::uintptr_t>(GetFunc(a_function)) != processInternal;
	}

	std::uint8_t* FrameLocals(void* a_frame, Obj a_function, Obj a_self)
	{
		const auto frame = reinterpret_cast<std::uintptr_t>(a_frame);
		if (!frame || !Ok(frame, 0x60)) {
			return nullptr;
		}
		if (L.frameNode < 0) {
			for (int k = 0; k + 16 <= 0x60; k += 4) {
				if (Rd<Addr>(frame + k) == a_function && Rd<Addr>(frame + k + 4) == a_self) {
					L.frameNode = k;
					DC_INFO("UE3: FFrame::Node at 0x%X (Locals at 0x%X)", k, k + 12);
					break;
				}
			}
			if (L.frameNode < 0) {
				return nullptr;
			}
		}
		if (Rd<Addr>(frame + L.frameNode) != a_function) {
			return nullptr;
		}
		const Addr locals = Rd<Addr>(frame + L.frameNode + 12);
		return locals && Ok(locals, 4) ? reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(locals)) : nullptr;
	}

	Obj FrameFunction(void* a_frame)
	{
		const auto frame = reinterpret_cast<std::uintptr_t>(a_frame);
		if (!frame || L.frameNode < 0 || !Ok(frame + L.frameNode, 4)) {
			return 0;
		}
		return Rd<Addr>(frame + L.frameNode);
	}

	int FrameCalledFromScript(void* a_frame, Obj a_self)
	{
		const auto frame = reinterpret_cast<std::uintptr_t>(a_frame);
		if (!frame || !Ok(frame, 0x60)) {
			return -1;
		}
		if (L.frameNode < 0) {
			// FFrame: ..., Node, Object, Code, Locals, PreviousFrame. Node is the running function.
			for (int k = 0; k + 20 <= 0x60; k += 4) {
				const Obj node = Rd<Addr>(frame + k);
				if (Rd<Addr>(frame + k + 4) == a_self && IsObject(node) && NameOf(ClassOf(node)) == "Function") {
					L.frameNode = k;
					DC_INFO("UE3: FFrame::Node at 0x%X (Locals at 0x%X, PreviousFrame at 0x%X)", k, k + 12, k + 16);
					break;
				}
			}
			if (L.frameNode < 0) {
				return -1;
			}
		}
		return Rd<Addr>(frame + L.frameNode + 16) != 0 ? 1 : 0;
	}
}
