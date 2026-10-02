#pragma once

#include "../Mem.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if defined(_MSC_VER)
#	define DC_THISCALL __thiscall
#	define DC_FASTCALL __fastcall
#elif defined(__i386__)
#	define DC_THISCALL __attribute__((thiscall))
#	define DC_FASTCALL __attribute__((fastcall))
#else
#	define DC_THISCALL
#	define DC_FASTCALL
#endif

// Unreal Engine 3, from the inside, without an SDK.
//
// Dishonored runs on a 32-bit Unreal Engine 3. Instead of hard-coding addresses and structure
// offsets for one particular Dishonored.exe (they differ between builds and stores), DisCraft finds
// everything at runtime from the engine's own reflection data:
//
//  1. GNames (the name table) is the array in the exe's data whose entries start "None",
//     "ByteProperty", "IntProperty", ... (UE3's hard-coded first names).
//  2. GObjects (every UObject) is the array whose entries each hold their own index.
//  3. The UObject layout (name, class, outer) is the one under which "Class Core.Class" is its own
//     class and "Class Engine.Actor" sits in the package "Engine".
//  4. UStruct/UField/UProperty/UFunction layouts come from structures whose contents are known:
//     Engine.Actor's super class is Core.Object; Core.Object.Vector's X, Y, Z are at 0, 4, 8 and it
//     is 12 bytes; every script function shares one native entry point (UObject::ProcessInternal)...
//
// Everything after that is ordinary reflection: classes, properties and functions by name.
// Every offset can also be forced from DisCraft.ini ([Engine]) if detection ever gets one wrong.
namespace discraft::ue3
{
	using Addr = std::uint32_t;  // an engine pointer as stored in the game's memory (32-bit)
	using Obj = std::uintptr_t;  // an engine object's address (0 = none)

	struct Layout
	{
		// UObject
		int index{ -1 };
		int outer{ -1 };
		int name{ -1 };
		int cls{ -1 };
		// UField
		int next{ -1 };
		// UStruct
		int superField{ -1 };
		int children{ -1 };
		int propertySize{ -1 };
		// UProperty
		int arrayDim{ -1 };
		int elementSize{ -1 };
		int offset{ -1 };
		// UBoolProperty / UStructProperty
		int bitMask{ -1 };
		int structType{ -1 };
		// UFunction
		int func{ -1 };
		int funcFlags{ -1 };
		// UEnum
		int enumNames{ -1 };
		// FNameEntry: where the text starts
		int nameText{ -1 };
		// FFrame (found at the first hooked call): Node; Object at +4, Code +8, Locals +12
		int frameNode{ -1 };

		[[nodiscard]] std::string Describe() const;
	};

	// Addresses of the two static TArrays (GNames, GObjects) in the game's memory.
	struct Arrays
	{
		std::uintptr_t names{ 0 };
		std::uintptr_t objects{ 0 };
	};

	// ---- discovery ---------------------------------------------------------------------------
	// Scans a_ranges (the game's writable sections) for GNames and GObjects; fills a_layout's
	// nameText and index.
	bool FindArrays(const std::vector<mem::Range>& a_ranges, Arrays& a_out, Layout& a_layout);
	// Works out the rest of the layout from the engine's own reflection data. a_code: the game's
	// executable sections (native function pointers point there).
	bool DetectLayout(const Arrays& a_arrays, Layout& a_layout, const std::vector<mem::Range>& a_code);
	// Both, against the running game, with DisCraft.ini overrides applied. Logs what it found.
	bool Discover();
	// Use known arrays and layout directly (tests).
	void Use(const Arrays& a_arrays, const Layout& a_layout);
	[[nodiscard]] bool          Ready();
	[[nodiscard]] const Layout& GetLayout();
	// UObject::ProcessInternal (what every script function's Func is), once detected.
	[[nodiscard]] std::uintptr_t ProcessInternal();

	// ---- names & objects ---------------------------------------------------------------------
	int         NameCount();
	std::string NameString(int a_index);
	int         FindName(std::string_view a_text);  // -1 if the name doesn't exist

	int  ObjectCount();
	Obj  ObjectAt(int a_index);
	bool IsObject(Obj a_object);

	int         IndexOf(Obj a_object);
	Obj         OuterOf(Obj a_object);
	Obj         ClassOf(Obj a_object);
	int         NameIndexOf(Obj a_object);
	std::string NameOf(Obj a_object);
	std::string PathOf(Obj a_object);      // Engine.Actor.Location
	std::string FullNameOf(Obj a_object);  // Class Engine.Actor
	// "Class" + "Engine.Actor", "ScriptStruct" + "Core.Object.Vector", ... (className may be empty).
	Obj FindObject(std::string_view a_className, std::string_view a_path);
	Obj FindClass(std::string_view a_path);
	Obj SuperOf(Obj a_struct);
	bool IsChildOf(Obj a_class, Obj a_base);
	bool IsA(Obj a_object, Obj a_class);
	// True for objects the engine is getting rid of, and for class default objects.
	bool IsDefaultObject(Obj a_object);

	// ---- reflection --------------------------------------------------------------------------
	struct Field
	{
		Obj           prop{ 0 };
		int           offset{ -1 };
		int           size{ 0 };  // one element
		int           dim{ 1 };
		std::uint32_t mask{ 0 };  // BoolProperty
		Obj           type{ 0 };  // StructProperty: its struct
		std::string   kind;       // "IntProperty", "StructProperty", ...

		explicit operator bool() const { return offset >= 0; }
	};

	// A property of a class, struct or function (its parameters), searching super structs too.
	Field FindField(Obj a_struct, std::string_view a_name);
	// A function, searching super classes too (the most derived one wins).
	Obj   FindFunction(Obj a_class, std::string_view a_name);
	int   StructSize(Obj a_struct);
	// An enum's value by name ("PHYS_Flying"), -1 if unknown.
	int   EnumValue(Obj a_enum, std::string_view a_name);
	// Every UFunction object with this name (all classes' overrides).
	std::vector<Obj> FunctionsNamed(std::string_view a_name);
	// A struct's own fields and functions (not its super's), in declaration order.
	std::vector<Obj> ChildrenOf(Obj a_struct);

	template <class T>
	T Get(Obj a_object, const Field& a_field)
	{
		T value{};
		if (a_object && a_field) {
			std::memcpy(&value, reinterpret_cast<const void*>(a_object + a_field.offset), sizeof(T));
		}
		return value;
	}

	template <class T>
	void Set(Obj a_object, const Field& a_field, const T& a_value)
	{
		if (a_object && a_field) {
			std::memcpy(reinterpret_cast<void*>(a_object + a_field.offset), &a_value, sizeof(T));
		}
	}

	inline Obj GetObj(Obj a_object, const Field& a_field) { return Get<Addr>(a_object, a_field); }
	bool       GetBool(Obj a_object, const Field& a_field);
	void       SetBool(Obj a_object, const Field& a_field, bool a_value);

	// ---- calling functions -------------------------------------------------------------------
	// UObject::ProcessEvent's slot in every object's vtable (found at the first hooked call, or
	// [Engine] iProcessEventIndex).
	void SetProcessEventIndex(int a_index);
	int  ProcessEventIndex();
	// Calls a function on an object exactly as the engine's native code calls script events.
	// a_lastParm: a native gets its parameters up to the one at this offset; the rest are left
	// out, as script leaves out trailing optional parameters (they keep their defaults).
	bool CallFunction(Obj a_object, Obj a_function, void* a_parms, int a_lastParm = 0x7FFFFFFF);

	// A function's parameter block, filled and read by parameter name.
	class Params
	{
	public:
		Params() = default;
		explicit Params(Obj a_function);

		[[nodiscard]] bool Valid() const { return function_ != 0; }
		[[nodiscard]] Obj  Function() const { return function_; }
		std::uint8_t*      Data() { return buffer_.data(); }

		template <class T>
		Params& Set(std::string_view a_name, const T& a_value)
		{
			const auto f = FindField(function_, a_name);
			if (f && f.offset + static_cast<int>(sizeof(T)) <= static_cast<int>(buffer_.size())) {
				std::memcpy(buffer_.data() + f.offset, &a_value, sizeof(T));
				lastSet_ = f.offset > lastSet_ ? f.offset : lastSet_;
			}
			return *this;
		}

		template <class T>
		T Get(std::string_view a_name) const
		{
			T          value{};
			const auto f = FindField(function_, a_name);
			if (f && f.offset + static_cast<int>(sizeof(T)) <= static_cast<int>(buffer_.size())) {
				std::memcpy(&value, buffer_.data() + f.offset, sizeof(T));
			}
			return value;
		}

		Params& SetBool(std::string_view a_name, bool a_value);
		bool    GetBool(std::string_view a_name) const;
		// Int, float, byte or bool parameter, whichever it is.
		Params& SetNumber(std::string_view a_name, double a_value);
		double  GetNumber(std::string_view a_name) const;
		bool    Has(std::string_view a_name) const { return static_cast<bool>(FindField(function_, a_name)); }

		bool Invoke(Obj a_object);

	private:
		Obj                       function_{ 0 };
		std::vector<std::uint8_t> buffer_;
		int                       lastSet_{ -1 };  // a native gets the parameters up to the last one set
	};

	// ---- native hooks ------------------------------------------------------------------------
	// UFunction::Func: the native entry point ProcessEvent and the script VM call.
	using NativeFn = void(DC_THISCALL*)(void* a_self, void* a_frame, void* a_result);
	void* GetFunc(Obj a_function);
	bool  SetFunc(Obj a_function, void* a_func);
	bool  IsNativeFunction(Obj a_function);
	// FFrame::Locals of a frame running a_function on a_self (the parameters), or null. The first
	// call works out where FFrame keeps them.
	std::uint8_t* FrameLocals(void* a_frame, Obj a_function, Obj a_self);
	Obj           FrameFunction(void* a_frame);
	// Whether script made this call (the frame has a PreviousFrame) or native code did, through
	// ProcessEvent (it hasn't): 1 or 0, -1 while the frame's layout is unknown.
	int           FrameCalledFromScript(void* a_frame, Obj a_self);
	// False once calls through ProcessEvent have faulted in every native call mode.
	bool          CallsWork();
	// Keeps a copy of a live FFrame's header (its vtable and flags) for calling natives directly.
	void          CaptureFrameTemplate(void* a_frame);
	// "Dishonored.exe+0x1234" for a code address.
	std::string   Where(std::uintptr_t a_address);
}
