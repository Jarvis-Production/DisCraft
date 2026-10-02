#pragma once

#include "ue3/UE3.h"

// The engine classes, properties and functions DisCraft uses, looked up by name once UE3 discovery
// has run. Everything here is stock Unreal Engine 3 (Engine.*): Dishonored's own classes derive
// from these, so nothing depends on Dishonored's script code.
namespace discraft::bind
{
	using ue3::Field;
	using ue3::Obj;

	struct Classes
	{
		Obj actor{ 0 }, pawn{ 0 }, controller{ 0 }, playerController{ 0 }, camera{ 0 }, worldInfo{ 0 }, cylinder{ 0 }, damageType{ 0 };
	};

	struct Fields
	{
		// Actor
		Field location, rotation, velocity, acceleration, physics, worldInfo, collideWorld, deleteMe, collisionComponent, owner, base, hidden;
		// Pawn
		Field pawnController, health, healthMax, cylinder, nextPawn, eyeHeight, baseEyeHeight, isCrouched;
		// Controller / PlayerController
		Field controllerPawn, enemy, playerCamera, cinematicMode, ignoreMoveInput, ignoreLookInput, myHud;
		// HUD
		Field showHud;
		// Camera: CameraCache.POV.{Location, Rotation, FOV} as absolute offsets in the camera actor
		int povLocation{ -1 }, povRotation{ -1 }, povFov{ -1 };
		// WorldInfo
		Field pauser, pawnList;
		// CylinderComponent
		Field collisionHeight, collisionRadius;
	};

	struct Functions
	{
		Obj setLocation{ 0 }, setRotation{ 0 }, setPhysics{ 0 }, trace{ 0 }, hurtRadius{ 0 }, died{ 0 }, setHidden{ 0 };
		int physFlying{ 4 }, physWalking{ 1 }, physNone{ 0 };
	};

	extern Classes   C;
	extern Fields    F;
	extern Functions Fn;

	// Looks everything up. False if something DisCraft can't work without is missing (logged).
	bool Bind();

	// A function of the object's own (most derived) class, e.g. Dishonored's override of Use.
	Obj FunctionOf(Obj a_object, const char* a_name);
	// A field of the object's own class (properties Dishonored's classes add), cached per class.
	Field FieldOf(Obj a_object, const char* a_name);

	// Convenience reads (zero/false for a null object).
	struct Vec3f
	{
		float x{ 0 }, y{ 0 }, z{ 0 };
	};
	struct Rot3i
	{
		std::int32_t pitch{ 0 }, yaw{ 0 }, roll{ 0 };
	};
	Vec3f Location(Obj a_actor);
	Rot3i Rotation(Obj a_actor);
	// A pawn's collision cylinder (half height, radius) in Unreal units; 0 if unknown.
	float HalfHeight(Obj a_pawn);
	float Radius(Obj a_pawn);
	bool  IsDying(Obj a_actor);
}
