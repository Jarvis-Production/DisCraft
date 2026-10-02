#include "Check.h"
#include "Coords.h"

using namespace discraft;

TEST(CoordsRoundTrip)
{
	const double s = 50.0;
	const auto   mc = UeToMc({ 1000.0f, -250.0f, 75.0f }, s);
	CHECK_NEAR(mc.x, -5.0, 1e-9);  // UE Y (east) -> MC X
	CHECK_NEAR(mc.y, 1.5, 1e-9);   // UE Z (up) -> MC Y
	CHECK_NEAR(mc.z, -20.0, 1e-9); // UE X (north) -> MC -Z (north)
	const auto ue = McToUe(mc.x, mc.y, mc.z, s);
	CHECK_NEAR(ue.x, 1000.0, 1e-3);
	CHECK_NEAR(ue.y, -250.0, 1e-3);
	CHECK_NEAR(ue.z, 75.0, 1e-3);
}

TEST(CoordsNotMirrored)
{
	// Turning right (clockwise from above) is turning right in both games: north then east.
	const auto north = UeDirToMc(1, 0, 0);
	const auto east = UeDirToMc(0, 1, 0);
	const auto up = UeDirToMc(0, 0, 1);
	CHECK_NEAR(north.z, -1.0, 1e-12);
	CHECK_NEAR(east.x, 1.0, 1e-12);
	CHECK_NEAR(up.y, 1.0, 1e-12);
	// Facing north in Dishonored, east is on your right (UE: X forward, Y right). In Minecraft the
	// right of a view is forward x up; facing the mapped north it must be the mapped east.
	const double rx = north.y * up.z - north.z * up.y;
	const double ry = north.z * up.x - north.x * up.z;
	const double rz = north.x * up.y - north.y * up.x;
	CHECK_NEAR(rx, east.x, 1e-12);
	CHECK_NEAR(ry, east.y, 1e-12);
	CHECK_NEAR(rz, east.z, 1e-12);
}

TEST(CoordsYawPitch)
{
	// UE yaw 0 faces north = MC yaw 180; UE yaw 16384 (90 deg) faces east = MC -90.
	CHECK_NEAR(UeYawToMc(0), -180.0, 1e-4);
	CHECK_NEAR(UeYawToMc(16384), -90.0, 1e-4);
	CHECK_NEAR(UeYawToMc(-16384), 90.0, 1e-4);
	CHECK_EQ(McYawToUe(-90.0f), 16384);
	CHECK_EQ(McYawToUe(180.0f), 0);
	// Pitch: UE up is positive, MC down is positive; UE stores down as just under 65536.
	CHECK_NEAR(UePitchToMc(4096), -22.5, 1e-4);
	CHECK_NEAR(UePitchToMc(65536 - 4096), 22.5, 1e-4);
	CHECK_EQ(McPitchToUe(22.5f), 65536 - 4096);
	CHECK_EQ(McPitchToUe(-22.5f), 4096);
	// The MC look vector of the converted angles is the UE forward vector in MC axes.
	for (std::int32_t yaw : { 0, 5000, 16384, 30000, 47000, 60000 }) {
		for (std::int32_t pitch : { 0, 3000, 62000 }) {
			const auto a = UeRotatorForwardMc({ pitch, yaw, 0 });
			const auto b = McLookVector(UeYawToMc(yaw), UePitchToMc(pitch));
			CHECK_NEAR(a.x, b.x, 1e-6);
			CHECK_NEAR(a.y, b.y, 1e-6);
			CHECK_NEAR(a.z, b.z, 1e-6);
		}
	}
}
