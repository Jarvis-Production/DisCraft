#include "Mesher.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace discraft::mesher
{
	std::vector<Interval> IntervalsFromHits(std::vector<Hit> a_hits, float a_yMin, float a_yMax, float a_thin)
	{
		std::vector<Interval> out;
		a_hits.erase(std::remove_if(a_hits.begin(), a_hits.end(), [&](const Hit& h) { return h.y < a_yMin || h.y > a_yMax || !std::isfinite(h.y); }),
			a_hits.end());
		std::sort(a_hits.begin(), a_hits.end(), [](const Hit& a, const Hit& b) { return a.y < b.y; });
		if (a_hits.empty()) {
			return out;
		}
		bool  solid = a_hits.front().up;  // the lowest surface faces up: we start underground
		float start = a_yMin;
		bool  startIsSurface = false;
		float lastTop = a_yMin;
		for (const auto& h : a_hits) {
			if (h.up) {
				Interval iv;
				iv.top = h.y;
				iv.topIsSurface = true;
				iv.nx = h.nx;
				iv.ny = h.ny;
				iv.nz = h.nz;
				if (solid) {
					iv.bottom = start;
					iv.bottomIsSurface = startIsSurface;
				} else {
					// A top without the underside before it: a thin slab (or one-sided geometry).
					iv.bottom = std::max(h.y - a_thin, lastTop);
					iv.bottomIsSurface = true;
				}
				if (iv.top > iv.bottom) {
					out.push_back(iv);
				}
				lastTop = h.y;
				solid = false;
			} else {
				if (solid) {
					// Two undersides in a row: the top of the first stretch was never seen.
					Interval iv;
					iv.bottom = start;
					iv.bottomIsSurface = startIsSurface;
					iv.top = std::min(start + a_thin, h.y);
					iv.topIsSurface = true;
					if (iv.top > iv.bottom) {
						out.push_back(iv);
					}
					lastTop = iv.top;
				}
				start = h.y;
				startIsSurface = true;
				solid = true;
			}
		}
		if (solid) {
			Interval iv;
			iv.bottom = start;
			iv.bottomIsSurface = startIsSurface;
			iv.top = a_yMax;
			iv.topIsSurface = false;
			if (iv.top > iv.bottom) {
				out.push_back(iv);
			}
		}
		// Merge stretches that touch.
		std::vector<Interval> merged;
		for (const auto& iv : out) {
			if (!merged.empty() && iv.bottom <= merged.back().top + 1e-3f) {
				auto& last = merged.back();
				if (iv.top > last.top) {
					last.top = iv.top;
					last.topIsSurface = iv.topIsSurface;
					last.nx = iv.nx;
					last.ny = iv.ny;
					last.nz = iv.nz;
				}
				continue;
			}
			merged.push_back(iv);
		}
		return merged;
	}

	namespace
	{
		void Quad(std::vector<Tri>& a_out, const float a_a[3], const float a_b[3], const float a_c[3], const float a_d[3])
		{
			Tri t1;
			Tri t2;
			for (int k = 0; k < 3; ++k) {
				t1.v[k] = a_a[k];
				t1.v[3 + k] = a_b[k];
				t1.v[6 + k] = a_c[k];
				t2.v[k] = a_a[k];
				t2.v[3 + k] = a_c[k];
				t2.v[6 + k] = a_d[k];
			}
			a_out.push_back(t1);
			a_out.push_back(t2);
		}

		// [a_lo, a_hi] minus the union of a_cover, as segments at least a_min long.
		void Uncovered(float a_lo, float a_hi, const std::vector<Interval>& a_cover, float a_min, std::vector<std::pair<float, float>>& a_out)
		{
			a_out.clear();
			float at = a_lo;
			for (const auto& c : a_cover) {  // sorted, disjoint
				if (c.top <= at) {
					continue;
				}
				if (c.bottom >= a_hi) {
					break;
				}
				if (c.bottom > at && c.bottom - at >= a_min) {
					a_out.emplace_back(at, std::min(c.bottom, a_hi));
				}
				at = std::max(at, c.top);
				if (at >= a_hi) {
					return;
				}
			}
			if (a_hi - at >= a_min) {
				a_out.emplace_back(at, a_hi);
			}
		}
	}

	void BuildTriangles(const Grid& a_grid, std::vector<Tri>& a_out, float a_minStep)
	{
		const float                          c = static_cast<float>(a_grid.cell);
		std::vector<std::pair<float, float>> open;
		for (int k = 1; k + 1 < a_grid.nz; ++k) {
			for (int i = 1; i + 1 < a_grid.nx; ++i) {
				const auto& column = a_grid.At(i, k);
				if (column.empty()) {
					continue;
				}
				const float x0 = static_cast<float>(a_grid.x0 + i * a_grid.cell);
				const float z0 = static_cast<float>(a_grid.z0 + k * a_grid.cell);
				const float x1 = x0 + c, z1 = z0 + c;
				const float cx = x0 + c * 0.5f, cz = z0 + c * 0.5f;
				for (const auto& iv : column) {
					if (iv.topIsSurface) {
						// The top follows its surface's slope across the cell (flat if it's steep).
						const auto h = [&](float x, float z) {
							if (iv.ny < 0.25f) {
								return iv.top;
							}
							const float y = iv.top - (iv.nx * (x - cx) + iv.nz * (z - cz)) / iv.ny;
							return std::clamp(y, iv.top - 2.0f * c, iv.top + 2.0f * c);
						};
						const float a[3]{ x0, h(x0, z0), z0 }, b[3]{ x1, h(x1, z0), z0 }, d[3]{ x0, h(x0, z1), z1 }, e[3]{ x1, h(x1, z1), z1 };
						Quad(a_out, a, b, e, d);
					}
					if (iv.bottomIsSurface) {
						const float y = iv.bottom;
						const float a[3]{ x0, y, z0 }, b[3]{ x1, y, z0 }, d[3]{ x0, y, z1 }, e[3]{ x1, y, z1 };
						Quad(a_out, a, d, e, b);
					}
					// Sides wherever the neighbouring column is open at these heights.
					struct Side
					{
						int   di, dk;
						float ax, az, bx, bz;
					};
					const Side sides[4]{ { -1, 0, x0, z0, x0, z1 }, { 1, 0, x1, z1, x1, z0 }, { 0, -1, x1, z0, x0, z0 }, { 0, 1, x0, z1, x1, z1 } };
					for (const auto& s : sides) {
						Uncovered(iv.bottom, iv.top, a_grid.At(i + s.di, k + s.dk), a_minStep, open);
						for (const auto& [lo, hi] : open) {
							const float a[3]{ s.ax, lo, s.az }, b[3]{ s.bx, lo, s.bz }, d[3]{ s.ax, hi, s.az }, e[3]{ s.bx, hi, s.bz };
							Quad(a_out, a, b, e, d);
						}
					}
				}
			}
		}
	}

	void BuildVoxels(const Grid& a_grid, int a_yMinBlock, int a_yMaxBlock, std::vector<Block>& a_out)
	{
		struct Key
		{
			std::int32_t x, y, z;
			bool         operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
		};
		struct KeyHash
		{
			std::size_t operator()(const Key& k) const
			{
				return static_cast<std::size_t>(k.x) * 73856093u ^ static_cast<std::size_t>(k.y) * 19349663u ^ static_cast<std::size_t>(k.z) * 83492791u;
			}
		};
		std::unordered_map<Key, std::size_t, KeyHash> index;
		const auto                                    block = [&](std::int32_t x, std::int32_t y, std::int32_t z) -> Block& {
            const Key key{ x, y, z };
            auto      it = index.find(key);
            if (it == index.end()) {
                it = index.emplace(key, a_out.size()).first;
                Block b;
                b.x = x;
                b.y = y;
                b.z = z;
                a_out.push_back(b);
            }
            return a_out[it->second];
		};
		const std::int64_t yLo = static_cast<std::int64_t>(a_yMinBlock) * 8;
		const std::int64_t yHi = (static_cast<std::int64_t>(a_yMaxBlock) + 1) * 8 - 1;
		for (int k = 1; k + 1 < a_grid.nz; ++k) {
			for (int i = 1; i + 1 < a_grid.nx; ++i) {
				const auto& column = a_grid.At(i, k);
				if (column.empty()) {
					continue;
				}
				// Sub-voxel columns this grid column covers (by their centres).
				const double       x0 = a_grid.x0 + i * a_grid.cell, z0 = a_grid.z0 + k * a_grid.cell;
				const std::int64_t sx0 = static_cast<std::int64_t>(std::ceil(x0 * 8.0 - 0.5));
				const std::int64_t sx1 = static_cast<std::int64_t>(std::ceil((x0 + a_grid.cell) * 8.0 - 0.5)) - 1;
				const std::int64_t sz0 = static_cast<std::int64_t>(std::ceil(z0 * 8.0 - 0.5));
				const std::int64_t sz1 = static_cast<std::int64_t>(std::ceil((z0 + a_grid.cell) * 8.0 - 0.5)) - 1;
				for (const auto& iv : column) {
					std::int64_t sy0 = static_cast<std::int64_t>(std::ceil(iv.bottom * 8.0 - 0.5));
					std::int64_t sy1 = static_cast<std::int64_t>(std::floor(iv.top * 8.0 - 0.5));
					if (sy1 < sy0) {
						// Thinner than a sub-voxel: keep it as the one its middle is in.
						sy0 = sy1 = static_cast<std::int64_t>(std::floor((iv.bottom + iv.top) * 4.0));
					}
					sy0 = std::max(sy0, yLo);
					sy1 = std::min(sy1, yHi);
					for (std::int64_t sz = sz0; sz <= sz1; ++sz) {
						for (std::int64_t sx = sx0; sx <= sx1; ++sx) {
							for (std::int64_t sy = sy0; sy <= sy1; ++sy) {
								const auto bx = static_cast<std::int32_t>(sx >> 3), by = static_cast<std::int32_t>(sy >> 3), bz = static_cast<std::int32_t>(sz >> 3);
								auto&      b = block(bx, by, bz);
								b.bits[sy & 7] |= std::uint64_t(1) << ((sz & 7) * 8 + (sx & 7));
							}
						}
					}
				}
			}
		}
	}
}
