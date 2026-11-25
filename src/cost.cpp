// Magicka cost of DT / SDT: the drain while a cost effect is active, the activation cost (ModeCost) and the out-of-magicka end.
// Design notes: cost.h.

#include "cost.h"

#include "transform.h"

#include <mutex>
#include <unordered_map>

namespace
{
	// ===== Forms and settings (set at data load) =====

	RE::EffectSetting* g_costEffect[2]{};  // dsdtcostAMGEF (DT) / dsdtcostBMGEF (SDT)
	Cost::Settings     g_settings[2]{};

	// ===== State =====

	// per cost effect instance
	struct DrainState
	{
		RE::FormID    actor = 0;
		std::uint16_t uniqueID = 0;
		float         elapsed = 0.0f;
		bool          outOfMagicka = false;
		bool          logged = false;
	};

	// effect updates may run on worker threads (several actors at once)
	std::mutex                                       g_lock;
	std::unordered_map<RE::ActiveEffect*, DrainState> g_state;

	// ===== Out of magicka =====

	// main thread: log and end the actor's mode (Transform::OutOfMagicka checks it is still in that mode)
	void SendOutOfMana(RE::FormID a_actor, int a_mode)
	{
		SKSE::GetTaskInterface()->AddTask([a_actor, a_mode]() {
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actor);
			if (!actor) {
				return;
			}
			const float magicka = static_cast<RE::ActorValueOwner*>(actor)->GetActorValue(RE::ActorValue::kMagicka);
			REX::INFO("{} {:08X}: out of magicka in {} ({:.2f} left, cost {:.2f}/s)", actor->GetName(), a_actor, a_mode ? "SDT" : "DT",
				magicka, Cost::ModeCost(actor, a_mode));
			Transform::OutOfMagicka(a_actor, a_mode);
		});
	}
}

namespace Cost
{
	void Configure(int a_mode, RE::EffectSetting* a_costEffect, const Settings& a_settings, bool a_log)
	{
		g_costEffect[a_mode] = a_costEffect;
		g_settings[a_mode] = a_settings;
		if (a_log) {
			REX::INFO("  cost: player {}/s, NPCs {}% of max magicka/s, drain after {} s", a_settings.rate, a_settings.npcPercent,
				a_settings.delay);
		}
	}

	float ModeCost(RE::Actor* a_actor, int a_mode)
	{
		const auto& s = g_settings[a_mode];
		if (a_actor->IsPlayerRef()) {
			return s.rate;
		}
		const float max = static_cast<RE::ActorValueOwner*>(a_actor)->GetPermanentActorValue(RE::ActorValue::kMagicka);
		return std::max(1.0f, max * s.npcPercent / 100.0f);
	}

	// every frame per active script effect: after the delay, pay this frame's share or end the mode
	void OnUpdate(RE::ActiveEffect* a_effect, float a_delta)
	{
		auto* base = a_effect->GetBaseObject();
		if (!base || a_delta <= 0.0f) {
			return;
		}
		const int mode = base == g_costEffect[0] ? 0 : base == g_costEffect[1] ? 1 : -1;
		if (mode < 0 || a_effect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) {
			return;
		}
		auto* ref = a_effect->target ? a_effect->target->GetTargetStatsObject() : nullptr;
		auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
		if (!actor || actor->IsDead()) {
			return;
		}

		const float rate = Cost::ModeCost(actor, mode);
		auto*       av = static_cast<RE::ActorValueOwner*>(actor);
		bool        send = false;
		{
			std::scoped_lock lock(g_lock);
			auto&            state = g_state[a_effect];
			if (state.actor != actor->GetFormID() || state.uniqueID != a_effect->usUniqueID) {
				// new effect (or a reused pointer); after a load the effect's saved elapsed time counts toward the delay
				state = { actor->GetFormID(), a_effect->usUniqueID };
				state.elapsed = (std::max)(a_effect->elapsedSeconds - a_delta, 0.0f);
			}
			if (state.outOfMagicka) {
				return;
			}
			state.elapsed += a_delta;
			if (state.elapsed < g_settings[mode].delay) {
				return;
			}
			if (!state.logged) {
				state.logged = true;
				REX::INFO("{} {:08X}: {} drain starts, {:.2f} magicka/s", actor->GetName(), actor->GetFormID(), mode ? "SDT" : "DT", rate);
			}
			const float pay = rate * a_delta;
			if (av->GetActorValue(RE::ActorValue::kMagicka) >= pay) {
				av->DamageActorValue(RE::ActorValue::kMagicka, pay);
			} else {
				state.outOfMagicka = true;
				send = true;
			}
		}
		if (send) {
			SendOutOfMana(actor->GetFormID(), mode);
		}
	}

	void Forget(RE::FormID a_actor)
	{
		std::scoped_lock lock(g_lock);
		std::erase_if(g_state, [a_actor](const auto& a_entry) { return a_entry.second.actor == a_actor; });
	}

	void Reset()
	{
		std::scoped_lock lock(g_lock);
		g_state.clear();
	}
}
