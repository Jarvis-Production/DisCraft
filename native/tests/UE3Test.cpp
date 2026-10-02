#include "Check.h"
#include "Coords.h"
#include "FakeEngine.h"
#include "Mem.h"
#include "ue3/UE3.h"

#include <memory>

using namespace discraft;

namespace
{
	ue3::Layout LayoutA()
	{
		// The usual 32-bit UE3 (UDK-era) layout.
		ue3::Layout l;
		l.index = 0x20;
		l.outer = 0x28;
		l.name = 0x2C;
		l.cls = 0x34;
		l.next = 0x3C;
		l.superField = 0x44;
		l.children = 0x48;
		l.propertySize = 0x4C;
		l.arrayDim = 0x40;
		l.elementSize = 0x44;
		l.offset = 0x60;
		l.bitMask = 0x88;
		l.structType = 0x88;
		l.funcFlags = 0x84;
		l.func = 0xA0;
		l.enumNames = 0x40;
		return l;
	}

	ue3::Layout LayoutB()
	{
		// Something else entirely: nothing may be hard-coded.
		ue3::Layout l;
		l.index = 0x24;
		l.outer = 0x30;
		l.name = 0x38;
		l.cls = 0x40;
		l.next = 0x48;
		l.superField = 0x50;
		l.children = 0x54;
		l.propertySize = 0x58;
		l.arrayDim = 0x4C;
		l.elementSize = 0x50;
		l.offset = 0x6C;
		l.bitMask = 0x90;
		l.structType = 0x94;
		l.funcFlags = 0x90;
		l.func = 0xB0;
		l.enumNames = 0x4C;
		return l;
	}

	void Discover(const ue3::Layout& a_truth, int a_textOff)
	{
		auto engine = std::make_unique<fake::Engine>(a_truth, a_textOff);
		fake::Engine::Current() = engine.get();
		mem::SetReadableOverride(&fake::Engine::Readable);
		engine->Build();

		ue3::Arrays arrays;
		ue3::Layout found;
		CHECK(ue3::FindArrays({ { engine->dataSection, engine->dataSection + 0x10000 } }, arrays, found));
		CHECK_EQ(arrays.names, engine->namesAt);
		CHECK_EQ(arrays.objects, engine->objectsAt);
		CHECK_EQ(found.nameText, a_textOff);
		CHECK_EQ(found.index, a_truth.index);

		CHECK(ue3::DetectLayout(arrays, found, { { engine->codeBegin, engine->codeEnd } }));
		std::printf("  detected %s\n", found.Describe().c_str());
		CHECK_EQ(found.outer, a_truth.outer);
		CHECK_EQ(found.name, a_truth.name);
		CHECK_EQ(found.cls, a_truth.cls);
		CHECK_EQ(found.next, a_truth.next);
		CHECK_EQ(found.superField, a_truth.superField);
		CHECK_EQ(found.children, a_truth.children);
		CHECK_EQ(found.propertySize, a_truth.propertySize);
		CHECK_EQ(found.arrayDim, a_truth.arrayDim);
		CHECK_EQ(found.elementSize, a_truth.elementSize);
		CHECK_EQ(found.offset, a_truth.offset);
		CHECK_EQ(found.bitMask, a_truth.bitMask);
		CHECK_EQ(found.structType, a_truth.structType);
		CHECK_EQ(found.func, a_truth.func);
		CHECK_EQ(found.funcFlags, a_truth.funcFlags);
		CHECK_EQ(found.enumNames, a_truth.enumNames);
		CHECK_EQ(ue3::ProcessInternal(), engine->processInternal);

		// Reflection on top of it.
		ue3::Use(arrays, found);
		const auto actor = ue3::FindClass("Engine.Actor");
		const auto pawn = ue3::FindClass("Engine.Pawn");
		const auto pc = ue3::FindClass("Engine.PlayerController");
		CHECK(actor != 0);
		CHECK(pawn != 0);
		CHECK_EQ(ue3::FullNameOf(actor), std::string("Class Engine.Actor"));
		CHECK(ue3::IsChildOf(pc, actor));
		CHECK(!ue3::IsChildOf(actor, pawn));
		const auto loc = ue3::FindField(pawn, "Location");  // inherited from Actor
		CHECK_EQ(loc.offset, 0xC0);
		CHECK_EQ(loc.size, 12);
		CHECK_EQ(loc.kind, std::string("StructProperty"));
		CHECK_EQ(loc.type, ue3::FindObject("ScriptStruct", "Core.Object.Vector"));
		// FNames are case-insensitive.
		CHECK_EQ(ue3::FindField(pawn, "location").offset, 0xC0);
		CHECK(ue3::FindFunction(pawn, "setlocation") != 0);
		const auto hidden = ue3::FindField(actor, "bHidden");
		CHECK_EQ(hidden.mask, 2u);
		CHECK(!ue3::FindField(actor, "NoSuchThing"));
		CHECK(!ue3::FindField(actor, "Tick"));  // a function, not a property
		const auto setLocation = ue3::FindFunction(pawn, "SetLocation");
		CHECK(setLocation != 0);
		CHECK(ue3::IsNativeFunction(setLocation));
		CHECK(!ue3::IsNativeFunction(ue3::FindFunction(pc, "PlayerTick")));
		// The most derived override wins.
		CHECK_EQ(ue3::OuterOf(ue3::FindFunction(pawn, "TakeDamage")), pawn);
		CHECK_EQ(ue3::FunctionsNamed("TakeDamage").size(), std::size_t(2));
		CHECK_EQ(ue3::EnumValue(ue3::FindObject("Enum", "Engine.Actor.EPhysics"), "PHYS_Flying"), 4);

		ue3::Params params(setLocation);
		params.Set("NewLocation", UeVector{ 1.0f, 2.0f, 3.0f }).SetBool("ReturnValue", true);
		float x = 0.0f, z = 0.0f;
		std::memcpy(&x, params.Data(), 4);
		std::memcpy(&z, params.Data() + 8, 4);
		CHECK_NEAR(x, 1.0, 0.0);
		CHECK_NEAR(z, 3.0, 0.0);
		CHECK(params.GetBool("ReturnValue"));

		// A field's value through Get/Set on a live object.
		const auto health = ue3::FindField(pawn, "Health");
		const auto someone = engine->Object("SomePawn", pawn, 0);
		ue3::Set<std::int32_t>(someone, health, 77);
		CHECK_EQ(ue3::Get<std::int32_t>(someone, health), 77);
		ue3::SetBool(someone, hidden, true);
		CHECK(ue3::GetBool(someone, hidden));
		CHECK(!ue3::GetBool(someone, ue3::FindField(actor, "bStatic")));

		// FFrame: a native call (ProcessEvent) has no PreviousFrame, a script call has one.
		{
			const auto tick = ue3::FindFunction(pc, "PlayerTick");
			const auto self = engine->Object("ThePlayerController", pc, 0);
			const auto frame = engine->Alloc(0x60);
			auto       put = [&](int a_off, std::uint32_t a_v) { std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(frame + a_off)), &a_v, 4); };
			put(0x0C, tick);
			put(0x10, self);
			put(0x1C, 0);
			CHECK_EQ(ue3::FrameCalledFromScript(reinterpret_cast<void*>(static_cast<std::uintptr_t>(frame)), self), 0);
			put(0x1C, frame + 0x40);
			CHECK_EQ(ue3::FrameCalledFromScript(reinterpret_cast<void*>(static_cast<std::uintptr_t>(frame)), self), 1);
			CHECK_EQ(ue3::FrameFunction(reinterpret_cast<void*>(static_cast<std::uintptr_t>(frame))), tick);
		}

		mem::SetReadableOverride(nullptr);
		fake::Engine::Current() = nullptr;
	}
}

TEST(UE3DiscoversUdkLayout) { Discover(LayoutA(), 0x10); }

TEST(UE3DiscoversShiftedLayout) { Discover(LayoutB(), 0x14); }
