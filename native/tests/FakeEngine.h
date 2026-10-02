#pragma once

// A miniature fake of Unreal Engine 3's object system in 32-bit-addressable memory, laid out by a
// chosen Layout. The discovery code is run against it and must find that layout again.

#include "ue3/UE3.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <sys/mman.h>

namespace fake
{
	using discraft::ue3::Addr;
	using discraft::ue3::Layout;

	class Engine
	{
	public:
		static constexpr std::size_t kArenaBytes = 64u << 20;

		explicit Engine(const Layout& a_layout, int a_nameEntryTextOffset) :
			L(a_layout), textOff(a_nameEntryTextOffset)
		{
			void* p = ::mmap(nullptr, kArenaBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
			if (p == MAP_FAILED) {
				std::abort();
			}
			arena = static_cast<std::uint8_t*>(p);
			arenaBase = reinterpret_cast<std::uintptr_t>(p);
			std::memset(arena, 0, kArenaBytes);
			// The "exe's data section": 64 KB with the two arrays somewhere in it, plus decoys.
			dataSection = Alloc(0x10000);
			// The "exe's code section": just a range of addresses native function pointers point into.
			codeBegin = Alloc(0x10000);
			codeEnd = codeBegin + 0x10000;
			processInternal = codeBegin + 0x1230;
		}

		~Engine() { ::munmap(arena, kArenaBytes); }

		static Engine*& Current()
		{
			static Engine* current = nullptr;
			return current;
		}

		static bool Readable(std::uintptr_t a_addr, std::size_t a_size)
		{
			const Engine* e = Current();
			return e && a_addr >= e->arenaBase && a_addr + a_size <= e->arenaBase + e->used;
		}

		Addr Alloc(std::size_t a_bytes)
		{
			used = (used + 15) & ~std::size_t(15);
			const Addr at = static_cast<Addr>(arenaBase + used);
			used += a_bytes;
			if (used > kArenaBytes) {
				std::abort();
			}
			return at;
		}

		template <class T>
		void Put(std::uintptr_t a_at, const T& a_value)
		{
			std::memcpy(reinterpret_cast<void*>(a_at), &a_value, sizeof(T));
		}

		int Name(const std::string& a_text)
		{
			if (const auto it = nameIds.find(a_text); it != nameIds.end()) {
				return it->second;
			}
			const int  id = static_cast<int>(names.size());
			const Addr entry = Alloc(static_cast<std::size_t>(textOff) + a_text.size() + 1);
			// FNameEntry: flags (8), index << 1, hash next, then the text at textOff.
			Put<std::int32_t>(entry + 8, id << 1);
			std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(entry) + textOff), a_text.c_str(), a_text.size() + 1);
			names.push_back(entry);
			nameIds.emplace(a_text, id);
			return id;
		}

		Addr Object(const std::string& a_name, Addr a_class, Addr a_outer)
		{
			const Addr o = Alloc(0x200);
			Put<Addr>(o, vtable);
			Put<std::int32_t>(o + L.index, static_cast<std::int32_t>(objects.size()));
			Put<Addr>(o + L.outer, a_outer);
			Put<std::int32_t>(o + L.name, Name(a_name));
			Put<std::int32_t>(o + L.name + 4, 0);
			Put<Addr>(o + L.cls, a_class);
			objects.push_back(o);
			return o;
		}

		void AddChild(Addr a_struct, Addr a_field)
		{
			Addr* tail = &lastChild[a_struct];
			if (!*tail) {
				Put<Addr>(a_struct + L.children, a_field);
			} else {
				Put<Addr>(*tail + L.next, a_field);
			}
			*tail = a_field;
		}

		Addr Property(Addr a_owner, const std::string& a_name, const std::string& a_kind, int a_offset, int a_size)
		{
			const Addr p = Object(a_name, Class(a_kind), a_owner);
			Put<std::int32_t>(p + L.arrayDim, 1);
			Put<std::int32_t>(p + L.elementSize, a_size);
			Put<std::int32_t>(p + L.offset, a_offset);
			AddChild(a_owner, p);
			return p;
		}

		Addr Bool(Addr a_owner, const std::string& a_name, int a_offset, std::uint32_t a_mask)
		{
			const Addr p = Property(a_owner, a_name, "BoolProperty", a_offset, 4);
			Put<std::uint32_t>(p + L.bitMask, a_mask);
			return p;
		}

		Addr Function(Addr a_owner, const std::string& a_name, bool a_native, int a_parmsSize)
		{
			const Addr f = Object(a_name, Class("Function"), a_owner);
			Put<std::int32_t>(f + L.propertySize, a_parmsSize);
			Put<std::uint32_t>(f + L.funcFlags, a_native ? 0x401u : 0x1u);
			Put<Addr>(f + L.func, a_native ? static_cast<Addr>(codeBegin + 0x2000 + 16 * (nativeCount++)) : static_cast<Addr>(processInternal));
			AddChild(a_owner, f);
			return f;
		}

		Addr Class(const std::string& a_name) { return classes.at(a_name); }

		void Build()
		{
			Name("None");
			for (const char* n : { "ByteProperty", "IntProperty", "BoolProperty", "FloatProperty", "ObjectProperty", "NameProperty", "DelegateProperty",
					 "ClassProperty", "ArrayProperty", "StructProperty", "VectorProperty", "RotatorProperty", "StrProperty", "MapProperty",
					 "InterfaceProperty" }) {
				Name(n);
			}
			vtable = Alloc(0x400);

			const Addr core = Object("Core", 0, 0);
			const Addr engine = Object("Engine", 0, 0);
			const Addr classClass = Object("Class", 0, core);
			Put<Addr>(classClass + L.cls, classClass);
			classes["Class"] = classClass;
			const auto makeClass = [&](const std::string& n, Addr outer, Addr super) {
				const Addr c = Object(n, classClass, outer);
				Put<Addr>(c + L.superField, super);
				classes[n] = c;
				return c;
			};
			const Addr objectCls = makeClass("Object", core, 0);
			const Addr packageCls = makeClass("Package", core, objectCls);
			Put<Addr>(core + L.cls, packageCls);
			Put<Addr>(engine + L.cls, packageCls);
			const Addr fieldCls = makeClass("Field", core, objectCls);
			const Addr structCls = makeClass("Struct", core, fieldCls);
			makeClass("Function", core, structCls);
			makeClass("ScriptStruct", core, structCls);
			makeClass("Enum", core, fieldCls);
			makeClass("Const", core, fieldCls);
			const Addr propCls = makeClass("Property", core, fieldCls);
			for (const char* k : { "BoolProperty", "IntProperty", "FloatProperty", "ByteProperty", "StructProperty", "ObjectProperty", "NameProperty" }) {
				makeClass(k, core, propCls);
			}
			const Addr actorCls = makeClass("Actor", engine, objectCls);
			const Addr pawnCls = makeClass("Pawn", engine, actorCls);
			const Addr controllerCls = makeClass("Controller", engine, actorCls);
			const Addr pcCls = makeClass("PlayerController", engine, controllerCls);

			// Core.Object's own members (a few).
			Property(objectCls, "ObjectInternalInteger", "IntProperty", L.index, 4);
			Property(objectCls, "Outer", "ObjectProperty", L.outer, 4);
			const Addr vec = Object("Vector", Class("ScriptStruct"), objectCls);
			Put<std::int32_t>(vec + L.propertySize, 12);
			AddChild(objectCls, vec);
			Property(vec, "X", "FloatProperty", 0, 4);
			Property(vec, "Y", "FloatProperty", 4, 4);
			Property(vec, "Z", "FloatProperty", 8, 4);
			const Addr rot = Object("Rotator", Class("ScriptStruct"), objectCls);
			Put<std::int32_t>(rot + L.propertySize, 12);
			AddChild(objectCls, rot);
			Property(rot, "Pitch", "IntProperty", 0, 4);
			Property(rot, "Yaw", "IntProperty", 4, 4);
			Property(rot, "Roll", "IntProperty", 8, 4);
			const Addr plane = Object("Plane", Class("ScriptStruct"), objectCls);
			Put<std::int32_t>(plane + L.propertySize, 16);
			Put<Addr>(plane + L.superField, vec);
			AddChild(objectCls, plane);
			Property(plane, "W", "FloatProperty", 12, 4);
			Function(objectCls, "Tick", false, 4);

			// Engine.Actor.
			const Addr loc = Property(actorCls, "Location", "StructProperty", 0xC0, 12);
			Put<Addr>(loc + L.structType, vec);
			const Addr rotation = Property(actorCls, "Rotation", "StructProperty", 0xCC, 12);
			Put<Addr>(rotation + L.structType, rot);
			Bool(actorCls, "bStatic", 0x100, 1);
			Bool(actorCls, "bHidden", 0x100, 2);
			Bool(actorCls, "bNoDelete", 0x100, 4);
			Bool(actorCls, "bDeleteMe", 0x100, 8);
			Bool(actorCls, "bCollideWorld", 0x104, 1);
			Bool(actorCls, "bBlockActors", 0x104, 2);
			Property(actorCls, "Physics", "ByteProperty", 0x108, 1);
			Property(actorCls, "Owner", "ObjectProperty", 0x10C, 4);
			const Addr physics = Object("EPhysics", Class("Enum"), actorCls);
			AddChild(actorCls, physics);
			{
				const char* entries[] = { "PHYS_None", "PHYS_Walking", "PHYS_Falling", "PHYS_Swimming", "PHYS_Flying", "PHYS_Rotating" };
				const Addr  data = Alloc(8 * 6);
				for (int i = 0; i < 6; ++i) {
					Put<std::int32_t>(data + 8 * i, Name(entries[i]));
					Put<std::int32_t>(data + 8 * i + 4, 0);
				}
				Put<Addr>(physics + L.enumNames, data);
				Put<std::int32_t>(physics + L.enumNames + 4, 6);
				Put<std::int32_t>(physics + L.enumNames + 8, 6);
			}
			const Addr setLocation = Function(actorCls, "SetLocation", true, 16);
			Put<Addr>(Property(setLocation, "NewLocation", "StructProperty", 0, 12) + L.structType, vec);
			Bool(setLocation, "ReturnValue", 12, 1);
			Function(actorCls, "Tick", false, 4);
			Function(actorCls, "Touch", false, 32);
			Function(actorCls, "TakeDamage", false, 64);

			// Engine.Pawn / Controller / PlayerController.
			Property(pawnCls, "Health", "IntProperty", 0x200, 4);
			Property(pawnCls, "Controller", "ObjectProperty", 0x204, 4);
			Function(pawnCls, "TakeDamage", false, 64);
			Property(controllerCls, "Pawn", "ObjectProperty", 0x150, 4);
			Function(pcCls, "PlayerTick", false, 4);

			// Plenty of other script and native functions, and filler objects (a real game has
			// hundreds of thousands).
			for (int i = 0; i < 300; ++i) {
				Function(objectCls, "ScriptFn" + std::to_string(i), false, 8);
			}
			for (int i = 0; i < 120; ++i) {
				Function(objectCls, "NativeFn" + std::to_string(i), true, 8);
			}
			for (int i = 0; i < 1200; ++i) {
				Name("FillerName" + std::to_string(i));
			}
			while (objects.size() < 6000) {
				Object("Filler" + std::to_string(objects.size() % 977), packageCls, objects.size() % 3 ? engine : 0);
			}

			// The arrays: TArray<FNameEntry*> and TArray<UObject*>, placed among decoy arrays.
			const Addr nameData = Alloc(names.size() * 4);
			std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(nameData)), names.data(), names.size() * 4);
			const Addr objectData = Alloc(objects.size() * 4);
			std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(objectData)), objects.data(), objects.size() * 4);
			// A decoy: an array of the same objects in a different order (no index match).
			const Addr decoyData = Alloc(objects.size() * 4);
			for (std::size_t i = 0; i < objects.size(); ++i) {
				Put<Addr>(decoyData + 4 * i, objects[(i * 7 + 3) % objects.size()]);
			}
			const auto putArray = [&](std::uintptr_t at, Addr data, std::size_t n) {
				Put<Addr>(at, data);
				Put<std::int32_t>(at + 4, static_cast<std::int32_t>(n));
				Put<std::int32_t>(at + 8, static_cast<std::int32_t>(n + 64));
			};
			putArray(dataSection + 0x1000, decoyData, objects.size());
			namesAt = dataSection + 0x3A24;
			objectsAt = dataSection + 0x7F18;
			putArray(namesAt, nameData, names.size());
			putArray(objectsAt, objectData, objects.size());
		}

		Layout                      L;
		int                         textOff;
		std::uint8_t*               arena{ nullptr };
		std::uintptr_t              arenaBase{ 0 };
		std::size_t                 used{ 0 };
		std::uintptr_t              dataSection{ 0 };
		std::uintptr_t              codeBegin{ 0 }, codeEnd{ 0 }, processInternal{ 0 };
		std::uintptr_t              namesAt{ 0 }, objectsAt{ 0 };
		Addr                        vtable{ 0 };
		std::vector<Addr>           names;
		std::map<std::string, int>  nameIds;
		std::vector<Addr>           objects;
		std::map<std::string, Addr> classes;
		std::map<Addr, Addr>        lastChild;
		int                         nativeCount{ 0 };
	};
}
