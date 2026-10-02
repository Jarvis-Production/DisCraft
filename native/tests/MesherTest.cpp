#include "Check.h"
#include "Mesher.h"

#include <algorithm>

using namespace discraft::mesher;

namespace
{
	Hit Up(float y) { return Hit{ y, true, 0.0f, 1.0f, 0.0f }; }
	Hit Down(float y) { return Hit{ y, false, 0.0f, -1.0f, 0.0f }; }

	// A grid of n x n inner columns (plus the apron) whose columns are given by f(i, k).
	template <class F>
	Grid MakeGrid(int a_inner, double a_cell, F a_f)
	{
		Grid g;
		g.x0 = 100.0 - a_cell;  // inner area starts at x = 100
		g.z0 = -50.0 - a_cell;
		g.cell = a_cell;
		g.yMin = 0.0f;
		g.yMax = 40.0f;
		g.Resize(a_inner + 2, a_inner + 2);
		for (int k = 0; k < g.nz; ++k) {
			for (int i = 0; i < g.nx; ++i) {
				g.At(i, k) = a_f(i, k);
			}
		}
		return g;
	}
}

TEST(IntervalsGroundAndRoom)
{
	// Ground at 10, a room's ceiling underside at 13, the roof at 14.
	const auto iv = IntervalsFromHits({ Up(14.0f), Down(13.0f), Up(10.0f) }, 0.0f, 40.0f, 0.25f);
	CHECK_EQ(iv.size(), std::size_t(2));
	CHECK_NEAR(iv[0].bottom, 0.0, 0.0);
	CHECK_NEAR(iv[0].top, 10.0, 0.0);
	CHECK(!iv[0].bottomIsSurface);
	CHECK(iv[0].topIsSurface);
	CHECK_NEAR(iv[1].bottom, 13.0, 0.0);
	CHECK_NEAR(iv[1].top, 14.0, 0.0);
	CHECK(iv[1].bottomIsSurface && iv[1].topIsSurface);
}

TEST(IntervalsThinAndOpen)
{
	// Nothing at all: open air.
	CHECK(IntervalsFromHits({}, 0.0f, 40.0f, 0.25f).empty());
	// A one-sided floor seen only from above, over open air.
	auto iv = IntervalsFromHits({ Down(2.0f), Up(5.0f), Up(9.0f) }, 0.0f, 40.0f, 0.25f);
	// Starts in air (the lowest surface faces down): 2..5 solid, then 9 is a thin slab.
	CHECK_EQ(iv.size(), std::size_t(2));
	CHECK_NEAR(iv[0].bottom, 2.0, 1e-6);
	CHECK_NEAR(iv[0].top, 5.0, 1e-6);
	CHECK_NEAR(iv[1].bottom, 8.75, 1e-6);
	CHECK_NEAR(iv[1].top, 9.0, 1e-6);
	// A ceiling with nothing above it up to the traced range: solid to the top.
	iv = IntervalsFromHits({ Up(3.0f), Down(20.0f) }, 0.0f, 40.0f, 0.25f);
	CHECK_EQ(iv.size(), std::size_t(2));
	CHECK_NEAR(iv[1].bottom, 20.0, 0.0);
	CHECK_NEAR(iv[1].top, 40.0, 0.0);
	CHECK(!iv[1].topIsSurface);
	// Hits outside the range are ignored.
	iv = IntervalsFromHits({ Up(-5.0f), Up(50.0f) }, 0.0f, 40.0f, 0.25f);
	CHECK(iv.empty());
}

TEST(FlatFloorHasNoWalls)
{
	const auto g = MakeGrid(8, 0.25, [](int, int) { return std::vector<Interval>{ { 0.0f, 10.0f, 0.0f, 1.0f, 0.0f, false, true } }; });
	std::vector<Tri> tris;
	BuildTriangles(g, tris);
	CHECK_EQ(tris.size(), std::size_t(8 * 8 * 2));  // only the tops
	for (const auto& t : tris) {
		CHECK_NEAR(t.v[1], 10.0, 1e-6);
		CHECK_NEAR(t.v[4], 10.0, 1e-6);
		CHECK_NEAR(t.v[7], 10.0, 1e-6);
	}
	std::vector<Block> blocks;
	BuildVoxels(g, 8, 10, blocks);
	// 8x8 columns of a quarter block = 2x2 blocks; blocks y=8 and 9 full, y=10 empty.
	CHECK_EQ(blocks.size(), std::size_t(2 * 2 * 2));
	for (const auto& b : blocks) {
		CHECK(b.y == 8 || b.y == 9);
		CHECK(b.x == 100 || b.x == 101);
		CHECK(b.z == -50 || b.z == -49);
		for (int y = 0; y < 8; ++y) {
			CHECK_EQ(b.bits[y], ~std::uint64_t(0));
		}
	}
}

TEST(StepMakesAWall)
{
	// The left half is a block higher than the right half (x < 101 at 11, else 10).
	const auto g = MakeGrid(8, 0.25, [](int i, int) {
		const float top = i <= 4 ? 11.0f : 10.0f;
		return std::vector<Interval>{ { 0.0f, top, 0.0f, 1.0f, 0.0f, false, true } };
	});
	std::vector<Tri> tris;
	BuildTriangles(g, tris);
	int walls = 0;
	for (const auto& t : tris) {
		const bool vertical = t.v[0] == t.v[3] && t.v[3] == t.v[6];
		if (vertical) {
			++walls;
			CHECK_NEAR(t.v[0], 101.0, 1e-6);  // between column 4 and 5: x = 100 + 4 * 0.25
			const float lo = std::min({ t.v[1], t.v[4], t.v[7] });
			const float hi = std::max({ t.v[1], t.v[4], t.v[7] });
			CHECK_NEAR(lo, 10.0, 1e-6);
			CHECK_NEAR(hi, 11.0, 1e-6);
		}
	}
	CHECK_EQ(walls, 8 * 2);  // one quad per row along the step
}

TEST(SlopedTopFollowsNormal)
{
	// A 45-degree slope rising towards +x: normal (-0.707, 0.707, 0).
	const float s = 0.70710678f;
	const auto  g = MakeGrid(4, 0.5, [&](int i, int) {
        const float centerX = static_cast<float>(100.0 - 0.5 + (i + 0.5) * 0.5);
        return std::vector<Interval>{ { 0.0f, 10.0f + (centerX - 100.0f), -s, s, 0.0f, false, true } };
	});
	std::vector<Tri> tris;
	BuildTriangles(g, tris);
	// Every top vertex lies on y = 10 + (x - 100), so neighbouring tops meet without steps.
	for (const auto& t : tris) {
		const bool vertical = t.v[0] == t.v[3] && t.v[3] == t.v[6];
		if (vertical) {
			continue;
		}
		for (int v = 0; v < 3; ++v) {
			CHECK_NEAR(t.v[v * 3 + 1], 10.0 + (t.v[v * 3] - 100.0), 1e-4);
		}
	}
}

TEST(ThinSlabStillBlocksVoxels)
{
	const auto g = MakeGrid(2, 0.5, [](int, int) { return std::vector<Interval>{ { 5.03f, 5.07f, 0.0f, 1.0f, 0.0f, true, true } }; });
	std::vector<Block> blocks;
	BuildVoxels(g, 0, 20, blocks);
	CHECK_EQ(blocks.size(), std::size_t(1));
	CHECK_EQ(blocks[0].y, 5);
	CHECK_EQ(blocks[0].bits[0], ~std::uint64_t(0));
	for (int y = 1; y < 8; ++y) {
		CHECK_EQ(blocks[0].bits[y], std::uint64_t(0));
	}
}
