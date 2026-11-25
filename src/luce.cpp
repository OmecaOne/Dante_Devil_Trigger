// The Luce: while dsdtluceMGEF is active, emitters around the actor fire the Luce spell at the target or toward a free-aim point.
// Design notes: luce.h.

#include "luce.h"

#include "TrueDirectionalMovementAPI.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <mutex>
#include <random>
#include <unordered_map>
#include <vector>

namespace
{
	// ===== Tuning (distances in game units, times in seconds) =====

	constexpr std::string_view kPlugin = "OmecaOneDanteSDT.esp";

	constexpr int   kEmitters = 30;
	constexpr float kMinInterval = 0.1f;
	constexpr float kMaxInterval = 0.8f;
	constexpr int   kMaxShotsPerUpdate = 6;  // a long frame spreads its due shots over the next frames

	constexpr float kRadius = 120.0f;
	constexpr float kGridStep = 24.0f;
	constexpr int   kGridMax = 5;  // kGridMax * kGridStep = kRadius: every grid point inside this circle lies on the sphere
	constexpr float kCenterLift = 60.0f;  // height offset for the floating animation
	constexpr float kTargetHeight = 0.6f;

	// Free aim (no target): the shots converge from the spawn sphere to a circle of kConvergeRadius (about an NPC's half width) at a
	// point ahead, each on a gravity arc that passes through its spot (see AimAngles).
	constexpr float kConvergeRadius = 30.0f;
	constexpr float kConvergeDefault = 1500.0f;   // no actor ahead
	constexpr float kConvergeMin = 150.0f;        // an actor in the corridor sets the distance: nearer than this is ignored
	constexpr float kConvergeMax = 4000.0f;       // speed 3000 / gravity 2.0 reach at most ~6550 units
	constexpr float kConvergeCorridor = 150.0f;   // max sideways distance of that actor from the heading line
	constexpr float kGravityScale = 686.378662f;  // engine constant (ID 210017): units/s^2 per point of projectile gravity

	// The spawn circle (dsdtluceexp, majin7up.nif) is an explosion. Explosion::Initialize raises its lifetime to the nif's longest
	// controller (mIdle, 9.67 s), so without help each circle stays ~10 s; it is ended after kFlashLife instead.
	constexpr float kFlashLife = 0.15f;
	// Explosion members on 1.7.104, read from Explosion::Update (ID 43836): age +0xA8, lifetime +0xAC (CommonLib's header says
	// +0xA0/+0xA4). Update deletes the explosion itself (SetDelete) once age > lifetime.
	constexpr std::size_t kExplosionAge = 0xA8;
	constexpr std::size_t kExplosionLifetime = 0xAC;

	// Firing sound: NEAE's DanteSDTLuceLoop (LuceLoop.wav, 3.16 s), played by the moveset's loop clip at 0.033 s of every 1.017 s loop.
	// Only its priority changes: 88 -> 128, the same as the shots' impact sound (MAGFireboltImpactSD).
	constexpr std::string_view kSoundPlugin = "NotEnoughAttackEffect.esp";
	constexpr RE::FormID       kSoundID = 0x1047;
	constexpr std::uint8_t     kSoundPriority = 128;

	// ===== Forms =====

	struct Forms
	{
		RE::EffectSetting* effect = nullptr;     // dsdtluceMGEF
		RE::SpellItem*     spell = nullptr;      // dsdtlucefireSP
		RE::BGSExplosion*  explosion = nullptr;  // dsdtluceexp
		RE::BGSProjectile* projectile = nullptr; // dsdtluceprojectile (both kinds of shot)
	} g;

	TDM_API::IVTDM1* g_tdm = nullptr;  // TDM's API (the player's lock-on target); nullptr without TDM

	// ===== State =====

	// a spawn circle still showing; removed after kFlashLife
	struct Flash
	{
		RE::ObjectRefHandle handle;
		float               left = kFlashLife;
	};

	// per firing actor (effect updates run on worker threads, so behind g_lock)
	struct State
	{
		std::array<float, kEmitters> timers{};
		const RE::ActiveEffect*      owner = nullptr;  // the effect that drives the timers; a recast can overlap the old one
		float                        ownerSilent = 0.0f;
		std::uint32_t                shots = 0;
		std::vector<Flash>           flashes;  // spawn circles still showing
	};

	std::mutex                             g_lock;
	std::unordered_map<RE::FormID, State>  g_states;

	// ===== Helpers =====

	float RandomFloat(float a_min, float a_max)
	{
		thread_local std::mt19937 rng{ std::random_device{}() };
		return std::uniform_real_distribution<float>(a_min, a_max)(rng);
	}

	int RandomInt(int a_min, int a_max)
	{
		thread_local std::mt19937 rng{ std::random_device{}() };
		return std::uniform_int_distribution<int>(a_min, a_max)(rng);
	}

	// the actor's state; a new one starts its emitters at random times (caller holds g_lock)
	State& StateOf(RE::FormID a_id)
	{
		auto [it, added] = g_states.try_emplace(a_id);
		if (added) {
			for (auto& timer : it->second.timers) {
				timer = RandomFloat(0.0f, kMaxInterval);
			}
		}
		return it->second;
	}

	// the actor an active effect is on
	RE::Actor* TargetActor(RE::ActiveEffect* a_effect)
	{
		auto* ref = a_effect->target ? a_effect->target->GetTargetStatsObject() : nullptr;
		return ref ? ref->As<RE::Actor>() : nullptr;
	}

	bool IsLuce(RE::ActiveEffect* a_effect)
	{
		return g.effect && a_effect && a_effect->GetBaseObject() == g.effect;
	}

	// what the shots aim at: TDM lock-on target (player) or combat target (NPCs); nullptr = free aim
	RE::Actor* TargetOf(RE::Actor* a_actor)
	{
		RE::ActorHandle handle;
		if (a_actor->IsPlayerRef()) {
			if (g_tdm) {
				handle = g_tdm->GetCurrentTarget();
			}
		} else {
			handle = a_actor->currentCombatTarget;
		}
		auto target = handle.get();
		return target && !target->IsDead() ? target.get() : nullptr;
	}

	// ===== Aiming =====

	// a random point on the spawn sphere's left or right side; a_side / a_up: its offset from the center, to the actor's right / up
	RE::NiPoint3 SpawnPoint(RE::Actor* a_actor, float a_heading, float& a_side, float& a_up)
	{
		// x^2 + y^2 + z^2 = R^2, on the actor's left and right
		int gridY = 0;
		int gridZ = 0;
		do {
			gridY = RandomInt(-kGridMax, kGridMax);
			gridZ = RandomInt(-kGridMax, kGridMax);
		} while (gridY * gridY + gridZ * gridZ > kGridMax * kGridMax);

		const float moveY = static_cast<float>(gridY) * kGridStep;
		const float moveZ = static_cast<float>(gridZ) * kGridStep;
		float       moveX = std::floor(std::sqrt(kRadius * kRadius - moveY * moveY - moveZ * moveZ));
		if (RandomInt(0, 1) == 1) {
			moveX = -moveX;
		}

		const float sinH = std::sin(a_heading);
		const float cosH = std::cos(a_heading);
		auto        point = a_actor->GetPosition();
		point.x += sinH * moveY + cosH * moveX;
		point.y += cosH * moveY - sinH * moveX;
		point.z += std::floor(a_actor->GetHeight() / 2.0f) + kCenterLift + moveZ;
		a_side = moveX;
		a_up = moveZ;
		return point;
	}

	// Free-aim convergence point: on the actor's heading, at the distance of the nearest actor in a corridor ahead (only its distance and
	// chest height are used, not its sideways position), else kConvergeDefault ahead at the actor's own chest height.
	RE::NiPoint3 FreeAimPoint(RE::Actor* a_actor)
	{
		const float heading = a_actor->GetAngleZ();
		const float fx = std::sin(heading);
		const float fy = std::cos(heading);
		const auto  own = a_actor->GetPosition();
		float       best = kConvergeMax + 1.0f;
		float       bestZ = 0.0f;
		auto        check = [&](RE::Actor* a_other) {
			if (!a_other || a_other == a_actor || a_other->IsDead() || !a_other->Is3DLoaded()) {
				return;
			}
			const auto  pos = a_other->GetPosition();
			const float rx = pos.x - own.x;
			const float ry = pos.y - own.y;
			const float along = rx * fx + ry * fy;
			const float side = std::abs(rx * fy - ry * fx);
			if (along >= kConvergeMin && along < best && side <= kConvergeCorridor) {
				best = along;
				bestZ = pos.z + a_other->GetHeight() * kTargetHeight;
			}
		};
		if (auto* lists = RE::ProcessLists::GetSingleton()) {
			lists->ForEachHighActor([&](RE::Actor* a_other) {
				check(a_other);
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		check(RE::PlayerCharacter::GetSingleton());  // not in the high actor list; matters when an NPC uses the Luce
		if (best > kConvergeMax) {
			best = kConvergeDefault;
			bestZ = own.z + a_actor->GetHeight() * kTargetHeight;
		}
		return { own.x + fx * best, own.y + fy * best, bestZ };
	}

	// Launch angles that make the shot's arc pass through a_to: the flatter of the two ballistic solutions for the projectile's speed and
	// gravity; straight at it when out of reach. Missile gravity on 1.7.104: velocity.z -= delta * 686.38 * data.gravity (ID 44197, the
	// constant is ID 210017).
	RE::Projectile::ProjectileRot AimAngles(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
	{
		const float dx = a_to.x - a_from.x;
		const float dy = a_to.y - a_from.y;
		const float dz = a_to.z - a_from.z;
		const float dist = std::hypot(dx, dy);
		const float v = g.projectile->data.speed;
		const float grav = g.projectile->data.gravity * kGravityScale;
		float       pitch = std::atan2(dz, dist);  // up = positive here
		if (dist > 1.0f && grav > 0.0f && v > 0.0f) {
			const float v2 = v * v;
			const float disc = v2 * v2 - grav * (grav * dist * dist + 2.0f * dz * v2);
			if (disc >= 0.0f) {
				pitch = std::atan((v2 - std::sqrt(disc)) / (grav * dist));
			}
		}
		return { -pitch, std::atan2(dx, dy) };  // the engine's angleX is positive downward
	}

	// ===== Spawn circles =====

	// main thread: let the engine remove the circle on its next update
	void EndFlashes(const std::vector<RE::ObjectRefHandle>& a_handles)
	{
		const auto vtable = RE::VTABLE_Explosion[0].address();
		for (const auto& handle : a_handles) {
			const auto ref = handle.get();
			if (!ref || *reinterpret_cast<const std::uintptr_t*>(ref.get()) != vtable) {
				continue;
			}
			auto* const base = reinterpret_cast<std::uint8_t*>(ref.get());
			const float lifetime = *reinterpret_cast<const float*>(base + kExplosionLifetime);
			float&      age = *reinterpret_cast<float*>(base + kExplosionAge);
			age = std::max(age, lifetime + 0.01f);
		}
	}

	// the same from any thread
	void QueueEndFlashes(std::vector<RE::ObjectRefHandle> a_handles)
	{
		if (!a_handles.empty()) {
			SKSE::GetTaskInterface()->AddTask([handles = std::move(a_handles)]() { EndFlashes(handles); });
		}
	}

	// ===== Firing =====

	// one shot: spawn circle + spell projectile from a sphere point; a_freeAim: the convergence point when there is no target
	RE::ObjectRefHandle FireOne(RE::Actor* a_actor, RE::Actor* a_target, const RE::NiPoint3& a_freeAim)
	{
		RE::NiPoint3 aim;
		float        heading = a_actor->GetAngleZ();
		if (a_target) {
			aim = a_target->GetPosition();
			aim.z += a_target->GetHeight() * kTargetHeight;
			const auto own = a_actor->GetPosition();
			heading = std::atan2(aim.x - own.x, aim.y - own.y);
		}

		float      side = 0.0f;
		float      up = 0.0f;
		const auto origin = SpawnPoint(a_actor, heading, side, up);
		if (!a_target) {
			// the spawn sphere (radius kRadius) narrows to kConvergeRadius at the aim point, keeping each shot's side and height
			constexpr float scale = kConvergeRadius / kRadius;
			aim = a_freeAim;
			aim.x += std::cos(heading) * side * scale;
			aim.y -= std::sin(heading) * side * scale;
			aim.z += up * scale;
		}
		const auto angles = AimAngles(origin, aim);
		RE::ObjectRefHandle flashHandle;
		if (auto flash = a_actor->PlaceObjectAtMe(g.explosion, false)) {
			flash->SetPosition(origin);
			flashHandle = flash->GetHandle();
		}

		// Every shot is the spell, with or without a target (weapon + ammo shots passed through actors).
		RE::ProjectileHandle         handle;
		RE::Projectile::LaunchData data(a_actor, origin, angles, g.spell);
		data.desiredTarget = a_target;
		RE::Projectile::Launch(&handle, data);
		return flashHandle;
	}

	// main thread: the shots that came due in one effect update
	void Fire(RE::FormID a_id, int a_count)
	{
		{
			std::scoped_lock lock(g_lock);
			const auto it = g_states.find(a_id);
			if (it == g_states.end() || !it->second.owner) {
				return;  // the effect ended or a save was loaded since these shots came due
			}
		}
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
		if (!actor || actor->IsDead() || !actor->Is3DLoaded() || !actor->GetParentCell()) {
			return;
		}
		auto*                            target = TargetOf(actor);
		const auto                       freeAim = target ? RE::NiPoint3{} : FreeAimPoint(actor);
		std::vector<RE::ObjectRefHandle> flashes;
		for (int i = 0; i < a_count; ++i) {
			if (const auto flash = FireOne(actor, target, freeAim)) {
				flashes.push_back(flash);
			}
		}
		{
			std::scoped_lock lock(g_lock);
			const auto       it = g_states.find(a_id);
			if (it != g_states.end() && it->second.owner) {
				for (const auto& flash : flashes) {
					it->second.flashes.push_back({ flash });
				}
				return;
			}
		}
		EndFlashes(flashes);  // the effect ended while these shots were fired
	}

	// ===== End of the loop clip =====

	// case-insensitive
	bool EndsWith(std::string_view a_text, std::string_view a_suffix)
	{
		return a_text.size() >= a_suffix.size() &&
		       std::ranges::equal(a_text.substr(a_text.size() - a_suffix.size()), a_suffix, [](char a, char b) {
				   return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			   });
	}

	// the moveset's power attack loop (BFCO or MCO name)
	bool IsLoopClip(const RE::hkbClipGenerator* a_clip)
	{
		const char* name = a_clip->animationName.c_str();
		if (!name) {
			return false;
		}
		const std::string_view view(name);
		return EndsWith(view, "\\BFCO_PowerAttackLoop1.hkx") || EndsWith(view, "\\MCO_PowerAttackLoop1.hkx");
	}

	// the behavior character belongs to one of the actor's graphs
	bool UsesCharacter(RE::Actor* a_actor, const RE::hkbCharacter* a_character)
	{
		RE::BSAnimationGraphManagerPtr manager;
		if (!a_actor->GetAnimationGraphManager(manager) || !manager) {
			return false;
		}
		RE::BSSpinLockGuard guard(manager->updateLock);
		return std::ranges::any_of(manager->graphs, [&](const auto& a_graph) {
			return a_graph && &a_graph->characterInstance == a_character;
		});
	}

	// the actor's Luce effects that aren't dispelled yet
	class CollectLuce final : public RE::MagicTarget::ForEachActiveEffectVisitor
	{
	public:
		RE::BSContainer::ForEachResult Accept(RE::ActiveEffect* a_effect) override
		{
			if (IsLuce(a_effect) && !a_effect->flags.any(RE::ActiveEffect::Flag::kDispelled)) {
				found.push_back(a_effect);
			}
			return RE::BSContainer::ForEachResult::kContinue;
		}

		std::vector<RE::ActiveEffect*> found;
	};

	// main thread: dispel the Luce of the firing actor whose loop clip ended; the character pointer is only compared, never read
	void ClipEnded(const RE::hkbCharacter* a_character)
	{
		std::vector<RE::FormID> firing;
		{
			std::scoped_lock lock(g_lock);
			for (const auto& [id, state] : g_states) {
				if (state.owner) {
					firing.push_back(id);
				}
			}
		}
		for (const auto id : firing) {
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!actor || !UsesCharacter(actor, a_character)) {
				continue;
			}
			CollectLuce visitor;
			static_cast<RE::MagicTarget*>(actor)->VisitEffects(visitor);
			REX::INFO("luce: {:08X} loop clip ended, dispelling {} effect(s)", id, visitor.found.size());
			for (auto* effect : visitor.found) {
				effect->Dispel(true);
			}
			return;
		}
	}

	// hkbClipGenerator::Deactivate hook (behavior thread): only queues the check
	struct ClipDeactivate
	{
		static void thunk(RE::hkbClipGenerator* a_this, const RE::hkbContext& a_context)
		{
			if (g.effect && a_context.character && IsLoopClip(a_this)) {
				const RE::hkbCharacter* character = a_context.character;
				SKSE::GetTaskInterface()->AddTask([character]() { ClipEnded(character); });
			}
			func(a_this, a_context);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

namespace Luce
{
	// ===== Public =====

	void Install()
	{
		// slot 7 = Deactivate (1.7.104 ID 59254)
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_hkbClipGenerator[0] };
		ClipDeactivate::func = vtbl.write_vfunc(0x7, ClipDeactivate::thunk);
		REX::INFO("luce: hooked hkbClipGenerator::Deactivate");
	}

	void OnDataLoaded()
	{
		auto* handler = RE::TESDataHandler::GetSingleton();
		g.effect = handler->LookupForm<RE::EffectSetting>(0x84D, kPlugin);
		g.spell = handler->LookupForm<RE::SpellItem>(0x851, kPlugin);
		g.explosion = handler->LookupForm<RE::BGSExplosion>(0x850, kPlugin);
		auto* shot = g.spell ? g.spell->GetAVEffect() : nullptr;
		g.projectile = shot ? shot->data.projectileBase : nullptr;
		if (!g.effect || !g.spell || !g.explosion || !g.projectile) {
			REX::ERROR("luce: forms missing (effect {}, spell {}, explosion {}, projectile {}); the Luce stays off", g.effect != nullptr,
				g.spell != nullptr, g.explosion != nullptr, g.projectile != nullptr);
			g.effect = nullptr;
			return;
		}
		REX::INFO("luce: shot projectile {:08X}, speed {:g}, gravity {:g} (reach {:.0f} units)", g.projectile->GetFormID(), g.projectile->data.speed,
			g.projectile->data.gravity, g.projectile->data.gravity > 0.0f ? g.projectile->data.speed * g.projectile->data.speed / (g.projectile->data.gravity * kGravityScale) : 0.0f);

		auto* sound = handler->LookupForm<RE::BGSSoundDescriptorForm>(kSoundID, kSoundPlugin);
		if (auto* def = sound ? skyrim_cast<RE::BGSStandardSoundDef*>(sound->soundDescriptor) : nullptr) {
			REX::INFO("luce: firing sound {:08X}, priority {} -> {}", sound->GetFormID(), def->soundCharacteristics.priority, kSoundPriority);
			def->soundCharacteristics.priority = kSoundPriority;
		} else {
			REX::INFO("luce: firing sound {:X} in {} not found, priority unchanged", kSoundID, kSoundPlugin);
		}
		g_tdm = static_cast<TDM_API::IVTDM1*>(TDM_API::RequestPluginAPI(TDM_API::InterfaceVersion::V1));
		REX::INFO("luce: ready, TDM target lock {}", g_tdm ? "available" : "NOT available (shots go straight ahead)");
	}

	void Reset()
	{
		std::scoped_lock lock(g_lock);
		g_states.clear();
	}

	void OnEffectStart(RE::ActiveEffect* a_effect)
	{
		if (!IsLuce(a_effect)) {
			return;
		}
		auto* actor = TargetActor(a_effect);
		if (!actor) {
			return;
		}
		const auto id = actor->GetFormID();
		{
			std::scoped_lock lock(g_lock);
			auto&            state = StateOf(id);
			state.owner = a_effect;
			state.ownerSilent = 0.0f;
			state.shots = 0;
		}
		REX::INFO("luce: {:08X} spell added, firing", id);
	}

	void OnEffectFinish(RE::ActiveEffect* a_effect)
	{
		if (!IsLuce(a_effect)) {
			return;
		}
		auto* actor = TargetActor(a_effect);
		if (!actor) {
			return;
		}
		const auto                       id = actor->GetFormID();
		std::uint32_t                    shots = 0;
		std::vector<RE::ObjectRefHandle> flashes;
		{
			std::scoped_lock lock(g_lock);
			const auto       it = g_states.find(id);
			if (it == g_states.end() || it->second.owner != a_effect) {
				return;  // an overlapped older cast: the newer one keeps firing
			}
			auto& state = it->second;
			state.owner = nullptr;
			shots = state.shots;
			for (const auto& flash : state.flashes) {
				flashes.push_back(flash.handle);
			}
			state.flashes.clear();
		}
		QueueEndFlashes(std::move(flashes));
		REX::INFO("luce: {:08X} spell removed, stopped after {} shots", id, shots);
	}

	// ages the spawn circles, counts down the emitters and queues the shots that are due (one owner effect drives the timers)
	void OnEffectUpdate(RE::ActiveEffect* a_effect, float a_delta)
	{
		if (!IsLuce(a_effect) || a_delta <= 0.0f ||
			a_effect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) {
			return;
		}
		auto* actor = TargetActor(a_effect);
		if (!actor) {
			return;
		}
		const auto                       id = actor->GetFormID();
		int                              due = 0;
		std::vector<RE::ObjectRefHandle> expired;
		{
			std::scoped_lock lock(g_lock);
			auto&            state = StateOf(id);
			if (state.owner == a_effect || !state.owner) {
				std::erase_if(state.flashes, [&](Flash& a_flash) {
					a_flash.left -= a_delta;
					if (a_flash.left > 0.0f) {
						return false;
					}
					expired.push_back(a_flash.handle);
					return true;
				});
			}
			if (state.owner != a_effect) {
				if (state.owner) {
					// another cast drives the timers; take over only if it stopped updating
					state.ownerSilent += a_delta;
					if (state.ownerSilent < 0.1f) {
						return;  // the owner's updates age the circles
					}
				}
				state.owner = a_effect;  // also covers an effect that was already running when a save loaded
			}
			state.ownerSilent = 0.0f;
			for (auto& timer : state.timers) {
				timer -= a_delta;
				if (timer <= 0.0f && due < kMaxShotsPerUpdate) {
					timer = RandomFloat(kMinInterval, kMaxInterval);
					++due;
				}
			}
			state.shots += static_cast<std::uint32_t>(due);
		}
		QueueEndFlashes(std::move(expired));
		if (due > 0) {
			SKSE::GetTaskInterface()->AddTask([id, due]() { Fire(id, due); });
		}
	}
}
