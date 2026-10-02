#include "Game.h"

#include "Bindings.h"
#include "Config.h"
#include "Log.h"
#include "Mesher.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Dishonored's world shape for Minecraft's physics.
//
// Around the player the world is cut into jobs: 8x8-block columns (Minecraft's collision regions),
// each 96 blocks tall around the player's height. A job traces a grid of vertical lines through the
// game's own collision (Actor.Trace, the same check the game's weapons and AI use), a few per frame
// within a time budget, then the mesher turns the hits into triangles and voxels for Minecraft. The
// player's own column and its neighbours use a quarter-block grid and are re-traced every few
// seconds (doors open, things move); farther ones use half a block and are refreshed less often.
namespace discraft::Collision
{
	namespace
	{
		using ue3::Obj;
		using bind::C;
		using bind::F;
		using bind::Fn;

		constexpr int   kRegion = 8;        // blocks, as DisCollision.REGION_SIZE in the Fabric mod
		constexpr int   kBandHeight = 32;   // the vertical window moves in steps of this many blocks
		constexpr int   kBelow = 32;        // traced below the band...
		constexpr int   kAbove = 64;        // ...and above its bottom
		constexpr int   kMaxHits = 16;      // per column and direction
		constexpr float kThinSlab = 0.25f;  // a surface seen from one side only is this thick
		constexpr double kMaxTraceBlocks = 24.0;  // longer lines are traced in pieces

		struct Key
		{
			int  rx, rz, band;
			bool operator<(const Key& o) const { return std::tie(rx, rz, band) < std::tie(o.rx, o.rz, o.band); }
			bool operator==(const Key& o) const { return rx == o.rx && rz == o.rz && band == o.band; }
		};

		struct Done
		{
			ULONGLONG at;
			double    cell;
		};

		struct Job
		{
			Key            key{};
			double         cell{ 0.25 };
			mesher::Grid   grid;
			int            cursor{ 0 };  // next column to trace
			bool           active{ false };
		};

		std::uint32_t        epoch = 0;
		std::map<Key, Done>  done;
		Job                  job;
		std::vector<std::vector<std::uint8_t>> outbox;  // finished messages waiting for ring space

		float  budgetMs = 4.0f;
		float  urgentBudgetMs = 20.0f;
		double fineCell = 0.25;
		double coarseCell = 0.5;
		int    radiusRegions = 3;
		bool   configured = false;

		// Actor.Trace's parameters, by offset (the same for every call).
		struct TraceLayout
		{
			int  size{ 0 };
			int  hitLocation{ -1 }, hitNormal{ -1 }, traceEnd{ -1 }, traceStart{ -1 }, traceActors{ -1 }, extent{ -1 }, extraFlags{ -1 }, returnValue{ -1 };
			std::vector<int> passed;  // every parameter but HitInfo
			std::uint32_t traceActorsMask{ 1 };
			bool ok{ false };
		};
		// Trace can fail in this game (it is then left alone); the world's shape then comes from
		// FastTrace, which only says whether a line is blocked: surfaces are found by halving.
		int  traceFailures = 0;
		bool fastMode = false;
		struct FastLayout
		{
			int              size{ 0 };
			int              traceEnd{ -1 }, traceStart{ -1 }, extent{ -1 }, bullet{ -1 }, returnValue{ -1 };
			std::vector<int> passed;
			bool             ok{ false };
		};
		FastLayout                fastLayout;
		std::vector<std::uint8_t> fastParms;

		// For the log: what the traces find.
		int                      regionsSent = 0;
		long long                hitsTotal = 0;
		long long                tracesTotal = 0;
		int                      tracesLogged = 0;
		int                      regionsLogged = 0;

		TraceLayout              traceLayout;
		std::vector<std::uint8_t> traceParms;

		void Configure()
		{
			configured = true;
			budgetMs = config::Float("World", "fTraceBudgetMs", 4.0f);
			urgentBudgetMs = std::max(budgetMs, config::Float("World", "fTraceBudgetUrgentMs", 20.0f));
			fineCell = std::clamp(static_cast<double>(config::Float("World", "fFineCell", 0.25f)), 0.125, 1.0);
			coarseCell = std::clamp(static_cast<double>(config::Float("World", "fCoarseCell", 0.5f)), fineCell, 2.0);
			radiusRegions = std::clamp(static_cast<int>(config::Float("World", "fTraceRadiusBlocks", 24.0f) / kRegion), 1, 8);

			if (Fn.trace) {
				const auto f = [](const char* n) { return ue3::FindField(Fn.trace, n); };
				const auto actors = f("bTraceActors");
				traceLayout.size = std::max(ue3::StructSize(Fn.trace), 16);
				traceLayout.hitLocation = f("HitLocation").offset;
				traceLayout.hitNormal = f("HitNormal").offset;
				traceLayout.traceEnd = f("TraceEnd").offset;
				traceLayout.traceStart = f("TraceStart").offset;
				traceLayout.traceActors = actors.offset;
				traceLayout.traceActorsMask = actors.mask ? actors.mask : 1u;
				traceLayout.extent = f("Extent").offset;
				traceLayout.extraFlags = f("ExtraTraceFlags").offset;
				for (const int o : { traceLayout.hitLocation, traceLayout.hitNormal, traceLayout.traceEnd, traceLayout.traceStart, traceLayout.traceActors,
						 traceLayout.extent, traceLayout.extraFlags }) {
					if (o >= 0) {
						traceLayout.passed.push_back(o);
					}
				}
				traceLayout.returnValue = f("ReturnValue").offset;
				traceLayout.ok = traceLayout.hitLocation >= 0 && traceLayout.hitNormal >= 0 && traceLayout.traceEnd >= 0 && traceLayout.traceStart >= 0 &&
				                 traceLayout.returnValue >= 0;
				traceParms.assign(static_cast<std::size_t>(traceLayout.size) + 16, 0);
			}
			if (Fn.fastTrace) {
				const auto f = [](const char* n) { return ue3::FindField(Fn.fastTrace, n).offset; };
				fastLayout.size = std::max(ue3::StructSize(Fn.fastTrace), 16);
				fastLayout.traceEnd = f("TraceEnd");
				fastLayout.traceStart = f("TraceStart");
				fastLayout.extent = f("BoxExtent");
				fastLayout.bullet = f("bTraceBullet");
				fastLayout.returnValue = f("ReturnValue");
				for (const int o : { fastLayout.traceEnd, fastLayout.traceStart, fastLayout.extent, fastLayout.bullet }) {
					if (o >= 0) {
						fastLayout.passed.push_back(o);
					}
				}
				fastLayout.ok = fastLayout.traceEnd >= 0 && fastLayout.traceStart >= 0 && fastLayout.returnValue >= 0;
				fastParms.assign(static_cast<std::size_t>(fastLayout.size) + 16, 0);
			}
			DC_INFO("collision: trace %s; budget %.1f ms (urgent %.1f), cells %.3f/%.3f, radius %d regions", traceLayout.ok ? "ready" : "MISSING", budgetMs,
				urgentBudgetMs, fineCell, coarseCell, radiusRegions);
		}

		struct TraceHit
		{
			bool     hit{ false };
			UeVector location{};
			UeVector normal{};
			Obj      actor{ 0 };
		};

		TraceHit Trace(Obj a_pawn, const UeVector& a_start, const UeVector& a_end)
		{
			TraceHit out;
			std::fill(traceParms.begin(), traceParms.end(), std::uint8_t(0));
			auto* p = traceParms.data();
			std::memcpy(p + traceLayout.traceEnd, &a_end, 12);
			std::memcpy(p + traceLayout.traceStart, &a_start, 12);
			// The world only (level, meshes, movers such as doors): characters aren't wanted anyway.
			static const bool withActors = config::Bool("World", "bTraceActors", false);
			if (traceLayout.traceActors >= 0) {
				const std::uint32_t v = withActors ? traceLayout.traceActorsMask : 0u;
				std::memcpy(p + traceLayout.traceActors, &v, 4);
			}
			// Every parameter but HitInfo (asking for it makes the trace look up surface materials);
			// Extent and ExtraTraceFlags are passed as zero.
			if (!ue3::CallFunction(a_pawn, Fn.trace, p, &traceLayout.passed)) {
				if (++traceFailures >= 3 && !fastMode && fastLayout.ok) {
					fastMode = true;
					DC_WARN("collision: Trace doesn't work here; finding surfaces with FastTrace instead (slower)");
				}
				return out;
			}
			traceFailures = 0;
			ue3::Addr actor = 0;
			std::memcpy(&actor, p + traceLayout.returnValue, 4);
			out.actor = actor;
			out.hit = actor != 0;
			std::memcpy(&out.location, p + traceLayout.hitLocation, 12);
			std::memcpy(&out.normal, p + traceLayout.hitNormal, 12);
			++tracesTotal;
			if (tracesLogged < 6) {
				++tracesLogged;
				DC_INFO("collision: trace (%.0f, %.0f, %.0f) -> (%.0f, %.0f, %.0f): %s at (%.0f, %.0f, %.0f) normal (%.2f, %.2f, %.2f)", a_start.x, a_start.y,
					a_start.z, a_end.x, a_end.y, a_end.z, out.hit ? ue3::FullNameOf(actor).c_str() : "nothing", out.location.x, out.location.y, out.location.z,
					out.normal.x, out.normal.y, out.normal.z);
			}
			return out;
		}

		// FastTrace: true when nothing of the world blocks the line (pawns don't count).
		bool Clear(Obj a_pawn, const UeVector& a_from, const UeVector& a_to)
		{
			std::fill(fastParms.begin(), fastParms.end(), std::uint8_t(0));
			auto* p = fastParms.data();
			std::memcpy(p + fastLayout.traceEnd, &a_to, 12);
			std::memcpy(p + fastLayout.traceStart, &a_from, 12);
			++tracesTotal;
			if (!ue3::CallFunction(a_pawn, Fn.fastTrace, p, &fastLayout.passed)) {
				return true;
			}
			std::uint32_t clear = 0;
			std::memcpy(&clear, p + fastLayout.returnValue, 4);
			return clear != 0;
		}

		// Every surface in one vertical column, both ways, with FastTrace: from the current point,
		// is anything in the way to the end? Then halve towards it until the gap is a 32nd of a
		// block. Normals aren't known: surfaces count as flat.
		void FastColumn(Obj a_pawn, double a_x, double a_z, float a_yMin, float a_yMax, std::vector<mesher::Hit>& a_out)
		{
			a_out.clear();
			const double upb = State().unitsPerBlock;
			const double eps = 1.0 / 32.0;
			const double step = 0.02;
			for (int dir = 0; dir < 2; ++dir) {
				const bool   down = dir == 0;
				double       y = down ? a_yMax : a_yMin;
				const double yEnd = down ? a_yMin : a_yMax;
				for (int n = 0; n < kMaxHits; ++n) {
					if (down ? y <= yEnd + eps : y >= yEnd - eps) {
						break;
					}
					const auto from = McToUe(a_x, y, a_z, upb);
					if (Clear(a_pawn, from, McToUe(a_x, yEnd, a_z, upb))) {
						break;
					}
					double clearTo = y, blockedBy = yEnd;  // the line from y is clear to clearTo, blocked by blockedBy
					while (std::abs(blockedBy - clearTo) > eps) {
						const double mid = 0.5 * (clearTo + blockedBy);
						if (Clear(a_pawn, from, McToUe(a_x, mid, a_z, upb))) {
							clearTo = mid;
						} else {
							blockedBy = mid;
						}
					}
					const double at = 0.5 * (clearTo + blockedBy);
					if (std::abs(at - y) < 2.0 * eps) {
						y += down ? -0.125 : 0.125;  // started inside something: step through it
						continue;
					}
					mesher::Hit  h;
					h.y = static_cast<float>(at);
					h.up = down;
					h.nx = 0.0f;
					h.ny = down ? 1.0f : -1.0f;
					h.nz = 0.0f;
					a_out.push_back(h);
					y = at + (down ? -step : step);
				}
			}
		}

		// Every surface in one vertical column, both ways. Lines longer than kMaxTraceBlocks are
		// traced a piece at a time.
		void TraceColumn(Obj a_pawn, double a_x, double a_z, float a_yMin, float a_yMax, std::vector<mesher::Hit>& a_out)
		{
			if (fastMode) {
				FastColumn(a_pawn, a_x, a_z, a_yMin, a_yMax, a_out);
				return;
			}
			a_out.clear();
			const double upb = State().unitsPerBlock;
			const double step = 0.02;  // move past a surface before looking for the next one
			for (int dir = 0; dir < 2; ++dir) {
				const bool down = dir == 0;
				double     y = down ? a_yMax : a_yMin;
				const double yEnd = down ? a_yMin : a_yMax;
				for (int n = 0; n < kMaxHits * 4; ++n) {
					if (down ? y <= yEnd : y >= yEnd) {
						break;
					}
					const double pieceEnd = down ? std::max(yEnd, y - kMaxTraceBlocks) : std::min(yEnd, y + kMaxTraceBlocks);
					const auto   hit = Trace(a_pawn, McToUe(a_x, y, a_z, upb), McToUe(a_x, pieceEnd, a_z, upb));
					if (!hit.hit) {
						y = pieceEnd;  // nothing in this piece: on to the next
						continue;
					}
					const auto   at = UeToMc(hit.location, upb);
					const auto   normal = UeDirToMc(hit.normal.x, hit.normal.y, hit.normal.z);
					const double travelled = down ? y - at.y : at.y - y;
					if (travelled < 0.001) {
						y += down ? -0.125 : 0.125;  // started inside something: step through it
						continue;
					}
					const bool skip = ue3::IsA(hit.actor, C.pawn);  // characters aren't the world
					if (!skip && ((down && normal.y > -0.05) || (!down && normal.y < 0.05))) {
						mesher::Hit h;
						h.y = static_cast<float>(at.y);
						h.up = down;
						h.nx = static_cast<float>(normal.x);
						h.ny = static_cast<float>(normal.y);
						h.nz = static_cast<float>(normal.z);
						a_out.push_back(h);
						if (static_cast<int>(a_out.size()) >= kMaxHits * 2) {
							return;
						}
					}
					y = at.y + (down ? -step : step);
				}
			}
		}

		void Queue(proto::ColType a_type, const void* a_header, std::size_t a_headerBytes, const void* a_body, std::size_t a_bodyBytes)
		{
			std::vector<std::uint8_t> msg;
			msg.reserve(4 + a_headerBytes + a_bodyBytes);
			const std::uint32_t type = a_type;
			const auto*         typeBytes = reinterpret_cast<const std::uint8_t*>(&type);
			const auto*         header = static_cast<const std::uint8_t*>(a_header);
			msg.insert(msg.end(), typeBytes, typeBytes + 4);
			msg.insert(msg.end(), header, header + a_headerBytes);
			if (a_body && a_bodyBytes) {
				const auto* body = static_cast<const std::uint8_t*>(a_body);
				msg.insert(msg.end(), body, body + a_bodyBytes);
			}
			outbox.push_back(std::move(msg));
		}

		void Flush()
		{
			auto&       link = Link::Get();
			std::size_t sent = 0;
			for (; sent < outbox.size(); ++sent) {
				const auto&   msg = outbox[sent];
				std::uint32_t type;
				std::memcpy(&type, msg.data(), 4);
				if (!link.WriteCollision(static_cast<proto::ColType>(type), msg.data() + 4, static_cast<std::uint32_t>(msg.size() - 4))) {
					break;  // ring full: next frame
				}
			}
			outbox.erase(outbox.begin(), outbox.begin() + static_cast<std::ptrdiff_t>(sent));
		}

		void Finish(Job& a_job)
		{
			const auto& g = a_job.grid;
			std::vector<mesher::Tri> tris;
			mesher::BuildTriangles(g, tris);
			const int yMin = static_cast<int>(std::floor(g.yMin));
			const int yMax = static_cast<int>(std::ceil(g.yMax)) - 1;
			std::vector<mesher::Block> blocks;
			mesher::BuildVoxels(g, yMin, yMax, blocks);

			const int x0 = a_job.key.rx * kRegion, z0 = a_job.key.rz * kRegion;
			// Triangles per 8-block cube (Minecraft keeps them per region), each with every cube it
			// comes within half a block of.
			const int cubeLo = static_cast<int>(std::floor(double(yMin) / kRegion));
			const int cubeHi = static_cast<int>(std::floor(double(yMax) / kRegion));
			std::vector<proto::ColTri> cubeTris;
			for (int cy = cubeLo; cy <= cubeHi; ++cy) {
				const double lo = cy * kRegion - 0.5, hi = (cy + 1) * kRegion + 0.5;
				cubeTris.clear();
				for (const auto& t : tris) {
					const float ty0 = std::min({ t.v[1], t.v[4], t.v[7] }), ty1 = std::max({ t.v[1], t.v[4], t.v[7] });
					if (ty1 < lo || ty0 > hi) {
						continue;
					}
					proto::ColTri c{};
					std::memcpy(c.v, t.v, sizeof(c.v));
					c.flags = t.flags;
					cubeTris.push_back(c);
				}
				proto::ColRegion header{ x0, cy * kRegion, z0, x0 + kRegion - 1, cy * kRegion + kRegion - 1, z0 + kRegion - 1, epoch,
					static_cast<std::uint32_t>(cubeTris.size()) };
				Queue(proto::kColTris, &header, sizeof(header), cubeTris.data(), cubeTris.size() * sizeof(proto::ColTri));
			}
			// The voxels of the whole column at once (Minecraft replaces everything in the box).
			std::vector<proto::ColBlock> out;
			out.reserve(blocks.size());
			for (const auto& b : blocks) {
				if (b.x < x0 || b.x >= x0 + kRegion || b.z < z0 || b.z >= z0 + kRegion) {
					continue;
				}
				proto::ColBlock c{};
				c.x = b.x;
				c.y = b.y;
				c.z = b.z;
				std::memcpy(c.bits, b.bits, sizeof(c.bits));
				out.push_back(c);
			}
			proto::ColRegion header{ x0, cubeLo * kRegion, z0, x0 + kRegion - 1, cubeHi * kRegion + kRegion - 1, z0 + kRegion - 1, epoch,
				static_cast<std::uint32_t>(out.size()) };
			Queue(proto::kColRegion, &header, sizeof(header), out.data(), out.size() * sizeof(proto::ColBlock));
			done[a_job.key] = { ::GetTickCount64(), a_job.cell };
			++regionsSent;
			if (regionsLogged < 8) {
				++regionsLogged;
				DC_INFO("collision: region (%d, %d) band %d, y %.0f..%.0f: %zu triangles, %zu voxel blocks (cell %.3f)", a_job.key.rx, a_job.key.rz,
					a_job.key.band, g.yMin, g.yMax, tris.size(), out.size(), a_job.cell);
			}
			DC_DIAG("collision: region (%d, %d) band %d: %zu triangles, %zu blocks (cell %.3f)", a_job.key.rx, a_job.key.rz, a_job.key.band, tris.size(),
				out.size(), a_job.cell);
		}

		void Start(Job& a_job, const Key& a_key, double a_cell)
		{
			a_job = Job{};
			a_job.key = a_key;
			a_job.cell = a_cell;
			a_job.active = true;
			auto&     g = a_job.grid;
			const int perSide = static_cast<int>(std::lround(kRegion / a_cell));
			g.cell = a_cell;
			g.x0 = a_key.rx * kRegion - a_cell;
			g.z0 = a_key.rz * kRegion - a_cell;
			g.yMin = static_cast<float>(a_key.band * kBandHeight - kBelow);
			g.yMax = static_cast<float>(a_key.band * kBandHeight + kAbove);
			g.Resize(perSide + 2, perSide + 2);
		}

		// The next job to run: nearest region first; near regions finely and often.
		bool Pick(const Vec3d& a_feet, Key& a_key, double& a_cell)
		{
			const int       prx = static_cast<int>(std::floor(a_feet.x / kRegion));
			const int       prz = static_cast<int>(std::floor(a_feet.z / kRegion));
			const int       band = static_cast<int>(std::floor(a_feet.y / kBandHeight));
			const ULONGLONG now = ::GetTickCount64();
			for (int ring = 0; ring <= radiusRegions; ++ring) {
				for (int dz = -ring; dz <= ring; ++dz) {
					for (int dx = -ring; dx <= ring; ++dx) {
						if (std::max(std::abs(dx), std::abs(dz)) != ring) {
							continue;
						}
						const Key       key{ prx + dx, prz + dz, band };
						const double    cell = ring <= 1 ? fineCell : coarseCell;
						const ULONGLONG maxAge = ring == 0 ? 3000 : ring == 1 ? 6000 : 30000;
						const auto      it = done.find(key);
						if (it == done.end() || it->second.cell > cell + 1e-9 || now - it->second.at > maxAge) {
							a_key = key;
							a_cell = cell;
							return true;
						}
					}
				}
			}
			return false;
		}
	}

	void Reset(std::uint32_t a_epoch)
	{
		epoch = a_epoch;
		done.clear();
		job.active = false;
		outbox.clear();
		const std::uint32_t payload = a_epoch;
		Queue(proto::kColClear, &payload, sizeof(payload), nullptr, 0);
		Flush();
		tracesLogged = 0;  // show the first traces of every new world
		regionsLogged = 0;
		DC_INFO("collision: reset (epoch %u)", a_epoch);
	}

	void PerFrame(Obj a_pawn, const Vec3d& a_feet, bool a_urgent)
	{
		if (!configured) {
			Configure();
		}
		Flush();
		if (!traceLayout.ok && fastLayout.ok && !fastMode) {
			fastMode = true;
			DC_WARN("collision: no Trace; finding surfaces with FastTrace (slower)");
		}
		if ((!traceLayout.ok && !fastMode) || ue3::ProcessEventIndex() <= 0 || !a_pawn || outbox.size() > 64) {
			return;
		}
		if (Link::Get().CollisionSpace() < (4ull << 20)) {
			return;  // Minecraft is behind; don't pile up more
		}
		LARGE_INTEGER freq, start, now;
		::QueryPerformanceFrequency(&freq);
		::QueryPerformanceCounter(&start);
		const double budget = (a_urgent ? urgentBudgetMs : budgetMs) * double(freq.QuadPart) / 1000.0;

		std::vector<mesher::Hit> hits;
		hits.reserve(kMaxHits * 2);
		while (true) {
			if (!job.active) {
				Key    key{};
				double cell = fineCell;
				if (!Pick(a_feet, key, cell)) {
					return;
				}
				Start(job, key, cell);
			}
			auto&     g = job.grid;
			const int total = g.nx * g.nz;
			while (job.cursor < total) {
				const int    i = job.cursor % g.nx, k = job.cursor / g.nx;
				const double x = g.x0 + (i + 0.5) * g.cell, z = g.z0 + (k + 0.5) * g.cell;
				TraceColumn(a_pawn, x, z, g.yMin, g.yMax, hits);
				hitsTotal += static_cast<long long>(hits.size());
				g.At(i, k) = mesher::IntervalsFromHits(hits, g.yMin, g.yMax, kThinSlab);
				++job.cursor;
				::QueryPerformanceCounter(&now);
				if (double(now.QuadPart - start.QuadPart) > budget) {
					return;
				}
			}
			Finish(job);
			job.active = false;
			Flush();
			::QueryPerformanceCounter(&now);
			if (double(now.QuadPart - start.QuadPart) > budget) {
				return;
			}
		}
	}

	std::string Summary()
	{
		char buf[160];
		std::snprintf(buf, sizeof(buf), "%d regions sent, %lld traces, %lld surfaces, outbox %zu%s%s", regionsSent, tracesTotal, hitsTotal, outbox.size(),
			traceLayout.ok ? "" : " (Trace missing)", fastMode ? " (FastTrace mode)" : "");
		return buf;
	}
}
