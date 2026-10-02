#pragma once

#include <cstdint>
#include <vector>

// Dishonored's world as Minecraft collision, from vertical line traces.
//
// The game side samples a grid of vertical columns (Minecraft space, in blocks) around the player:
// line traces straight down find every upward-facing surface in a column (floors, roofs, ground),
// traces straight up find every downward-facing one (ceilings, undersides). Sorted by height those
// give the column's solid intervals, which become
//  - triangles for the player's smooth collider (sloped tops, flat undersides, vertical sides where
//    a neighbouring column is open), and
//  - 8x8x8 occupancy masks per block for everything else (mobs, items, arrows).
// This file has no engine or Windows code: it is unit-tested on its own.
namespace discraft::mesher
{
	// A surface a vertical trace hit. up: it faces up (a down trace found it: the top of something).
	struct Hit
	{
		float y{ 0.0f };
		bool  up{ true };
		float nx{ 0.0f }, ny{ 1.0f }, nz{ 0.0f };  // surface normal (Minecraft axes)
	};

	// A solid stretch of one column.
	struct Interval
	{
		float bottom{ 0.0f };
		float top{ 0.0f };
		float nx{ 0.0f }, ny{ 1.0f }, nz{ 0.0f };  // the top surface's normal
		bool  bottomIsSurface{ false };           // a real underside (else: the traced range's end)
		bool  topIsSurface{ false };              // a real top surface
	};

	// Turns one column's hits (any order) into solid intervals within [a_yMin, a_yMax]. Below the
	// lowest top surface is solid (ground). A top seen without the underside before it (or an
	// underside without its top) is taken to be a slab a_thin thick.
	std::vector<Interval> IntervalsFromHits(std::vector<Hit> a_hits, float a_yMin, float a_yMax, float a_thin);

	// A square grid of columns. Column (i, k) covers x in [x0 + i*cell, x0 + (i+1)*cell) and z
	// likewise. The outermost ring is an apron: traced so the inner columns know their neighbours,
	// but producing no geometry itself.
	struct Grid
	{
		double                             x0{ 0.0 }, z0{ 0.0 };
		double                             cell{ 0.25 };
		int                                nx{ 0 }, nz{ 0 };
		float                              yMin{ 0.0f }, yMax{ 0.0f };
		std::vector<std::vector<Interval>> columns;  // nx * nz, index i + k * nx

		void Resize(int a_nx, int a_nz)
		{
			nx = a_nx;
			nz = a_nz;
			columns.assign(static_cast<std::size_t>(a_nx) * static_cast<std::size_t>(a_nz), {});
		}
		std::vector<Interval>&       At(int a_i, int a_k) { return columns[static_cast<std::size_t>(a_i + a_k * nx)]; }
		const std::vector<Interval>& At(int a_i, int a_k) const { return columns[static_cast<std::size_t>(a_i + a_k * nx)]; }
	};

	struct Tri
	{
		float         v[9]{};
		std::uint32_t flags{ 0 };
	};

	// bits[y] bit (z * 8 + x): sub-voxel (x, y, z) of the block, each 1/8 block.
	struct Block
	{
		std::int32_t  x{ 0 }, y{ 0 }, z{ 0 };
		std::uint64_t bits[8]{};
	};

	// Triangles for the inner columns. Steps lower than a_minStep between neighbours make no wall.
	void BuildTriangles(const Grid& a_grid, std::vector<Tri>& a_out, float a_minStep = 0.02f);
	// Occupancy of the inner columns, for blocks with y in [a_yMinBlock, a_yMaxBlock].
	void BuildVoxels(const Grid& a_grid, int a_yMinBlock, int a_yMaxBlock, std::vector<Block>& a_out);
}
