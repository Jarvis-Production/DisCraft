#include "Bindings.h"

#include "Log.h"

#include <map>
#include <string>

namespace discraft::bind
{
	Classes   C;
	Fields    F;
	Functions Fn;

	namespace
	{
		Field Need(Obj a_class, const char* a_name, bool& a_ok)
		{
			auto f = ue3::FindField(a_class, a_name);
			if (!f) {
				DC_ERROR("engine: %s.%s not found", ue3::NameOf(a_class).c_str(), a_name);
				a_ok = false;
			}
			return f;
		}

		Field Want(Obj a_class, std::initializer_list<const char*> a_names)
		{
			for (const char* name : a_names) {
				if (auto f = ue3::FindField(a_class, name)) {
					return f;
				}
			}
			DC_WARN("engine: %s.%s not found (optional)", ue3::NameOf(a_class).c_str(), *a_names.begin());
			return {};
		}

		Obj NeedFn(Obj a_class, const char* a_name, bool& a_ok)
		{
			const Obj f = ue3::FindFunction(a_class, a_name);
			if (!f) {
				DC_ERROR("engine: function %s.%s not found", ue3::NameOf(a_class).c_str(), a_name);
				a_ok = false;
			}
			return f;
		}

		// The struct a StructProperty holds: from the property, else by the struct's name.
		Obj StructOf(const Field& a_field, Obj a_owner, const char* a_structName)
		{
			if (a_field.type) {
				return a_field.type;
			}
			return ue3::FindObject("ScriptStruct", ue3::PathOf(a_owner) + "." + a_structName);
		}
	}

	bool Bind()
	{
		bool ok = true;
		C.actor = ue3::FindClass("Engine.Actor");
		C.pawn = ue3::FindClass("Engine.Pawn");
		C.controller = ue3::FindClass("Engine.Controller");
		C.playerController = ue3::FindClass("Engine.PlayerController");
		C.camera = ue3::FindClass("Engine.Camera");
		C.worldInfo = ue3::FindClass("Engine.WorldInfo");
		C.cylinder = ue3::FindClass("Engine.CylinderComponent");
		C.damageType = ue3::FindClass("Engine.DamageType");
		if (!C.actor || !C.pawn || !C.controller || !C.playerController) {
			DC_ERROR("engine: core classes missing (Actor %d, Pawn %d, Controller %d, PlayerController %d)", C.actor != 0, C.pawn != 0,
				C.controller != 0, C.playerController != 0);
			return false;
		}

		F.location = Need(C.actor, "Location", ok);
		F.rotation = Need(C.actor, "Rotation", ok);
		F.velocity = Want(C.actor, { "Velocity" });
		F.acceleration = Want(C.actor, { "Acceleration" });
		F.physics = Want(C.actor, { "Physics" });
		F.worldInfo = Want(C.actor, { "WorldInfo" });
		F.collideWorld = Want(C.actor, { "bCollideWorld" });
		F.deleteMe = Want(C.actor, { "bDeleteMe" });
		F.collisionComponent = Want(C.actor, { "CollisionComponent" });
		F.owner = Want(C.actor, { "Owner" });
		F.base = Want(C.actor, { "Base" });
		F.hidden = Want(C.actor, { "bHidden" });
		F.instigator = Want(C.actor, { "Instigator" });
		F.components = Want(C.actor, { "Components" });
		F.isStatic = Want(C.actor, { "bStatic" });

		F.pawnController = Need(C.pawn, "Controller", ok);
		F.health = Need(C.pawn, "Health", ok);
		F.healthMax = Want(C.pawn, { "HealthMax" });
		F.cylinder = Want(C.pawn, { "CylinderComponent" });
		F.nextPawn = Want(C.pawn, { "NextPawn" });
		F.eyeHeight = Want(C.pawn, { "EyeHeight" });
		F.baseEyeHeight = Want(C.pawn, { "BaseEyeHeight" });
		F.isCrouched = Want(C.pawn, { "bIsCrouched" });

		F.controllerPawn = Need(C.controller, "Pawn", ok);
		F.enemy = Want(C.controller, { "Enemy" });
		F.playerCamera = Want(C.playerController, { "PlayerCamera" });
		F.cinematicMode = Want(C.playerController, { "bCinematicMode" });
		F.ignoreMoveInput = Want(C.playerController, { "bIgnoreMoveInput", "IgnoreMoveInput" });
		F.ignoreLookInput = Want(C.playerController, { "bIgnoreLookInput", "IgnoreLookInput" });
		F.myHud = Want(C.playerController, { "myHUD" });
		C.hud = ue3::FindClass("Engine.HUD");
		if (C.hud) {
			F.showHud = Want(C.hud, { "bShowHUD" });
		}
		C.primitive = ue3::FindClass("Engine.PrimitiveComponent");
		if (C.primitive) {
			F.hiddenGame = Want(C.primitive, { "HiddenGame" });
			Fn.setComponentHidden = ue3::FindFunction(C.primitive, "SetHidden");
		}

		if (C.camera) {
			const auto cache = ue3::FindField(C.camera, "CameraCache");
			const Obj  cacheStruct = cache ? StructOf(cache, C.camera, "TCameraCache") : 0;
			const auto pov = cacheStruct ? ue3::FindField(cacheStruct, "POV") : Field{};
			const Obj  povStruct = pov ? StructOf(pov, C.camera, "TPOV") : 0;
			if (povStruct) {
				const auto loc = ue3::FindField(povStruct, "Location");
				const auto rot = ue3::FindField(povStruct, "Rotation");
				const auto fov = ue3::FindField(povStruct, "FOV");
				if (loc && rot && fov) {
					F.povLocation = cache.offset + pov.offset + loc.offset;
					F.povRotation = cache.offset + pov.offset + rot.offset;
					F.povFov = cache.offset + pov.offset + fov.offset;
				}
			}
			if (F.povLocation < 0) {
				DC_WARN("engine: Camera.CameraCache.POV not found; blocks are drawn from the player's eye instead");
			}
		}
		if (C.worldInfo) {
			F.pauser = Want(C.worldInfo, { "Pauser" });
			F.pawnList = Want(C.worldInfo, { "PawnList" });
		}
		if (C.cylinder) {
			F.collisionHeight = Want(C.cylinder, { "CollisionHeight" });
			F.collisionRadius = Want(C.cylinder, { "CollisionRadius" });
		}

		Fn.setLocation = NeedFn(C.actor, "SetLocation", ok);
		Fn.setRotation = ue3::FindFunction(C.actor, "SetRotation");
		Fn.setPhysics = ue3::FindFunction(C.actor, "SetPhysics");
		Fn.trace = NeedFn(C.actor, "Trace", ok);
		Fn.hurtRadius = ue3::FindFunction(C.actor, "HurtRadius");
		Fn.died = ue3::FindFunction(C.pawn, "Died");
		Fn.setHidden = ue3::FindFunction(C.actor, "SetHidden");
		Fn.fastTrace = ue3::FindFunction(C.actor, "FastTrace");
		Fn.vsize = ue3::FindFunction(C.actor, "VSize");
		if (const Obj physics = ue3::FindObject("Enum", "Engine.Actor.EPhysics")) {
			const int flying = ue3::EnumValue(physics, "PHYS_Flying");
			const int walking = ue3::EnumValue(physics, "PHYS_Walking");
			const int none = ue3::EnumValue(physics, "PHYS_None");
			Fn.physFlying = flying >= 0 ? flying : Fn.physFlying;
			Fn.physWalking = walking >= 0 ? walking : Fn.physWalking;
			Fn.physNone = none >= 0 ? none : Fn.physNone;
		}
		DC_INFO("engine: Actor.Location @0x%X, Pawn.Health @0x%X, Controller.Pawn @0x%X, camera POV @0x%X, PHYS_Flying = %d", F.location.offset,
			F.health.offset, F.controllerPawn.offset, F.povLocation, Fn.physFlying);
		return ok;
	}

	Obj FunctionOf(Obj a_object, const char* a_name)
	{
		static std::map<std::pair<Obj, std::string>, Obj> cache;
		const Obj                                            cls = ue3::ClassOf(a_object);
		const auto                                           key = std::make_pair(cls, std::string(a_name));
		if (const auto it = cache.find(key); it != cache.end()) {
			return it->second;
		}
		const Obj f = ue3::FindFunction(cls, a_name);
		cache.emplace(key, f);
		return f;
	}

	Field FieldOf(Obj a_object, const char* a_name)
	{
		static std::map<std::pair<Obj, std::string>, Field> cache;
		const Obj                                             cls = ue3::ClassOf(a_object);
		const auto                                            key = std::make_pair(cls, std::string(a_name));
		if (const auto it = cache.find(key); it != cache.end()) {
			return it->second;
		}
		const auto f = ue3::FindField(cls, a_name);
		cache.emplace(key, f);
		return f;
	}

	Vec3f Location(Obj a_actor) { return ue3::Get<Vec3f>(a_actor, F.location); }
	Rot3i Rotation(Obj a_actor) { return ue3::Get<Rot3i>(a_actor, F.rotation); }

	float HalfHeight(Obj a_pawn)
	{
		const Obj cyl = ue3::GetObj(a_pawn, F.cylinder);
		return cyl ? ue3::Get<float>(cyl, F.collisionHeight) : 0.0f;
	}

	float Radius(Obj a_pawn)
	{
		const Obj cyl = ue3::GetObj(a_pawn, F.cylinder);
		return cyl ? ue3::Get<float>(cyl, F.collisionRadius) : 0.0f;
	}

	bool IsDying(Obj a_actor) { return !a_actor || ue3::GetBool(a_actor, F.deleteMe); }
}
