#pragma once

#include <cmath>
#include <cstdint>

// Unreal Engine 3 space <-> Minecraft space.
//
// UE3 is left-handed and Z-up: X forward (call it north), Y right (east), Z up, in Unreal units.
// Minecraft is right-handed and Y-up: X east, Y up, Z south, in blocks. So
//
//   mc.x =  ue.y / s      mc.y = ue.z / s      mc.z = -ue.x / s
//
// with s Unreal units per block. The map has determinant -1, which is exactly the handedness flip:
// the world looks the same in both games (nothing is mirrored).
//
// Rotations: UE3 rotators are integers, 65536 per turn. UE yaw 0 faces +X (north) and grows
// clockwise seen from above (towards +Y, east); Minecraft yaw 0 faces south and also grows
// clockwise, so mc.yaw = ue.yaw + 180 degrees. UE pitch is positive looking up, Minecraft's looking
// down: mc.pitch = -ue.pitch.
namespace discraft
{
	struct Vec3d
	{
		double x{ 0.0 }, y{ 0.0 }, z{ 0.0 };
	};

	struct UeVector
	{
		float x{ 0.0f }, y{ 0.0f }, z{ 0.0f };
	};

	struct UeRotator
	{
		std::int32_t pitch{ 0 }, yaw{ 0 }, roll{ 0 };
	};

	inline constexpr double kUeUnitsPerTurn = 65536.0;

	inline Vec3d UeToMc(const UeVector& a_v, double a_unitsPerBlock)
	{
		return { a_v.y / a_unitsPerBlock, a_v.z / a_unitsPerBlock, -a_v.x / a_unitsPerBlock };
	}

	inline UeVector McToUe(double a_x, double a_y, double a_z, double a_unitsPerBlock)
	{
		return { static_cast<float>(-a_z * a_unitsPerBlock), static_cast<float>(a_x * a_unitsPerBlock), static_cast<float>(a_y * a_unitsPerBlock) };
	}

	// A direction (no scale): UE -> MC.
	inline Vec3d UeDirToMc(double a_x, double a_y, double a_z) { return { a_y, a_z, -a_x }; }

	// Wraps to [-180, 180).
	inline double WrapDegrees(double a_deg)
	{
		double d = std::fmod(a_deg + 180.0, 360.0);
		if (d < 0.0) {
			d += 360.0;
		}
		return d - 180.0;
	}

	// A rotator component (any integer, wraps at 65536) as signed degrees in [-180, 180).
	inline double UeAngleToDegrees(std::int32_t a_angle)
	{
		const auto wrapped = static_cast<std::int16_t>(static_cast<std::uint16_t>(a_angle & 0xFFFF));
		return wrapped * (360.0 / kUeUnitsPerTurn);
	}

	inline std::int32_t DegreesToUeAngle(double a_deg)
	{
		return static_cast<std::int32_t>(std::lround(WrapDegrees(a_deg) * (kUeUnitsPerTurn / 360.0))) & 0xFFFF;
	}

	inline float UeYawToMc(std::int32_t a_yaw) { return static_cast<float>(WrapDegrees(UeAngleToDegrees(a_yaw) + 180.0)); }
	inline float UePitchToMc(std::int32_t a_pitch) { return static_cast<float>(-UeAngleToDegrees(a_pitch)); }
	inline std::int32_t McYawToUe(float a_yaw) { return DegreesToUeAngle(a_yaw - 180.0); }
	// UE stores pitch as 0..65535 (looking down is just under 65536).
	inline std::int32_t McPitchToUe(float a_pitch) { return DegreesToUeAngle(-a_pitch); }

	// Minecraft's look vector for a yaw/pitch (degrees), as Entity.calculateViewVector.
	inline Vec3d McLookVector(double a_yawDeg, double a_pitchDeg)
	{
		const double y = a_yawDeg * 0.017453292519943295;
		const double p = a_pitchDeg * 0.017453292519943295;
		return { -std::sin(y) * std::cos(p), -std::sin(p), std::cos(y) * std::cos(p) };
	}

	// A UE rotator's forward vector, in Minecraft axes.
	inline Vec3d UeRotatorForwardMc(const UeRotator& a_r)
	{
		const double p = UeAngleToDegrees(a_r.pitch) * 0.017453292519943295;
		const double y = UeAngleToDegrees(a_r.yaw) * 0.017453292519943295;
		return UeDirToMc(std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p));
	}
}
