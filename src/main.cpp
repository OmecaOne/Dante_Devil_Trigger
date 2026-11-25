// DanteDevilTrigger: the SKSE plugin of "Dante's Devil Trigger" (OmecaOneDanteSDT.esp). No Papyrus scripts.
// This file: plugin entry and messaging, and the per-actor mode scan that drives the costume. The other modules:
// - Settings (settings.cpp): the INI, written into the forms; changed live by the menu.
// - Transformations (transform.cpp): triggers → stages, mode end conditions, untransform animation, shout setup on load.
// - Magicka cost (cost.cpp): drain while transformed, activation cost.
// - Visual override (visual.cpp): any actor with an active DT/SDT mode effect gets the costume drawn over its gear.
// - The Luce (luce.cpp): the SDT power attack's sword volley.
// - Settings page (menu.cpp, SKSE Menu Framework, optional).
// Movement speed changes only apply after the next carry weight update (engine quirk), so carry weight is nudged whenever one of
// the mode effects starts or ends on the player.

#include "cost.h"
#include "luce.h"
#include "menu.h"
#include "settings.h"
#include "transform.h"
#include "visual.h"

#include <mutex>
#include <set>
#include <string>

namespace
{
	// ===== Forms =====

	constexpr std::string_view kPlugin = "OmecaOneDanteSDT.esp";
	constexpr RE::FormID       kModeEffects[2] = { 0x802, 0x838 };  // dsdtmodeAMGEF / dsdtmodeBMGEF (also OAR conditions)

	// a form of OmecaOneDanteSDT.esp by its local ID; logs a missing one
	template <class T>
	T* Form(RE::FormID a_local)
	{
		auto* form = RE::TESDataHandler::GetSingleton()->LookupForm<T>(a_local, kPlugin);
		if (!form) {
			REX::ERROR("form {:03X} not found in {}", a_local, kPlugin);
		}
		return form;
	}

	// ===== State =====

	bool g_inMode[2]{};  // the player's last scanned modes (DT, SDT)

	// actors whose effects changed this frame; handled in one task on the main thread
	std::mutex           g_pendingLock;
	std::set<RE::FormID> g_pending;

	// ===== Mode scan =====

	// Mode state from the player's active effect list: an active (not inactive, not dispelled) effect whose base is
	// dsdtmodeAMGEF / dsdtmodeBMGEF. (The engine's HasMagicEffect reported SDT as DT.)
	class ModeVisitor final : public RE::MagicTarget::ForEachActiveEffectVisitor
	{
	public:
		RE::EffectSetting* modes[2]{};
		bool               found[2]{};
		std::string        trace;

		RE::BSContainer::ForEachResult Accept(RE::ActiveEffect* a_effect) override
		{
			auto* base = a_effect ? a_effect->GetBaseObject() : nullptr;
			for (int i = 0; i < 2; ++i) {
				if (base && base == modes[i]) {
					const auto flags = a_effect->flags.underlying();
					const bool live = !a_effect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled);
					found[i] = found[i] || live;
					trace += std::format(" {}:{:08X}{}", i ? "SDT" : "DT", flags, live ? "" : "(off)");
				}
			}
			return RE::BSContainer::ForEachResult::kContinue;
		}
	};

	// the actor's live DT / SDT mode effects
	ModeVisitor ScanModes(RE::Actor* a_actor)
	{
		ModeVisitor v;
		for (int i = 0; i < 2; ++i) {
			v.modes[i] = Form<RE::EffectSetting>(kModeEffects[i]);
		}
		static_cast<RE::MagicTarget*>(a_actor)->VisitEffects(v);
		return v;
	}

	// SDT wins while both are briefly active (DT -> SDT switch)
	Visual::Costume CostumeOf(const ModeVisitor& a_scan)
	{
		return a_scan.found[1] ? Visual::Costume::kSDT : a_scan.found[0] ? Visual::Costume::kDT : Visual::Costume::kNone;
	}

	// ===== After a save load / new game =====

	// settings (globals, CARP check) and the costume of every loaded transformed actor
	void OnGameLoaded()
	{
		Settings::OnGameLoaded();
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		const auto scan = ScanModes(player);
		for (int i = 0; i < 2; ++i) {
			g_inMode[i] = scan.found[i];
		}
		// every loaded actor that is transformed gets its costume (the state isn't saved; NPC modes end on unload)
		Visual::SetCostume(player, CostumeOf(scan));
		RE::ProcessLists::GetSingleton()->ForAllActors([](RE::Actor* a_actor) {
			if (a_actor && !a_actor->IsPlayerRef()) {
				Visual::SetCostume(a_actor, CostumeOf(ScanModes(a_actor)));
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
	}

	// ===== Effect changes =====

	// movement speed changes apply only at the next carry weight update (engine quirk)
	void NudgeCarryWeight(RE::PlayerCharacter* a_player)
	{
		auto* avOwner = static_cast<RE::ActorValueOwner*>(a_player);
		avOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight, 0.1f);
		avOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight, -0.1f);
	}

	// main thread: rescan every actor whose effects changed; costume, forget untransformed actors, player mode log + speed refresh
	void ProcessPending()
	{
		std::set<RE::FormID> pending;
		{
			std::scoped_lock lock(g_pendingLock);
			pending.swap(g_pending);
		}
		for (const auto id : pending) {
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!actor) {
				continue;
			}
			const auto scan = ScanModes(actor);
			Visual::SetCostume(actor, CostumeOf(scan));
			if (CostumeOf(scan) == Visual::Costume::kNone) {
				Cost::Forget(id);
				Transform::Forget(id);
			}
			if (!actor->IsPlayerRef()) {
				continue;
			}
			bool changed = false;
			for (int i = 0; i < 2; ++i) {
				if (scan.found[i] != g_inMode[i]) {
					g_inMode[i] = scan.found[i];
					changed = true;
					REX::INFO("{} {} (mode effects:{})", Settings::Section(i), scan.found[i] ? "started" : "ended",
						scan.trace.empty() ? " none" : scan.trace);
				}
			}
			if (changed) {
				NudgeCarryWeight(RE::PlayerCharacter::GetSingleton());
			}
		}
	}

	// any effect applied / removed on an actor: queue it for ProcessPending (one task per frame)
	class EffectSink final : public RE::BSTEventSink<RE::TESActiveEffectApplyRemoveEvent>
	{
	public:
		static EffectSink* GetSingleton()
		{
			static EffectSink sink;
			return &sink;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESActiveEffectApplyRemoveEvent* a_event,
			RE::BSTEventSource<RE::TESActiveEffectApplyRemoveEvent>*) override
		{
			auto* actor = a_event && a_event->target ? a_event->target->As<RE::Actor>() : nullptr;
			if (!actor) {
				return RE::BSEventNotifyControl::kContinue;
			}
			bool first = false;
			{
				std::scoped_lock lock(g_pendingLock);
				first = g_pending.empty();
				g_pending.insert(actor->GetFormID());
			}
			if (first) {
				SKSE::GetTaskInterface()->AddTask(ProcessPending);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// ===== Plugin =====

	// data loaded: INI + forms + modules; before a load: reset; after a load / new game: per-save setup
	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			if (!RE::TESDataHandler::GetSingleton()->LookupModByName(kPlugin)) {
				REX::ERROR("{} is not loaded; nothing to do", kPlugin);
				return;
			}
			Settings::Load();
			Visual::OnDataLoaded();
			Transform::OnDataLoaded();
			Luce::OnDataLoaded();
			Menu::Register();
			RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESActiveEffectApplyRemoveEvent>(EffectSink::GetSingleton());
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			Visual::Reset();
			Cost::Reset();
			Transform::Reset();
			Luce::Reset();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			if (a_msg->type == SKSE::MessagingInterface::kNewGame) {
				Visual::Reset();
				Cost::Reset();
				Transform::Reset();
				Luce::Reset();
			}
			OnGameLoaded();
			Transform::OnGameLoaded();
			break;
		default:
			break;
		}
	}
}

// hooks go in at plugin load (trampoline: the two visual.cpp call hooks)
SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, { .trampoline = true, .trampolineSize = 64 });
	Visual::Install();
	Transform::Install();
	Luce::Install();
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
