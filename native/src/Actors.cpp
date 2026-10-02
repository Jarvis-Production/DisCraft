#include "Game.h"

#include "Bindings.h"
#include "Config.h"
#include "Log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Combat between the Minecraft player and Dishonored's characters.
//
//  - Every pawn near the player goes into the actor table; Minecraft mirrors each as an invisible,
//    hittable stand-in (DisCombat / DishonoredActorEntity in the Fabric mod).
//  - Minecraft's hits on a stand-in come back as events; the game applies them with the pawn's own
//    TakeDamage, so Dishonored's AI reacts, alerts and fights back as it would to Corvo's sword.
//  - Damage to the player is Minecraft's business (its health, armour, shield): the game's damage
//    is intercepted and forwarded, and the game's own player never loses health while Minecraft
//    drives it. Two ways, so it works whatever path the damage takes: script TakeDamage on the
//    player's pawn is cancelled and forwarded, and anything that still lowers Health is taken
//    back from a large health buffer and forwarded too.
namespace discraft::Actors
{
	namespace
	{
		using ue3::Obj;
		using bind::C;
		using bind::F;
		using bind::Fn;

		constexpr double kRangeBlocks = 64.0;

		float toGame = 8.0f;          // Minecraft damage -> game damage
		float fromGame = 1.0f;        // game damage -> the "game damage" Minecraft divides by 5
		float explosionDamage = 60.0f;
		float knockback = 400.0f;      // game momentum per Minecraft knockback unit
		bool  hookTakeDamage = true;

		Obj damageMelee = 0, damageProjectile = 0, damageExplosion = 0, damageGeneric = 0;

		std::vector<proto::ActorRecord> records;
		std::vector<Obj>                pawnCache;
		ULONGLONG                       pawnCacheMs = 0;

		Obj  bufferedPawn = 0;
		int  healthBuffer = 0;
		int  lastSetHealth = 0;
		bool clampMode = false;
		bool allowPlayerDamage = false;

		std::unordered_map<Obj, void*> damageOriginals;
		void*                          damageShared = nullptr;
		bool                           damageSharedOk = false;

		std::string Lower(std::string a_s)
		{
			std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}

		Obj FindDamageType(const char* a_key, std::initializer_list<const char*> a_hints, const std::vector<Obj>& a_all)
		{
			const auto configured = config::String("Combat", a_key, "");
			if (!configured.empty()) {
				if (configured.find('.') != std::string::npos) {
					if (const Obj c = ue3::FindClass(configured)) {
						return c;
					}
				}
				for (const Obj c : a_all) {
					if (Lower(ue3::NameOf(c)) == Lower(configured)) {
						return c;
					}
				}
				DC_WARN("combat: damage type %s (%s) not found; picking one", configured.c_str(), a_key);
			}
			for (const char* hint : a_hints) {
				for (const Obj c : a_all) {
					if (Lower(ue3::NameOf(c)).find(hint) != std::string::npos) {
						return c;
					}
				}
			}
			return C.damageType;
		}

		std::uint16_t HurtKindOf(Obj a_damageType)
		{
			const auto name = Lower(ue3::NameOf(a_damageType));
			for (const char* k : { "bullet", "bolt", "arrow", "projectile", "pistol", "crossbow", "gun", "ranged", "dart", "shot" }) {
				if (name.find(k) != std::string::npos) {
					return proto::kHurtProjectile;
				}
			}
			for (const char* k : { "fire", "electric", "magic", "possess", "swarm", "plague", "spark", "tesla", "wall", "burn" }) {
				if (name.find(k) != std::string::npos) {
					return proto::kHurtMagic;
				}
			}
			for (const char* k : { "explosion", "grenade", "mine", "fall", "drown" }) {
				if (name.find(k) != std::string::npos) {
					return proto::kHurtOther;
				}
			}
			return proto::kHurtMelee;
		}

		void SendHurt(std::uint16_t a_kind, double a_gameDamage, std::uint32_t a_attacker)
		{
			const auto amount = static_cast<std::int32_t>(std::lround(a_gameDamage * fromGame * 100.0));
			if (amount <= 0) {
				return;
			}
			Link::Get().PushInput(proto::kInHurt, a_kind, amount, static_cast<std::int32_t>(a_attacker), 0);
			DC_DIAG("combat: the game hit the player for %.1f (kind %u, attacker %u)", a_gameDamage, a_kind, a_attacker);
		}

		// ---- the player's TakeDamage -----------------------------------------------------------
		void DC_FASTCALL TakeDamageThunk(void* a_self, void* /*edx*/, void* a_frame, void* a_result)
		{
			const auto self = reinterpret_cast<Obj>(a_self);
			Obj        fn = ue3::FrameFunction(a_frame);
			if (!fn) {
				for (const auto& [f, original] : damageOriginals) {
					if (ue3::FrameLocals(a_frame, f, self)) {
						fn = f;
						break;
					}
				}
			}
			void* original = damageSharedOk ? damageShared : nullptr;
			if (!original) {
				const auto it = damageOriginals.find(fn);
				original = it != damageOriginals.end() ? it->second : nullptr;
			}
			auto& st = State();
			if (!allowPlayerDamage && (st.puppeting || st.mirrorButtons) && self == st.playerPawn.load() && fn) {
				if (std::uint8_t* locals = ue3::FrameLocals(a_frame, fn, self)) {
					const auto amountField = ue3::FindField(fn, "DamageAmount");
					const auto instigatorField = ue3::FindField(fn, "EventInstigator");
					const auto typeField = ue3::FindField(fn, "DamageType");
					double     amount = 0.0;
					if (amountField.kind == "FloatProperty") {
						float v;
						std::memcpy(&v, locals + amountField.offset, 4);
						amount = v;
					} else if (amountField) {
						std::int32_t v;
						std::memcpy(&v, locals + amountField.offset, 4);
						amount = v;
					}
					ue3::Addr instigator = 0, type = 0;
					if (instigatorField) {
						std::memcpy(&instigator, locals + instigatorField.offset, 4);
					}
					if (typeField) {
						std::memcpy(&type, locals + typeField.offset, 4);
					}
					if (amount >= 0.0 && amount < 1.0e6) {
						const Obj attackerPawn = instigator ? ue3::GetObj(instigator, F.controllerPawn) : 0;
						SendHurt(HurtKindOf(type), amount, attackerPawn ? static_cast<std::uint32_t>(ue3::IndexOf(attackerPawn)) : 0u);
						return;  // the game's player takes nothing: Minecraft's health is the one that counts
					}
				}
			}
			if (original) {
				reinterpret_cast<ue3::NativeFn>(original)(a_self, a_frame, a_result);
			}
		}

		void HookTakeDamage()
		{
			for (const Obj f : ue3::FunctionsNamed("TakeDamage")) {
				const Obj owner = ue3::OuterOf(f);
				// Only script versions on pawns: a native one reads its own parameters from the
				// caller's bytecode, so it can't be skipped.
				if (damageOriginals.count(f) || !ue3::IsChildOf(owner, C.pawn) || ue3::IsNativeFunction(f)) {
					continue;
				}
				void* original = ue3::GetFunc(f);
				if (!original || original == reinterpret_cast<void*>(&TakeDamageThunk)) {
					continue;
				}
				damageOriginals.emplace(f, original);
				ue3::SetFunc(f, reinterpret_cast<void*>(&TakeDamageThunk));
				DC_INFO("combat: hooked %s", ue3::PathOf(f).c_str());
			}
			damageShared = damageOriginals.empty() ? nullptr : damageOriginals.begin()->second;
			damageSharedOk = !damageOriginals.empty();
			for (const auto& [f, original] : damageOriginals) {
				damageSharedOk &= original == damageShared;
			}
		}

		// ---- the player's health ---------------------------------------------------------------
		int HealthMax(Obj a_pawn)
		{
			const int max = F.healthMax ? ue3::Get<std::int32_t>(a_pawn, F.healthMax) : 0;
			return max > 0 ? max : 100;
		}

		void KeepHealth(Obj a_pawn)
		{
			const int max = HealthMax(a_pawn);
			const int health = ue3::Get<std::int32_t>(a_pawn, F.health);
			if (bufferedPawn != a_pawn) {
				bufferedPawn = a_pawn;
				clampMode = false;
				healthBuffer = max * 20;
				ue3::Set<std::int32_t>(a_pawn, F.health, healthBuffer);
				lastSetHealth = healthBuffer;
				return;
			}
			if (!clampMode && lastSetHealth > max && health == max) {
				// The game keeps Health at most HealthMax: buffer at the maximum instead.
				clampMode = true;
				healthBuffer = max;
				DC_INFO("combat: the game clamps the player's health; buffering at %d", max);
			}
			if (health < healthBuffer && health > -1000000) {
				SendHurt(proto::kHurtOther, healthBuffer - health, 0);
			}
			if (health != healthBuffer) {
				ue3::Set<std::int32_t>(a_pawn, F.health, healthBuffer);
			}
			lastSetHealth = healthBuffer;
		}

		void Kill(Obj a_pc, Obj a_pawn)
		{
			allowPlayerDamage = true;
			bufferedPawn = 0;
			const auto loc = bind::Location(a_pawn);
			if (Fn.died) {
				ue3::Params p(Fn.died);
				p.Set<ue3::Addr>("Killer", 0);
				p.Set<ue3::Addr>("DamageType", static_cast<ue3::Addr>(damageGeneric));
				p.Set("HitLocation", loc);
				ue3::Set<std::int32_t>(a_pawn, F.health, 0);
				p.Invoke(a_pawn);
			} else if (const Obj takeDamage = bind::FunctionOf(a_pawn, "TakeDamage")) {
				ue3::Params p(takeDamage);
				p.SetNumber("DamageAmount", 100000);
				p.Set<ue3::Addr>("DamageType", static_cast<ue3::Addr>(damageGeneric));
				p.Set("HitLocation", loc);
				ue3::Set<std::int32_t>(a_pawn, F.health, 1);
				p.Invoke(a_pawn);
			}
			allowPlayerDamage = false;
			(void)a_pc;
			DC_INFO("combat: the Minecraft player died; so does the game's");
		}

		// ---- Minecraft's events ----------------------------------------------------------------
		void HitActor(const proto::McEvent& a_ev, Obj a_pc, Obj a_pawn)
		{
			const Obj target = ue3::ObjectAt(static_cast<int>(a_ev.actorId));
			static int hitsLogged = 0;
			if (hitsLogged < 5) {
				++hitsLogged;
				DC_INFO("combat: Minecraft hit actor #%u (%s)", a_ev.actorId, target && ue3::IsObject(target) ? ue3::PathOf(target).c_str() : "gone");
			}
			if (!target || !ue3::IsObject(target) || !ue3::IsA(target, C.pawn) || bind::IsDying(target) || target == a_pawn) {
				return;
			}
			const Obj takeDamage = bind::FunctionOf(target, "TakeDamage");
			if (!takeDamage) {
				return;
			}
			const double upb = State().unitsPerBlock;
			const double damage = std::max(1.0, static_cast<double>(a_ev.a) * toGame);
			// Knockback direction (Minecraft x/z) as a game momentum.
			const auto   dir = McToUe(a_ev.b, 0.0, a_ev.c, 1.0);
			const double push = a_ev.d * knockback;
			const UeVector momentum{ static_cast<float>(dir.x * push), static_cast<float>(dir.y * push), static_cast<float>(push * 0.25) };
			const Obj      type = a_ev.weapon == proto::kWeaponArrow ? damageProjectile : damageMelee;
			const auto     loc = bind::Location(target);
			ue3::Params    p(takeDamage);
			p.SetNumber("DamageAmount", damage).SetNumber("Damage", damage);  // UE3 / Dishonored names
			p.Set<ue3::Addr>("EventInstigator", static_cast<ue3::Addr>(a_pc)).Set<ue3::Addr>("InstigatedBy", static_cast<ue3::Addr>(a_pc));
			p.Set("HitInfo", std::array<std::uint8_t, 0x1C>{});  // a native wants its out struct passed
			p.Set("HitLocation", loc);
			p.Set("Momentum", momentum);
			p.Set<ue3::Addr>("DamageType", static_cast<ue3::Addr>(type));
			p.Set<ue3::Addr>("DamageCauser", static_cast<ue3::Addr>(a_pawn));
			const std::int32_t before = ue3::Get<std::int32_t>(target, F.health);
			p.Invoke(target);
			std::int32_t after = ue3::Get<std::int32_t>(target, F.health);
			// Dishonored's TakeDamage can leave health as it was (its own damage rules, or the call
			// not reaching the script): take it off directly, and kill when it runs out.
			if (after >= before && !bind::IsDying(target)) {
				after = before - static_cast<std::int32_t>(damage);
				ue3::Set<std::int32_t>(target, F.health, std::max(after, 0));
				if (after <= 0 && Fn.died) {
					ue3::Params d(Fn.died);
					d.Set<ue3::Addr>("Killer", static_cast<ue3::Addr>(a_pc));
					d.Set<ue3::Addr>("DamageType", static_cast<ue3::Addr>(type));
					d.Set("HitLocation", loc);
					d.Invoke(target);
				}
				static int logged = 0;
				if (logged++ < 5) {
					DC_INFO("combat: TakeDamage left %s at %d; set health to %d directly", ue3::NameOf(target).c_str(), before, after);
				}
			}
			DC_DIAG("combat: Minecraft hit %s for %.1f (game %.0f), health %d -> %d", ue3::NameOf(target).c_str(), a_ev.a, damage, before,
				ue3::Get<std::int32_t>(target, F.health));
			(void)upb;
		}

		void Explosion(const proto::McEvent& a_ev, Obj a_pc, Obj a_pawn)
		{
			if (!Fn.hurtRadius) {
				return;
			}
			const double upb = State().unitsPerBlock;
			ue3::Params  p(Fn.hurtRadius);
			p.SetNumber("BaseDamage", explosionDamage * std::max(1.0f, a_ev.d / 4.0f));
			p.SetNumber("DamageRadius", a_ev.d * upb * 1.5);
			p.Set<ue3::Addr>("DamageType", static_cast<ue3::Addr>(damageExplosion));
			p.SetNumber("Momentum", 40000.0 * a_ev.d / 4.0);
			p.Set("HurtOrigin", McToUe(a_ev.a, a_ev.b, a_ev.c, upb));
			p.Set<ue3::Addr>("IgnoredActor", static_cast<ue3::Addr>(a_pawn));  // Minecraft hurts its own player
			p.Set<ue3::Addr>("InstigatedByController", static_cast<ue3::Addr>(a_pc));
			p.Invoke(a_pawn);
			DC_DIAG("combat: Minecraft explosion radius %.1f", a_ev.d);
		}

		void CollectPawns(Obj a_pawn)
		{
			pawnCache.clear();
			const Obj world = ue3::GetObj(a_pawn, F.worldInfo);
			if (world && F.pawnList && F.nextPawn) {
				int guard = 0;
				for (Obj p = ue3::GetObj(world, F.pawnList); p && guard < 4096; p = ue3::GetObj(p, F.nextPawn), ++guard) {
					pawnCache.push_back(p);
				}
				return;
			}
			// No pawn list: scan every object now and then.
			static std::vector<Obj> scanned;
			const ULONGLONG         now = ::GetTickCount64();
			if (now - pawnCacheMs > 500) {
				pawnCacheMs = now;
				scanned.clear();
				const int count = ue3::ObjectCount();
				for (int i = 0; i < count; ++i) {
					const Obj o = ue3::ObjectAt(i);
					if (o && ue3::IsA(o, C.pawn) && !ue3::IsDefaultObject(o)) {
						scanned.push_back(o);
					}
				}
			}
			pawnCache = scanned;
		}

		std::string DisplayName(Obj a_pawn)
		{
			auto name = ue3::NameOf(ue3::ClassOf(a_pawn));
			for (const char* prefix : { "DisPawn_", "DisPawn", "Pawn_" }) {
				const std::size_t n = std::strlen(prefix);
				if (name.size() > n && name.compare(0, n, prefix) == 0) {
					name = name.substr(n);
					break;
				}
			}
			return name;
		}
	}

	bool Install()
	{
		toGame = config::Float("Combat", "fDamageToGame", 8.0f);
		fromGame = config::Float("Combat", "fDamageFromGame", 1.0f);
		explosionDamage = config::Float("Combat", "fExplosionDamage", 60.0f);
		knockback = config::Float("Combat", "fKnockback", 400.0f);
		hookTakeDamage = config::Bool("Combat", "bHookTakeDamage", true);

		std::vector<Obj> types;
		const Obj        classClass = ue3::FindClass("Core.Class");
		if (C.damageType && classClass) {
			const int count = ue3::ObjectCount();
			for (int i = 0; i < count; ++i) {
				const Obj o = ue3::ObjectAt(i);
				if (o && ue3::ClassOf(o) == classClass && ue3::IsChildOf(o, C.damageType)) {
					types.push_back(o);
				}
			}
		}
		std::string list;
		for (const Obj t : types) {
			list += (list.empty() ? "" : ", ") + ue3::PathOf(t);
		}
		DC_INFO("combat: damage types in the game: %s", list.empty() ? "(none found)" : list.c_str());
		damageMelee = FindDamageType("sDamageTypeMelee", { "sword", "melee", "blade", "slash" }, types);
		damageProjectile = FindDamageType("sDamageTypeProjectile", { "bolt", "arrow", "projectile", "bullet" }, types);
		damageExplosion = FindDamageType("sDamageTypeExplosion", { "explosion", "grenade", "explosive" }, types);
		damageGeneric = C.damageType;
		DC_INFO("combat: Minecraft melee hits as %s, arrows as %s, explosions as %s", ue3::PathOf(damageMelee).c_str(),
			ue3::PathOf(damageProjectile).c_str(), ue3::PathOf(damageExplosion).c_str());
		if (hookTakeDamage) {
			HookTakeDamage();
		}
		return true;
	}

	void Release(Obj a_pawn)
	{
		if (bufferedPawn == a_pawn && a_pawn) {
			ue3::Set<std::int32_t>(a_pawn, F.health, HealthMax(a_pawn));
		}
		bufferedPawn = 0;
	}

	void PerFrame(Obj a_controller, Obj a_pawn, bool a_puppeting, float a_delta)
	{
		(void)a_delta;
		auto& link = Link::Get();
		if (!a_pawn) {
			proto::McEvent ignored{};
			while (link.PopEvent(ignored)) {
			}
			link.WriteActors(nullptr, 0);
			return;
		}

		proto::McEvent ev{};
		while (link.PopEvent(ev)) {
			switch (ev.type) {
			case proto::kEvHitActor:
				HitActor(ev, a_controller, a_pawn);
				break;
			case proto::kEvPlayerDied:
				if (a_puppeting) {
					Kill(a_controller, a_pawn);
				}
				break;
			case proto::kEvExplosion:
				Explosion(ev, a_controller, a_pawn);
				break;
			default:
				break;  // arrows stuck in actors, SkyCraft's skills: nothing to do in Dishonored
			}
		}
		if (a_puppeting) {
			KeepHealth(a_pawn);
		}

		CollectPawns(a_pawn);
		const double upb = State().unitsPerBlock;
		const auto   me = bind::Location(a_pawn);
		records.clear();
		std::vector<std::pair<double, proto::ActorRecord>> nearby;
		for (const Obj p : pawnCache) {
			if (p == a_pawn || !ue3::IsObject(p) || bind::IsDying(p) || ue3::IsDefaultObject(p)) {
				continue;
			}
			const auto   loc = bind::Location(p);
			const double dx = (loc.x - me.x) / upb, dy = (loc.y - me.y) / upb, dz = (loc.z - me.z) / upb;
			const double dist2 = dx * dx + dy * dy + dz * dz;
			if (dist2 > kRangeBlocks * kRangeBlocks) {
				continue;
			}
			const float halfHeight = std::max(bind::HalfHeight(p), static_cast<float>(0.15 * upb));
			const float radius = std::max(bind::Radius(p), static_cast<float>(0.15 * upb));
			const auto  feet = UeToMc({ loc.x, loc.y, loc.z - halfHeight }, upb);
			const int   health = ue3::Get<std::int32_t>(p, F.health);
			const int   healthMax = F.healthMax ? ue3::Get<std::int32_t>(p, F.healthMax) : 0;
			const Obj   controller = ue3::GetObj(p, F.pawnController);
			const bool  hostile = controller && F.enemy && ue3::GetObj(controller, F.enemy) == a_pawn;

			proto::ActorRecord r{};
			r.actorId = static_cast<std::uint32_t>(ue3::IndexOf(p));
			r.flags = (hostile ? proto::kActorHostile | proto::kActorInCombat : 0u) | (health <= 0 ? proto::kActorDead : 0u);
			r.x = static_cast<float>(feet.x);
			r.y = static_cast<float>(feet.y);
			r.z = static_cast<float>(feet.z);
			r.yaw = UeYawToMc(bind::Rotation(p).yaw);
			r.width = static_cast<float>(2.0 * radius / upb);
			r.height = static_cast<float>(2.0 * halfHeight / upb);
			r.healthFrac = healthMax > 0 ? std::clamp(float(health) / float(healthMax), 0.0f, 1.0f) : (health > 0 ? 1.0f : 0.0f);
			r.level = 1;
			const auto name = DisplayName(p);
			std::strncpy(r.name, name.c_str(), sizeof(r.name) - 1);
			nearby.emplace_back(dist2, r);
		}
		std::sort(nearby.begin(), nearby.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		for (const auto& [d, r] : nearby) {
			if (records.size() >= proto::kMaxActors) {
				break;
			}
			records.push_back(r);
		}
		link.WriteActors(records.data(), static_cast<std::uint32_t>(records.size()));
	}
}
