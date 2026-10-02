#include "Game.h"

#include "Bindings.h"
#include "Config.h"
#include "Log.h"
#include "Mem.h"
#include "Seh.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <deque>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef _MSC_VER
#	include <intrin.h>
#	pragma intrinsic(_ReturnAddress)
#endif

namespace discraft
{
	Runtime& State()
	{
		static Runtime runtime;
		return runtime;
	}

	namespace
	{
		using ue3::Obj;
		using bind::C;
		using bind::F;
		using bind::Fn;

		constexpr float  kSettleSeconds = 1.5f;
		constexpr double kGameMovedBlocks = 3.0;  // the game moved its player itself (load, script, ...)
		constexpr float  kMcPlayerHeight = 1.8f;

		// ---- the per-frame hook -------------------------------------------------------------------
		std::vector<Obj>               tickFunctions;
		std::unordered_map<Obj, void*> tickOriginals;
		void*                          sharedOriginal = nullptr;
		int                            tickDepth = 0;  // game thread only
		bool                           processEventSearched = false;
		bool                           warnedNoOriginal = false;
		std::string                    tickName;  // the candidate that matched, once one has
		std::atomic<bool>              tickMissing{ false };
		// The per-frame update faulted: DisCraft stops (the game goes on). stage says where it was.
		std::atomic<bool>              tickBroken{ false };
		const char* volatile           stage = "start";
		std::string                    brokenAt;

		// ---- game-thread state --------------------------------------------------------------------
		proto::McState mc{};
		bool           mcWasAlive = false;
		std::uint32_t  lastMcPid = 0;
		// Starts somewhere new each run, so a Minecraft still acknowledging the last run's teleport
		// can't be taken for having arrived at this run's.
		std::uint32_t epoch = 0;
		std::uint32_t teleportSeq = 0;
		std::uint32_t worldId = 0;
		Obj           lastWorld = 0;
		bool          teleportPending = true;
		float         holdMismatch = 0.0f;
		float         settleTimer = kSettleSeconds;
		UeVector      lastSet{};
		bool          haveLastSet = false;
		bool          unitsFixed = false;
		bool          wasPuppet = false;
		bool          wasMenu = true;
		std::int64_t  lastTickQpc = 0;
		float         lookTravel = 0.0f;
		ULONGLONG     statusLoggedAt = 0;

		// What we changed on the pawn while Minecraft drives it (put back afterwards).
		Obj          takenPawn = 0;
		std::uint8_t savedPhysics = 1;
		bool         savedCollideWorld = true;
		float        savedBaseEye = 0.0f;
		float        savedEye = 0.0f;

		// Minecraft's 20 Hz ticks, interpolated on our own clock (SkyCraft's scheme: render slightly in
		// the past so the next tick has always arrived; never extrapolate).
		struct McTick
		{
			proto::McState s;
			std::int64_t   at;
			int            slots;
		};
		std::deque<McTick>     tickHistory;
		std::array<double, 64> tickDue{};
		std::size_t            tickDueNext = 0;
		bool                   tickDueInit = false;
		double                 renderDelayMs = 10.0;
		std::int64_t           lastFrameQpc = 0;
		int                    stampOutliers = 0;

		std::mutex   statusLock;
		std::wstring status;
		bool         discoveryFailed = false;

		std::int64_t Qpc()
		{
			LARGE_INTEGER v;
			::QueryPerformanceCounter(&v);
			return v.QuadPart;
		}

		std::int64_t QpcFrequency()
		{
			static const std::int64_t f = [] {
				LARGE_INTEGER v;
				::QueryPerformanceFrequency(&v);
				return v.QuadPart;
			}();
			return f;
		}

		std::uint32_t Fnv1a(const std::string& a_text)
		{
			std::uint32_t h = 2166136261u;
			for (const unsigned char c : a_text) {
				h = (h ^ c) * 16777619u;
			}
			return h ? h : 1;
		}

		void SetStatus(std::wstring a_text)
		{
			std::lock_guard guard(statusLock);
			status = std::move(a_text);
		}

		// ---- Minecraft's motion -------------------------------------------------------------------
		struct Motion
		{
			double feetX, feetY, feetZ;
			double eyeHeight;
		};

		Motion Interpolate(const proto::McState& a_mc)
		{
			Motion out{ a_mc.x, a_mc.y, a_mc.z, a_mc.eyeHeight };
			if (a_mc.tickQpc == 0 || a_mc.tickMs <= 0.0f) {
				return out;
			}
			const double       qpcPerMs = double(QpcFrequency()) / 1000.0;
			const std::int64_t period = std::max<std::int64_t>(1, std::llround(double(a_mc.tickMs) * qpcPerMs));
			const std::int64_t now = Qpc();
			if (tickHistory.empty() || tickHistory.back().s.tickQpc != a_mc.tickQpc) {
				if (!tickHistory.empty() && a_mc.tickQpc < tickHistory.back().s.tickQpc) {
					tickHistory.clear();  // Minecraft restarted
				}
				McTick tick{ a_mc, a_mc.tickQpc, 1 };
				if (!tickHistory.empty()) {
					auto&              last = tickHistory.back();
					const std::int64_t n = std::llround(double(a_mc.tickQpc - last.at) / double(period));
					const std::int64_t err = a_mc.tickQpc - (last.at + n * period);
					if (n == 0 && last.slots >= 2) {
						// Two ticks in one Minecraft frame and we saw both: the first one carries the
						// second's stamp. It belongs a tick earlier.
						last.at -= period;
						last.slots -= 1;
						tick.at = last.at + period;
					} else if (n >= 1 && n <= 10 && std::llabs(err) < period * 3 / 10) {
						tick.at = last.at + n * period + err / 16;  // the rhythm is exact; the stamps are noisy
						tick.slots = static_cast<int>(n);
						stampOutliers = 0;
					} else if (n <= 10 && ++stampOutliers < 3) {
						tick.slots = static_cast<int>(std::max<std::int64_t>(n, 1));
						tick.at = last.at + tick.slots * period;  // one odd stamp (a hitch): keep the rhythm
					} else {
						stampOutliers = 0;  // lost the rhythm: start from this stamp
					}
				}
				if (lastFrameQpc != 0) {
					if (!tickDueInit) {
						tickDue.fill(renderDelayMs - 1.0);
						tickDueInit = true;
					}
					const double dueMs = double(lastFrameQpc - tick.at) / qpcPerMs;
					if (dueMs < 30.0) {
						tickDue[tickDueNext++ % tickDue.size()] = dueMs;
					}
				}
				tickHistory.push_back(tick);
				if (tickHistory.size() > 8) {
					tickHistory.pop_front();
				}
			}
			const double frameMs = lastFrameQpc != 0 ? double(now - lastFrameQpc) / qpcPerMs : 0.0;
			lastFrameQpc = now;
			if (tickDueInit) {
				const double target = std::clamp(*std::max_element(tickDue.begin(), tickDue.end()) + 1.0, 4.0, 30.0);
				const double dt = std::min(frameMs, 100.0) / 1000.0;
				renderDelayMs = target > renderDelayMs ? std::min(target, renderDelayMs + 20.0 * dt) : std::max(target, renderDelayMs - 2.0 * dt);
			}
			const std::int64_t renderQpc = now - std::llround(renderDelayMs * qpcPerMs);
			std::size_t        i = 0;
			for (std::size_t k = tickHistory.size(); k-- > 0;) {
				if (tickHistory[k].at <= renderQpc) {
					i = k;
					break;
				}
			}
			const McTick& tick = tickHistory[i];
			const McTick* next = i + 1 < tickHistory.size() ? &tickHistory[i + 1] : nullptr;
			const double  ticks = double(renderQpc - tick.at) / double(period);
			const double  t = std::clamp(ticks, 0.0, 1.0);
			const auto&   s = tick.s;
			out.feetX = s.prevX + (s.curX - s.prevX) * t;
			out.feetY = s.prevY + (s.curY - s.prevY) * t;
			out.feetZ = s.prevZ + (s.curZ - s.prevZ) * t;
			out.eyeHeight = s.tickEyeO + (s.tickEye - s.tickEyeO) * t;
			if (ticks > 1.0 && next) {
				// A tick we never saw: carry on from this tick's end to the next one's start.
				const auto&  n = next->s;
				const double gap = double(next->at - (tick.at + period));
				const double u = gap > 0.0 ? std::clamp(double(renderQpc - (tick.at + period)) / gap, 0.0, 1.0) : 1.0;
				out.feetX = s.curX + (n.prevX - s.curX) * u;
				out.feetY = s.curY + (n.prevY - s.curY) * u;
				out.feetZ = s.curZ + (n.prevZ - s.curZ) * u;
				out.eyeHeight = s.tickEye + (n.tickEyeO - s.tickEye) * u;
			}
			if (out.eyeHeight <= 0.1) {
				out.eyeHeight = a_mc.eyeHeight > 0.1f ? a_mc.eyeHeight : 1.62;
			}
			return out;
		}

		// ---- the pawn -----------------------------------------------------------------------------
		void SetIgnoreInput(Obj a_pc, bool a_ignore)
		{
			for (const auto* f : { &F.ignoreMoveInput, &F.ignoreLookInput }) {
				if (!*f) {
					continue;
				}
				if (f->kind == "BoolProperty") {
					ue3::SetBool(a_pc, *f, a_ignore);
				} else {
					ue3::Set<std::uint8_t>(a_pc, *f, a_ignore ? 1 : 0);
				}
			}
		}

		void SetPhysics(Obj a_pawn, int a_physics)
		{
			if (Fn.setPhysics) {
				ue3::Params p(Fn.setPhysics);
				p.SetNumber("newPhysics", a_physics);
				p.Invoke(a_pawn);
			} else if (F.physics) {
				ue3::Set<std::uint8_t>(a_pawn, F.physics, static_cast<std::uint8_t>(a_physics));
			}
		}

		void TakeOver(Obj a_pawn)
		{
			takenPawn = a_pawn;
			savedPhysics = ue3::Get<std::uint8_t>(a_pawn, F.physics);
			savedCollideWorld = ue3::GetBool(a_pawn, F.collideWorld);
			savedBaseEye = ue3::Get<float>(a_pawn, F.baseEyeHeight);
			savedEye = ue3::Get<float>(a_pawn, F.eyeHeight);
			// Minecraft moves the pawn: no gravity or walking physics of the game's own, and no world
			// collision check that could refuse a spot Minecraft's (slightly different) collision allowed.
			SetPhysics(a_pawn, Fn.physFlying);
			ue3::SetBool(a_pawn, F.collideWorld, false);
			DC_INFO("puppet: took over the player's pawn (%s); physics %u -> %d", ue3::NameOf(ue3::ClassOf(a_pawn)).c_str(), savedPhysics, Fn.physFlying);
		}

		// ---- the game's own player out of sight while Minecraft's is shown -------------------------
		// The pawn (its body, first-person hands), everything it or the controller owns or carries
		// (weapons, attachments) and the game's HUD. Put back as they were on release.
		struct HiddenActor
		{
			Obj  actor;
			bool wasHidden;
		};
		std::vector<HiddenActor> hiddenActors;
		Obj                      hiddenHud = 0;
		bool                     hudWasShown = true;
		ULONGLONG                hideScanAt = 0;

		void SetActorHidden(Obj a_actor, bool a_hidden)
		{
			if (Fn.setHidden) {
				ue3::Params p(Fn.setHidden);
				p.SetBool("bNewHidden", a_hidden);
				if (p.Invoke(a_actor)) {
					return;
				}
			}
			ue3::SetBool(a_actor, F.hidden, a_hidden);
		}

		bool BelongsTo(Obj a_actor, Obj a_pawn, Obj a_pc)
		{
			for (const auto* link : { &F.owner, &F.base }) {
				if (!*link) {
					continue;
				}
				Obj o = a_actor;
				for (int depth = 0; depth < 4 && o; ++depth) {
					o = ue3::GetObj(o, *link);
					if (o && (o == a_pawn || o == a_pc)) {
						return true;
					}
				}
			}
			return false;
		}

		void HideGamePlayer(Obj a_pc, Obj a_pawn)
		{
			static const bool hidePlayer = config::Bool("Render", "bHideGamePlayer", true);
			static const bool hideHud = config::Bool("Render", "bHideGameHud", true);
			if (hideHud && F.myHud && F.showHud) {
				const Obj hud = ue3::GetObj(a_pc, F.myHud);
				if (hud && ue3::IsObject(hud)) {
					if (hud != hiddenHud) {
						hiddenHud = hud;
						hudWasShown = ue3::GetBool(hud, F.showHud);
					}
					ue3::SetBool(hud, F.showHud, false);
				}
			}
			const ULONGLONG now = ::GetTickCount64();
			if (!hidePlayer || !F.hidden || now - hideScanAt < 2000) {
				return;
			}
			hideScanAt = now;
			auto hide = [&](Obj a_actor) {
				for (const auto& h : hiddenActors) {
					if (h.actor == a_actor) {
						return;
					}
				}
				const bool was = ue3::GetBool(a_actor, F.hidden);
				hiddenActors.push_back({ a_actor, was });
				if (!was) {
					SetActorHidden(a_actor, true);
					DC_DIAG("hid %s", ue3::FullNameOf(a_actor).c_str());
				}
			};
			const std::size_t before = hiddenActors.size();
			hide(a_pawn);
			const int count = ue3::ObjectCount();
			for (int i = 0; i < count; ++i) {
				const Obj o = ue3::ObjectAt(i);
				if (!o || o == a_pc || o == a_pawn || !ue3::IsA(o, C.actor) || ue3::IsA(o, C.controller) || (C.camera && ue3::IsA(o, C.camera)) ||
					ue3::IsDefaultObject(o) || bind::IsDying(o)) {
					continue;
				}
				if (BelongsTo(o, a_pawn, a_pc)) {
					hide(o);
				}
			}
			if (hiddenActors.size() != before) {
				DC_INFO("hid the game's player: %zu actor(s) (pawn, hands, weapons, attachments)", hiddenActors.size());
			}
		}

		void ShowGamePlayer()
		{
			for (const auto& h : hiddenActors) {
				if (ue3::IsObject(h.actor) && !bind::IsDying(h.actor) && !h.wasHidden) {
					SetActorHidden(h.actor, false);
				}
			}
			hiddenActors.clear();
			if (hiddenHud && ue3::IsObject(hiddenHud)) {
				ue3::SetBool(hiddenHud, F.showHud, hudWasShown);
			}
			hiddenHud = 0;
			hideScanAt = 0;
		}

		void Release(Obj a_pc)
		{
			if (!takenPawn) {
				return;
			}
			const Obj pawn = takenPawn;
			takenPawn = 0;
			SetIgnoreInput(a_pc, false);
			if (ue3::IsObject(pawn) && !bind::IsDying(pawn)) {
				ue3::SetBool(pawn, F.collideWorld, savedCollideWorld);
				if (F.baseEyeHeight) {
					ue3::Set<float>(pawn, F.baseEyeHeight, savedBaseEye);
				}
				if (F.eyeHeight) {
					ue3::Set<float>(pawn, F.eyeHeight, savedEye);
				}
				SetPhysics(pawn, savedPhysics == Fn.physFlying ? Fn.physWalking : savedPhysics);
				Actors::Release(pawn);
			}
			ShowGamePlayer();
			DC_INFO("puppet: handed the player back to the game");
		}

		void Puppet(Obj a_pc, Obj a_pawn, const Motion& a_m, float a_halfHeight)
		{
			auto&        st = State();
			const double upb = st.unitsPerBlock;
			if (takenPawn != a_pawn) {
				TakeOver(a_pawn);
			}
			UeVector center = McToUe(a_m.feetX, a_m.feetY, a_m.feetZ, upb);
			center.z += a_halfHeight;
			bool moved = false;
			if (Fn.setLocation) {
				ue3::Params p(Fn.setLocation);
				p.Set("NewLocation", center);
				p.Invoke(a_pawn);
				moved = p.GetBool("ReturnValue");
			}
			if (!moved) {
				ue3::Set(a_pawn, F.location, center);
			}
			ue3::Set(a_pawn, F.velocity, UeVector{});
			ue3::Set(a_pawn, F.acceleration, UeVector{});

			const bind::Rot3i look{ McPitchToUe(st.pitch), McYawToUe(st.yaw), 0 };
			ue3::Set(a_pc, F.rotation, look);
			ue3::Set(a_pawn, F.rotation, bind::Rot3i{ 0, look.yaw, 0 });

			// The game's camera at Minecraft's eye (lower while sneaking).
			const float eye = static_cast<float>(a_m.eyeHeight * upb) - a_halfHeight;
			ue3::Set<float>(a_pawn, F.baseEyeHeight, eye);
			ue3::Set<float>(a_pawn, F.eyeHeight, eye);
			SetIgnoreInput(a_pc, true);
			lastSet = center;
			haveLastSet = true;
		}

		void SnapshotCamera(Obj a_pc, Obj a_pawn, float a_halfHeight)
		{
			auto&      st = State();
			CameraView v;
			UeVector   loc{};
			UeRotator  rot{};
			float      fov = 90.0f;
			const Obj  camera = ue3::GetObj(a_pc, F.playerCamera);
			if (camera && F.povLocation >= 0) {
				loc = mem::Read<UeVector>(camera + F.povLocation);
				rot = mem::Read<UeRotator>(camera + F.povRotation);
				fov = mem::Read<float>(camera + F.povFov);
			} else if (a_pawn) {
				const auto l = bind::Location(a_pawn);
				const auto r = bind::Rotation(a_pc);
				loc = { l.x, l.y, l.z + ue3::Get<float>(a_pawn, F.eyeHeight) };
				rot = { r.pitch, r.yaw, r.roll };
				(void)a_halfHeight;
			} else {
				return;
			}
			if (!(fov > 5.0f && fov < 175.0f)) {
				fov = 90.0f;
			}
			const auto eye = UeToMc(loc, st.unitsPerBlock);
			v.valid = true;
			v.x = eye.x;
			v.y = eye.y;
			v.z = eye.z;
			v.yaw = UeYawToMc(rot.yaw);
			v.pitch = UePitchToMc(rot.pitch);
			v.roll = UeAngleToDegrees(rot.roll);
			v.fov = fov;
			std::lock_guard guard(st.viewLock);
			v.frame = st.view.frame + 1;
			st.view = v;
		}

		// ---- what to tell the player --------------------------------------------------------------
		void ReportMinecraft(bool a_connected, float a_delta)
		{
			static float waited = 0.0f;
			static bool  wasConnected = false;
			auto&        st = State();
			if (a_connected) {
				if (!wasConnected) {
					Render::Notify(L"DisCraft: Minecraft connected", 5.0f);
				}
				wasConnected = true;
				waited = 0.0f;
				SetStatus(st.suspended ? L"DisCraft paused (F11): Dishonored has the controls" : L"");
				return;
			}
			wasConnected = false;
			waited += a_delta;
			std::wstring text;
			switch (Launcher::GetStatus()) {
			case Launcher::Status::kSignIn:
				text = L"DisCraft: sign in to your Microsoft account in the Prism Launcher window (Alt-Tab to it), then come back";
				break;
			case Launcher::Status::kNoLauncher:
				text = L"DisCraft: no Minecraft to start (see DisCraft.ini, [Minecraft] sLauncher)";
				break;
			case Launcher::Status::kFailed:
				text = L"DisCraft: couldn't start Minecraft (see %LOCALAPPDATA%\\DisCraft\\DisCraft.log)";
				break;
			case Launcher::Status::kOff:
				text = L"DisCraft: waiting for Minecraft (start the DisCraft instance yourself)";
				break;
			default:
				if (waited < 60.0f) {
					text = L"DisCraft: starting Minecraft...";
				} else if (Launcher::PrismRunning()) {
					text = L"DisCraft: Prism Launcher is still busy (Alt-Tab to it: download, sign-in or an error)";
				} else if (Launcher::MinecraftRunning()) {
					text = L"DisCraft: Minecraft is running but not answering (see DisCraft.log)";
				} else {
					text = L"DisCraft: Minecraft closed (its log: %LOCALAPPDATA%\\DisCraft\\Prism\\instances\\DisCraft\\.minecraft\\logs\\latest.log)";
				}
				break;
			}
			SetStatus(text);
		}

		// ---- the frame ----------------------------------------------------------------------------
		void Tick(Obj a_pc)
		{
			auto& st = State();
			auto& link = Link::Get();
			mem::FlushCache();
			const std::int64_t now = Qpc();
			const float        delta = lastTickQpc ? std::clamp(float(double(now - lastTickQpc) / double(QpcFrequency())), 0.0f, 0.25f) : 0.016f;
			lastTickQpc = now;
			st.lastTickMs = ::GetTickCount64();
			link.Heartbeat();

			// Minecraft.
			const bool mcAlive = link.McAlive();
			const bool haveMc = mcAlive && link.ReadMcState(mc);
			const auto mcPid = link.McPid();
			const bool newMcProcess = mcAlive && mcPid != 0 && mcPid != lastMcPid;
			if (mcAlive) {
				lastMcPid = mcPid;
			}
			if (mcAlive && (!mcWasAlive || newMcProcess)) {
				DC_INFO("Minecraft connected (pid %u)", mcPid);
				link.ResetOverlay();
				settleTimer = kSettleSeconds;
				Collision::Reset(++epoch);
				teleportPending = true;
			}
			mcWasAlive = mcAlive;
			st.mcInWorld = haveMc && (mc.flags & proto::kMcInWorld);
			const bool screenOpen = haveMc && (mc.flags & proto::kMcScreenOpen);
			if (screenOpen && !st.mcScreenOpen) {
				st.cursorX = st.overlayW / 2;
				st.cursorY = st.overlayH / 2;
			}
			st.mcScreenOpen = screenOpen;
			if (haveMc && mc.sensitivity > 0.0f) {
				st.sensitivity = mc.sensitivity;
			}
			st.mcCameraMode = haveMc ? static_cast<int>(mc.cameraMode) : 0;
			ReportMinecraft(mcAlive, delta);

			// The game.
			const Obj  pawnRaw = ue3::GetObj(a_pc, F.controllerPawn);
			const bool pawnOk = pawnRaw && ue3::IsObject(pawnRaw) && ue3::IsA(pawnRaw, C.pawn) && !bind::IsDying(pawnRaw);
			const Obj  pawn = pawnOk ? pawnRaw : 0;
			st.playerPawn = pawn;
			const Obj  world = ue3::GetObj(pawn ? pawn : a_pc, F.worldInfo);
			const bool paused = world && F.pauser && ue3::GetObj(world, F.pauser) != 0;
			const bool cinematic = ue3::GetBool(a_pc, F.cinematicMode);
			const bool alive = pawn && ue3::Get<std::int32_t>(pawn, F.health) > 0;
			float      halfHeight = pawn ? bind::HalfHeight(pawn) : 0.0f;

			if (pawn && !unitsFixed) {
				const float forced = config::Float("World", "fUnitsPerBlock", 0.0f);
				if (forced > 1.0f) {
					st.unitsPerBlock = forced;
				} else if (halfHeight > 10.0f && halfHeight < 500.0f) {
					st.unitsPerBlock = 2.0 * halfHeight / kMcPlayerHeight;
				}
				unitsFixed = true;
				DC_INFO("scale: %.2f Unreal units per block (player cylinder half height %.1f)", static_cast<double>(st.unitsPerBlock), halfHeight);
			}
			if (halfHeight <= 1.0f) {
				halfHeight = static_cast<float>(0.9 * st.unitsPerBlock);
			}

			// World identity: the loaded map. A change wipes Minecraft's collision.
			if (world && world != lastWorld) {
				lastWorld = world;
				Obj outermost = world;
				for (int guard = 0; ue3::OuterOf(outermost) && guard < 16; ++guard) {
					outermost = ue3::OuterOf(outermost);
				}
				const auto mapName = ue3::NameOf(outermost);
				const auto id = Fnv1a(mapName);
				if (id != worldId) {
					DC_INFO("world: %s (%08X)", mapName.c_str(), id);
					worldId = id;
					Collision::Reset(++epoch);
					teleportPending = true;
					settleTimer = kSettleSeconds;
					haveLastSet = false;
				}
			}

			const bool menu = paused || cinematic || st.suspended || !alive;
			if (menu && !st.gameMenuOpen) {
				Input::ReleaseAll();
			}
			st.gameMenuOpen = menu;

			// The game moved its player itself (a load, a script, a cutscene ending).
			const auto pawnLoc = pawn ? bind::Location(pawn) : bind::Vec3f{};
			if (!pawn) {
				haveLastSet = false;
			} else if (haveLastSet) {
				const double dx = pawnLoc.x - lastSet.x, dy = pawnLoc.y - lastSet.y, dz = pawnLoc.z - lastSet.z;
				if (std::sqrt(dx * dx + dy * dy + dz * dz) > kGameMovedBlocks * st.unitsPerBlock) {
					DC_INFO("the game moved the player; resyncing Minecraft");
					teleportPending = true;
					haveLastSet = false;
				}
			}
			// Back from a menu, a cutscene or F11: if the game's player is somewhere else now, Minecraft's
			// player goes there (instead of the game's player being pulled back to Minecraft's).
			if (wasMenu && !menu && pawn && haveMc) {
				const auto   here = UeToMc({ pawnLoc.x, pawnLoc.y, pawnLoc.z - halfHeight }, st.unitsPerBlock);
				const double gap = std::sqrt((here.x - mc.x) * (here.x - mc.x) + (here.y - mc.y) * (here.y - mc.y) + (here.z - mc.z) * (here.z - mc.z));
				if (gap > 1.0) {
					DC_INFO("back in control %.1f blocks from Minecraft's player; resyncing", gap);
					teleportPending = true;
				}
			}
			wasMenu = menu;
			const auto pcRot = bind::Rotation(a_pc);
			if (teleportPending && pawn && !paused) {
				++teleportSeq;
				teleportPending = false;
				st.yaw = UeYawToMc(pcRot.yaw);
				st.pitch = UePitchToMc(pcRot.pitch);
				st.lookInitialized = true;
			}

			// Mouse look (Minecraft's formula), integrated here so the camera has no added latency.
			float dx = 0.0f, dy = 0.0f;
			Input::ConsumeLook(dx, dy);
			lookTravel += std::fabs(dx) + std::fabs(dy);
			if (!st.lookInitialized) {
				st.yaw = UeYawToMc(pcRot.yaw);
				st.pitch = UePitchToMc(pcRot.pitch);
				st.lookInitialized = true;
			}
			if (!st.mcScreenOpen && !menu) {
				const float s = st.sensitivity * 0.6f + 0.2f;
				const float factor = s * s * s * 8.0f * 0.15f;
				st.yaw = static_cast<float>(WrapDegrees(st.yaw + dx * factor));
				st.pitch = std::clamp(st.pitch + dy * factor, -90.0f, 90.0f);
			}

			// Minecraft holds its player after a teleport until the ground has arrived around it. If
			// it's holding somewhere the game's player isn't, that ground never comes: send it again.
			const bool arriving = haveMc && st.mcInWorld && pawn && mc.teleportAck != teleportSeq && !menu;
			if (arriving) {
				const auto   here = UeToMc({ pawnLoc.x, pawnLoc.y, pawnLoc.z - halfHeight }, st.unitsPerBlock);
				const double gap = std::sqrt((here.x - mc.x) * (here.x - mc.x) + (here.y - mc.y) * (here.y - mc.y) + (here.z - mc.z) * (here.z - mc.z));
				holdMismatch = gap > 8.0 ? holdMismatch + delta : 0.0f;
				if (holdMismatch > 1.0f) {
					DC_INFO("Minecraft is waiting %.0f blocks from the game's player; teleporting it again", gap);
					teleportPending = true;
					holdMismatch = 0.0f;
				}
			} else {
				holdMismatch = 0.0f;
			}

			const bool puppet = haveMc && st.mcInWorld && pawn && mc.teleportAck == teleportSeq && !menu;
			if (puppet != wasPuppet) {
				DC_INFO("puppet %s", puppet ? "on (Minecraft drives the player)" : "off");
			}
			st.puppeting = puppet;
			st.routeInput = puppet || arriving;
			st.mcCrosshair = puppet && mc.cameraMode == 0 && !st.mcScreenOpen;
			st.mcGuiScale = haveMc ? static_cast<int>(mc.guiScale) : 0;

			stage = "moving the player";
			if (puppet) {
				const auto m = Interpolate(mc);
				Puppet(a_pc, pawn, m, halfHeight);
				stage = "hiding the game's player";
				HideGamePlayer(a_pc, pawn);
				st.feetX = m.feetX;
				st.feetY = m.feetY;
				st.feetZ = m.feetZ;
				st.feetValid = true;
			} else {
				if (wasPuppet || (takenPawn && !arriving)) {
					Release(a_pc);
				}
				if (arriving) {
					SetIgnoreInput(a_pc, true);  // the game's controls don't move its player meanwhile
					// The game's camera follows the controller: keep turning it with the mouse.
					ue3::Set(a_pc, F.rotation, bind::Rot3i{ McPitchToUe(st.pitch), McYawToUe(st.yaw), 0 });
				}
				st.feetValid = false;
			}
			wasPuppet = puppet;

			// The overlay size Minecraft should render: the back buffer, scaled down to fit the mapping.
			{
				const int sw = st.screenW, sh = st.screenH;
				if (sw > 0 && sh > 0) {
					const double scale = std::min({ 1.0, double(proto::kMaxOverlayW) / sw, double(proto::kMaxOverlayH) / sh });
					st.overlayW = std::max(1, static_cast<int>(sw * scale));
					st.overlayH = std::max(1, static_cast<int>(sh * scale));
				}
			}

			proto::GameState gs{};
			gs.flags = (pawn ? proto::kGameInGame : 0u) | (menu ? proto::kGameMenuOpen : 0u) | (!pawn ? proto::kGameLoading : 0u);
			gs.worldId = worldId;
			gs.collisionEpoch = epoch;
			const auto feet = UeToMc({ pawnLoc.x, pawnLoc.y, pawnLoc.z - halfHeight }, st.unitsPerBlock);
			gs.posX = feet.x;
			gs.posY = feet.y;
			gs.posZ = feet.z;
			gs.yaw = st.yaw;
			gs.pitch = st.pitch;
			gs.teleportSeq = teleportSeq;
			gs.viewportW = static_cast<std::uint32_t>(st.overlayW.load());
			gs.viewportH = static_cast<std::uint32_t>(st.overlayH.load());
			gs.gameHour = 12.0f;
			link.WriteGameState(gs);
			stage = "use key";

			// G (by default): the game's own "use" (doors, pickups, talking), at what the player looks at.
			if (Input::TakeUseRequest() && puppet) {
				static const auto useName = config::String("Controls", "sActivateFunction", "Use");
				if (const Obj use = bind::FunctionOf(a_pc, useName.c_str())) {
					ue3::Params p(use);
					p.Invoke(a_pc);
					DC_DIAG("activate: called %s", ue3::PathOf(use).c_str());
				} else {
					DC_WARN("activate: %s has no function %s", ue3::NameOf(ue3::ClassOf(a_pc)).c_str(), useName.c_str());
				}
			}

			// Every 10 s while Minecraft is connected: enough to tell from a log what each side thinks.
			if (mcAlive && ::GetTickCount64() - statusLoggedAt > 10000) {
				statusLoggedAt = ::GetTickCount64();
				const auto gameFeet = UeToMc({ pawnLoc.x, pawnLoc.y, pawnLoc.z - halfHeight }, st.unitsPerBlock);
				// The controller as the game left it this frame (before ours), and the camera it rendered.
				const auto rotNow = pcRot;
				bind::Rot3i camRot{};
				if (const Obj cam = ue3::GetObj(a_pc, F.playerCamera); cam && F.povRotation >= 0) {
					camRot = mem::Read<bind::Rot3i>(cam + static_cast<std::uintptr_t>(F.povRotation));
				}
				DC_INFO("status: puppet %d, arriving %d, menu %d, mc in world %d, screen %d | mouse %.0f | look %.1f/%.1f, controller %.1f/%.1f, camera %.1f/%.1f | "
						"mc (%.2f, %.2f, %.2f) ack %u/%u, game feet (%.2f, %.2f, %.2f), physics %u | collision %s",
					puppet, arriving, menu, st.mcInWorld.load(), st.mcScreenOpen.load(), lookTravel, st.yaw, st.pitch, UeYawToMc(rotNow.yaw),
					UePitchToMc(rotNow.pitch), UeYawToMc(camRot.yaw), UePitchToMc(camRot.pitch), mc.x, mc.y, mc.z, mc.teleportAck, teleportSeq, gameFeet.x, gameFeet.y, gameFeet.z,
					pawn ? ue3::Get<std::uint8_t>(pawn, F.physics) : 0u, Collision::Summary().c_str());
				lookTravel = 0.0f;
			}
			stage = "camera";
			SnapshotCamera(a_pc, pawn, halfHeight);
			stage = "actors and combat";
			Actors::PerFrame(a_pc, pawn, puppet, delta);
			stage = "collision";
			if (settleTimer > 0.0f) {
				settleTimer -= delta;
			} else if (pawn && mcAlive) {
				// Urgent while Minecraft is waiting for the ground under it.
				Collision::PerFrame(pawn, puppet ? Vec3d{ st.feetX, st.feetY, st.feetZ } : feet, arriving);
			}
		}

		// ---- finding ProcessEvent ---------------------------------------------------------------
		// When native code calls a script event (eventTick, eventPostRender, ...) it goes through
		// UObject::ProcessEvent, which calls the function's Func: a hook on Func then returns into
		// ProcessEvent, whose vtable slot is the entry starting closest below that address. A call
		// made by script (CallFunction) also lands in Func, so only frames without a calling script
		// frame count. Besides the per-frame hook, script Tick/PostRender functions are probed for
		// this until it is found.
		std::unordered_map<Obj, void*> probes;  // probed function -> its Func

		// The vtable entry starting closest below a_returnAddress (within 8 KB), or -1.
		int ClosestBelow(ue3::Obj a_object, std::uintptr_t a_returnAddress, std::uintptr_t& a_start)
		{
			const auto     vtable = static_cast<std::uintptr_t>(mem::Read<ue3::Addr>(a_object));
			int            best = -1;
			std::uintptr_t bestAddr = 0;
			for (int i = 0; i < 300; ++i) {
				if (!mem::Readable(vtable + 4u * i, 4)) {
					break;
				}
				const auto fn = static_cast<std::uintptr_t>(mem::Read<ue3::Addr>(vtable + 4u * i));
				if (!mem::InCode(fn)) {
					continue;
				}
				if (fn <= a_returnAddress && a_returnAddress - fn < 0x2000 && fn > bestAddr) {
					best = i;
					bestAddr = fn;
				}
			}
			a_start = bestAddr;
			return best;
		}

		void FindProcessEvent(Obj a_self, std::uintptr_t a_returnAddress)
		{
			if (ue3::ProcessEventIndex() > 0) {
				processEventSearched = true;
				return;
			}
			// Actors override ProcessEvent (AActor::ProcessEvent checks the world, then calls
			// UObject::ProcessEvent directly), so the return address is in UObject::ProcessEvent,
			// which an actor's vtable doesn't hold. A plain UObject's does: the object's class (a
			// UClass) is one. The slot is the same in every vtable.
			std::uintptr_t start = 0;
			const Obj      plain = ue3::ClassOf(a_self);
			int            slot = plain ? ClosestBelow(plain, a_returnAddress, start) : -1;
			const char*    from = "its class";
			if (slot <= 0) {
				slot = ClosestBelow(a_self, a_returnAddress, start);
				from = "the object itself";
			}
			if (slot > 0) {
				processEventSearched = true;
				ue3::SetProcessEventIndex(slot);
				const auto own = static_cast<std::uintptr_t>(mem::Read<ue3::Addr>(static_cast<std::uintptr_t>(mem::Read<ue3::Addr>(a_self)) + 4u * slot));
				DC_INFO("UE3: ProcessEvent is vtable slot %d (0x%X): returned to %s+0x%X, from a native call on %s (vtable of %s; this object's slot holds %s)",
					slot, slot * 4, ue3::Where(start).c_str(), static_cast<unsigned>(a_returnAddress - start), ue3::FullNameOf(a_self).c_str(), from,
					ue3::Where(own).c_str());
			} else {
				static bool warned = false;
				if (!warned) {
					warned = true;
					DC_WARN("UE3: a native call didn't return into a virtual function (%s); still looking for ProcessEvent", ue3::Where(a_returnAddress).c_str());
				}
			}
		}

		void RemoveProbes()
		{
			for (const auto& [f, original] : probes) {
				ue3::SetFunc(f, original);
			}
			if (!probes.empty()) {
				DC_INFO("UE3: removed %d ProcessEvent probes", static_cast<int>(probes.size()));
			}
			probes.clear();
		}

		void DC_FASTCALL ProbeThunk(void* a_self, void* /*edx*/, void* a_frame, void* a_result)
		{
#ifdef _MSC_VER
			const auto ra = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#else
			const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#endif
			if (!processEventSearched && ue3::FrameCalledFromScript(a_frame, reinterpret_cast<Obj>(a_self)) == 0) {
				FindProcessEvent(reinterpret_cast<Obj>(a_self), ra);
			}
			// Only script functions are probed: they all run ProcessInternal.
			reinterpret_cast<ue3::NativeFn>(ue3::ProcessInternal())(a_self, a_frame, a_result);
		}

		void InstallProbes()
		{
			static int scannedCount = -1;
			if (processEventSearched || ue3::ProcessEventIndex() > 0 || !ue3::ProcessInternal() || ue3::ObjectCount() == scannedCount) {
				return;
			}
			scannedCount = ue3::ObjectCount();
			const auto processInternal = reinterpret_cast<void*>(ue3::ProcessInternal());
			int        added = 0;
			for (const auto& name : config::List("Engine", "sProbeFunctions", "Tick,PostRender,PlayerTick,UpdateCamera")) {
				for (const Obj f : ue3::FunctionsNamed(name)) {
					if (probes.count(f) || tickOriginals.count(f) || ue3::GetFunc(f) != processInternal) {
						continue;
					}
					probes.emplace(f, processInternal);
					ue3::SetFunc(f, reinterpret_cast<void*>(&ProbeThunk));
					++added;
				}
			}
			if (added) {
				DC_INFO("UE3: probing %d script events for ProcessEvent", added);
			}
		}

		void* OriginalFor(void* a_frame, Obj a_self)
		{
			if (sharedOriginal) {
				return sharedOriginal;
			}
			Obj fn = ue3::FrameFunction(a_frame);
			if (!fn) {
				for (const Obj f : tickFunctions) {
					if (ue3::FrameLocals(a_frame, f, a_self)) {
						fn = f;
						break;
					}
				}
			}
			const auto it = tickOriginals.find(fn);
			return it != tickOriginals.end() ? it->second : nullptr;
		}

		void DC_FASTCALL PlayerTickThunk(void* a_self, void* /*edx*/, void* a_frame, void* a_result)
		{
#ifdef _MSC_VER
			const auto ra = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#else
			const auto ra = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
#endif
			const auto self = reinterpret_cast<Obj>(a_self);
			const auto original = reinterpret_cast<ue3::NativeFn>(OriginalFor(a_frame, self));
			const bool outermost = tickDepth == 0;
			++tickDepth;
			if (original) {
				original(a_self, a_frame, a_result);
			} else if (!warnedNoOriginal) {
				warnedNoOriginal = true;
				DC_ERROR("PlayerTick hook: couldn't tell which function is running; the game's own PlayerTick is skipped");
			}
			--tickDepth;
			if (outermost) {
				if (ue3::FrameCalledFromScript(a_frame, self) >= 0) {
					ue3::CaptureFrameTemplate(a_frame);
				}
				if (!processEventSearched && ue3::FrameCalledFromScript(a_frame, self) == 0) {
					FindProcessEvent(self, ra);
				}
				if (processEventSearched && !probes.empty()) {
					RemoveProbes();
				}
				if (!tickBroken) {
					seh::Fault fault;
					stage = "start";
					if (!seh::Run([](void* a_pc) { Tick(reinterpret_cast<Obj>(a_pc)); }, a_self, fault)) {
						tickBroken = true;
						{
							std::lock_guard guard(statusLock);
							brokenAt = stage;
						}
						DC_ERROR("DisCraft stopped: the per-frame update failed in \"%s\": %s", stage, seh::Describe(fault).c_str());
					}
				}
			}
		}

		// The class a function belongs to: its outer, or the class around the state it's declared in.
		Obj OwnerClass(Obj a_function)
		{
			Obj owner = ue3::OuterOf(a_function);
			if (owner && ue3::NameOf(ue3::ClassOf(owner)) == "State") {
				owner = ue3::OuterOf(owner);
			}
			return owner;
		}

		int HookNamed(const std::string& a_name)
		{
			int added = 0;
			for (const Obj f : ue3::FunctionsNamed(a_name)) {
				if (tickOriginals.count(f) || !ue3::IsChildOf(OwnerClass(f), C.playerController)) {
					continue;
				}
				void* original = ue3::GetFunc(f);
				if (const auto probe = probes.find(f); probe != probes.end()) {
					original = probe->second;  // ours from now on, not a probe's
					probes.erase(probe);
				}
				if (!original || original == reinterpret_cast<void*>(&PlayerTickThunk)) {
					continue;
				}
				tickOriginals.emplace(f, original);
				tickFunctions.push_back(f);
				ue3::SetFunc(f, reinterpret_cast<void*>(&PlayerTickThunk));
				++added;
				DC_INFO("hooked %s (%s)", ue3::PathOf(f).c_str(), ue3::IsNativeFunction(f) ? "native" : "script");
			}
			return added;
		}

		std::string Lower(std::string a_text)
		{
			for (auto& c : a_text) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return a_text;
		}

		// Logged once when no candidate matched: what the engine has, for picking sTickFunction.
		void DescribeTickCandidates(const std::vector<std::string>& a_names)
		{
			for (const auto& name : a_names) {
				const auto found = ue3::FunctionsNamed(name);
				DC_INFO("tick candidates: %d function(s) named %s", static_cast<int>(found.size()), name.c_str());
				int shown = 0;
				for (const Obj f : found) {
					if (++shown > 12) {
						break;
					}
					const Obj owner = OwnerClass(f);
					DC_INFO("  %s: owner %s, player controller %s, func %p", ue3::PathOf(f).c_str(), ue3::FullNameOf(owner).c_str(),
						ue3::IsChildOf(owner, C.playerController) ? "yes" : "no", ue3::GetFunc(f));
				}
			}
			// The class of a live player controller, if there is one yet, else PlayerController itself.
			Obj       cls = C.playerController;
			const int count = ue3::ObjectCount();
			for (int i = 0; i < count; ++i) {
				const Obj o = ue3::ObjectAt(i);
				if (o && ue3::IsA(o, C.playerController) && !ue3::IsDefaultObject(o)) {
					cls = ue3::ClassOf(o);
					break;
				}
			}
			int listed = 0;
			for (Obj s = cls; s && listed < 80; s = ue3::SuperOf(s)) {
				for (const Obj f : ue3::ChildrenOf(s)) {
					if (ue3::NameOf(ue3::ClassOf(f)) != "Function") {
						continue;
					}
					const auto name = ue3::NameOf(f);
					if (Lower(name).find("tick") != std::string::npos || Lower(name).find("move") != std::string::npos) {
						DC_INFO("  function %s.%s", ue3::NameOf(s).c_str(), name.c_str());
						++listed;
					}
				}
			}
			DC_INFO("tick candidates: player controller class %s, %d tick/move functions listed", ue3::PathOf(cls).c_str(), listed);
		}

		void HookTickFunctions()
		{
			// Only when packages were loaded since the last look (a map load adds classes), or once a
			// minute (new objects can also reuse freed slots without growing the array).
			static int       scannedCount = -1;
			static ULONGLONG scannedAt = 0;
			const int        objectCount = ue3::ObjectCount();
			const ULONGLONG  now = ::GetTickCount64();
			if (objectCount == scannedCount && now - scannedAt < 60000) {
				return;
			}
			scannedCount = objectCount;
			scannedAt = now;
			// The first candidate that exists wins and is kept; hooking two (say PlayerTick and Tick)
			// would run our update twice a frame.
			const auto names = tickName.empty() ? config::List("Engine", "sTickFunction", "PlayerTick,PlayerMove,Tick") : std::vector<std::string>{ tickName };
			int        added = 0;
			for (const auto& name : names) {
				added += HookNamed(name);
				if (!tickFunctions.empty()) {
					tickName = name;
					break;
				}
			}
			if (!added) {
				return;
			}
			sharedOriginal = tickOriginals.begin()->second;
			for (const auto& [f, original] : tickOriginals) {
				if (original != sharedOriginal) {
					sharedOriginal = nullptr;
					break;
				}
			}
		}
	}

	namespace Game
	{
		bool Install()
		{
			auto& st = State();
			if (st.installed) {
				HookTickFunctions();  // overrides from packages loaded since
				InstallProbes();
				return true;
			}
			static bool bound = false;
			if (!bound) {
				if (!bind::Bind()) {
					return false;
				}
				bound = true;
				epoch = static_cast<std::uint32_t>(::GetTickCount64() & 0xFFFF) << 8;
				teleportSeq = epoch;
			}
			HookTickFunctions();
			if (tickFunctions.empty()) {
				// Retried as packages load (the frame update may live in a class loaded later);
				// said once, with what the engine has instead.
				if (!tickMissing) {
					tickMissing = true;
					const auto names = config::List("Engine", "sTickFunction", "PlayerTick,PlayerMove,Tick");
					DC_ERROR("no %s function found on a PlayerController class yet; still looking", config::String("Engine", "sTickFunction", "PlayerTick,PlayerMove,Tick").c_str());
					DescribeTickCandidates(names);
				}
				return false;
			}
			InstallProbes();
			Actors::Install();
			st.installed = true;
			tickMissing = false;
			DC_INFO("DisCraft installed into the game");
			return true;
		}

		std::wstring StatusText()
		{
			auto& st = State();
			if (tickBroken) {
				std::lock_guard guard(statusLock);
				return L"DisCraft stopped after an error (" + std::wstring(brokenAt.begin(), brokenAt.end()) + L"); see %LOCALAPPDATA%\\DisCraft\\DisCraft.log";
			}
			if (discoveryFailed) {
				return L"DisCraft: Unreal Engine data not found; see %LOCALAPPDATA%\\DisCraft\\DisCraft.log";
			}
			if (!st.installed) {
				return tickMissing ? L"DisCraft: can't hook the game's frame update yet; see %LOCALAPPDATA%\\DisCraft\\DisCraft.log" : L"";
			}
			std::lock_guard guard(statusLock);
			return status;
		}

		void SetDiscoveryFailed(bool a_failed) { discoveryFailed = a_failed; }

		CameraView View()
		{
			auto&           st = State();
			std::lock_guard guard(st.viewLock);
			return st.view;
		}
	}
}
