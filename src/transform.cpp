// Transformations: triggers (OAR clip markers, trigger spells, tap / hold of shout C), stages DT / SDT / off, mode end conditions,
// untransform animation, and the load-time setup of the player's shouts. Design notes: transform.h.

#include "transform.h"

#include "cost.h"
#include "luce.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
	// ===== Forms =====

	constexpr std::string_view kPlugin = "OmecaOneDanteSDT.esp";
	constexpr std::string_view kJCE = "Judgement Cut End - ap05's Remake.esp";
	constexpr std::string_view kLegacyArmorPlugin = "wp DMC.esp";  // the costume plugin of versions up to 1.0.x (merged into ours since)

	// every form the handler uses, resolved once at data load (Transform::OnDataLoaded)
	struct Forms
	{
		RE::EffectSetting* trigger[3]{};      // dsdtrigger0/1/2MGEF
		RE::EffectSetting* mode[2]{};         // dsdtmodeAMGEF / dsdtmodeBMGEF
		RE::SpellItem*     modeSpell[2]{};    // dsdtmodeASP / dsdtmodeBSP
		RE::TESGlobal*     duration[2]{};     // dtmodeduration / sdtmodeduration
		RE::EffectSetting* clip[4]{};         // clip markers: dsdtclipDTon / dsdtclipDToff / dsdtintro (SDT on) / dsdtclipSDToff
		RE::SpellItem*     vfx1 = nullptr;    // dsdtvfx1SP (JCE ready)
		RE::SpellItem*     vfx2 = nullptr;    // dsdtvfx2SP (JCE activation)
		RE::SpellItem*     vfx3 = nullptr;    // dsdtvfx3SP (JCE end spark)
		RE::SpellItem*     vfx4 = nullptr;    // dsdtvfx4SP
		RE::SpellItem*     vfxExp = nullptr;  // dsdtvfxexpSP
		RE::EffectSetting* intro = nullptr;   // dsdtintroMGEF: the SDT transformation clip is playing
		RE::SpellItem*     introCancel = nullptr;     // dsdtcancelSP: cut it
		RE::EffectSetting* cancelEffect[2]{};          // dsdtcancelDTMGEF / dsdtcancelMGEF
		RE::SpellItem*     cancelSpell[2]{};           // dsdtcancelDTSP / dsdtcancelSP
		RE::TESIdleForm*   combatIdle = nullptr;       // Skyrim.esm CombatIdleABC
		RE::BGSReferenceEffect* transformVFX = nullptr;  // dsdtransformVFX
		RE::TESForm* dtOn = nullptr;       // dtONsfx
		RE::TESForm* dtOnVoice = nullptr;  // dtONvoicesfx
		RE::TESForm* dtOff = nullptr;      // dtOFFsfx
		RE::TESForm* sdtOn = nullptr;      // sdtONsfx
		RE::TESForm* outOfMana = nullptr;  // outofmanasfx
		RE::TESForm* flameOut = nullptr;   // JCE_outflame (copy in our ESP)
		RE::TESForm* uiActi = nullptr;     // JCE_UIacti
		RE::TESForm* uiNote = nullptr;     // JCE_UInote
		RE::TESBoundObject* sdtExplosion = nullptr;  // JCE010Intro_expl3
		RE::TESBoundObject* legacyArmor[2]{};        // dsdtarmorA_wings_closed / dsdtarmorB_wings_open (the old version equipped them)
		RE::BGSListForm*    legacyOutfit = nullptr;  // equippedoutfit: the old version stored the worn armor here (slots 30-60, script-added)
		RE::TESShout*       shoutA = nullptr;        // dsdtshoutA (DT): taken off the player
		RE::TESShout*       shoutB = nullptr;        // dsdtshoutB (SDT): taken off the player
		RE::TESWordOfPower* words[3]{};              // dsdtword1..3: shout C's words
		RE::TESShout*       shoutC = nullptr;        // dsdtshoutC: tap = DT toggle, hold = SDT toggle
		RE::EffectSetting*  hold = nullptr;          // dsdtholdMGEF: shout C held past its second word (OAR picks SDT by it)
		RE::SpellItem*      holdSpell = nullptr;     // dsdtholdSP
	} g;

	bool g_ready = false;  // all required forms found; every hook and sink does nothing until then

	// a form by its local ID; a missing one is logged and clears a_ok.
	// LookupForm<T> casts by form type, which generic classes (TESForm, TESBoundObject) don't have: those are cast by hand
	template <class T>
	T* Lookup(RE::FormID a_local, std::string_view a_file, bool& a_ok)
	{
		auto* raw = RE::TESDataHandler::GetSingleton()->LookupForm(a_local, a_file);
		T*    form = nullptr;
		if constexpr (std::is_same_v<T, RE::TESForm>) {
			form = raw;
		} else if constexpr (std::is_same_v<T, RE::TESBoundObject>) {
			form = raw && raw->IsBoundObject() ? static_cast<RE::TESBoundObject*>(raw) : nullptr;
		} else {
			form = raw ? raw->As<T>() : nullptr;
		}
		if (!form) {
			REX::ERROR("transform: form {:03X} not found in {}", a_local, a_file);
			a_ok = false;
		}
		return form;
	}

	// ===== Helpers =====

	// bumped by Reset (load / new game): a delayed call queued before a load is dropped
	std::atomic<std::uint32_t> g_epoch{ 0 };

	// runs a_fn on the main thread after a_ms milliseconds
	void Later(int a_ms, std::function<void()> a_fn)
	{
		std::thread([a_ms, epoch = g_epoch.load(), fn = std::move(a_fn)]() mutable {
			std::this_thread::sleep_for(std::chrono::milliseconds(a_ms));
			SKSE::GetTaskInterface()->AddTask([epoch, fn = std::move(fn)]() {
				if (epoch == g_epoch.load()) {
					fn();
				}
			});
		}).detach();
	}

	// Has: the actor has a live (not inactive, not dispelled) effect of this base
	class FindEffect final : public RE::MagicTarget::ForEachActiveEffectVisitor
	{
	public:
		explicit FindEffect(const RE::EffectSetting* a_base) :
			base(a_base) {}

		RE::BSContainer::ForEachResult Accept(RE::ActiveEffect* a_effect) override
		{
			if (a_effect && a_effect->GetBaseObject() == base &&
				!a_effect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) {
				found = true;
				return RE::BSContainer::ForEachResult::kStop;
			}
			return RE::BSContainer::ForEachResult::kContinue;
		}

		const RE::EffectSetting* base;
		bool                     found = false;
	};

	bool Has(RE::Actor* a_actor, const RE::EffectSetting* a_effect)
	{
		if (!a_actor || !a_effect) {
			return false;
		}
		FindEffect v(a_effect);
		static_cast<RE::MagicTarget*>(a_actor)->VisitEffects(v);
		return v.found;
	}

	// DispelAll: dispel every effect of this base on the actor (marker effects)
	class StripEffects final : public RE::MagicTarget::ForEachActiveEffectVisitor
	{
	public:
		explicit StripEffects(const RE::EffectSetting* a_base) :
			base(a_base) {}

		RE::BSContainer::ForEachResult Accept(RE::ActiveEffect* a_effect) override
		{
			if (a_effect && a_effect->GetBaseObject() == base && !a_effect->flags.any(RE::ActiveEffect::Flag::kDispelled)) {
				found.push_back(a_effect);
			}
			return RE::BSContainer::ForEachResult::kContinue;
		}

		const RE::EffectSetting*        base;
		std::vector<RE::ActiveEffect*> found;
	};

	void DispelAll(RE::Actor* a_actor, const RE::EffectSetting* a_base)
	{
		if (!a_actor || !a_base) {
			return;
		}
		StripEffects v(a_base);
		static_cast<RE::MagicTarget*>(a_actor)->VisitEffects(v);
		for (auto* effect : v.found) {
			effect->Dispel(true);
		}
	}

	// the actor an active effect is on (nullptr for objects)
	RE::Actor* TargetActor(RE::ActiveEffect* a_effect)
	{
		auto* ref = a_effect->target ? a_effect->target->GetTargetStatsObject() : nullptr;
		return ref ? ref->As<RE::Actor>() : nullptr;
	}

	// current mode: 1 = SDT, 0 = DT, -1 = none
	int ModeOf(RE::Actor* a_actor) { return Has(a_actor, g.mode[1]) ? 1 : Has(a_actor, g.mode[0]) ? 0 : -1; }

	// the actor casts a_spell on itself at once
	void Cast(RE::Actor* a_actor, RE::SpellItem* a_spell)
	{
		if (auto* caster = a_spell ? a_actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant) : nullptr) {
			caster->CastSpellImmediate(a_spell, false, a_actor, 1.0f, false, 0.0f, a_actor);
		}
	}

	// plays a sound (SOUN or SNDR form) at the actor, following its 3D
	void PlaySound(RE::TESForm* a_sound, RE::Actor* a_actor)
	{
		RE::BSISoundDescriptor* descriptor = nullptr;
		if (auto* sound = a_sound ? a_sound->As<RE::TESSound>() : nullptr) {
			descriptor = sound->descriptor;
		} else if (auto* form = a_sound ? a_sound->As<RE::BGSSoundDescriptorForm>() : nullptr) {
			descriptor = form;
		}
		auto* audio = RE::BSAudioManager::GetSingleton();
		if (!descriptor || !audio) {
			return;
		}
		RE::BSSoundHandle handle;
		audio->GetSoundHandle(handle, descriptor);
		if (!handle.IsValid()) {
			return;
		}
		handle.SetPosition(a_actor->GetPosition());
		if (auto* node = a_actor->Get3D()) {
			handle.SetObjectToFollow(node);
		}
		handle.Play();
	}

	// current / maximum magicka (0 when the maximum is 0)
	float MagickaFraction(RE::Actor* a_actor)
	{
		auto*       av = static_cast<RE::ActorValueOwner*>(a_actor);
		const float max = av->GetPermanentActorValue(RE::ActorValue::kMagicka);
		return max > 0.0f ? av->GetActorValue(RE::ActorValue::kMagicka) / max : 0.0f;
	}

	const char* Name(RE::Actor* a_actor) { return a_actor->GetName(); }

	// ===== Papyrus calls =====

	// Papyrus calls for the few things CommonLib has no direct function for (Debug.Notification, Actor.PlayIdle, Game.TeachWord/UnlockWord,
	// Actor.AddShout/RemoveShout, ObjectReference.RemoveItem); they run on the VM
	RE::BSScript::Internal::VirtualMachine* VM() { return RE::BSScript::Internal::VirtualMachine::GetSingleton(); }

	// a Papyrus call's bool result, handed to a_fn on the main thread
	class BoolResult final : public RE::BSScript::IStackCallbackFunctor
	{
	public:
		explicit BoolResult(std::function<void(bool)> a_fn) :
			fn(std::move(a_fn)) {}

		void operator()(RE::BSScript::Variable a_result) override
		{
			const bool ok = a_result.IsBool() && a_result.GetBool();
			SKSE::GetTaskInterface()->AddTask([f = fn, ok]() { f(ok); });
		}
		void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

	private:
		std::function<void(bool)> fn;
	};

	// a_form.a_fn(a_args...) of Papyrus class a_class on the VM; a_callback gets the result
	template <class... Args>
	bool CallMethodCB(RE::TESForm* a_form, const char* a_class, const char* a_fn,
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> a_callback, Args&&... a_args)
	{
		auto* vm = VM();
		if (!vm || !a_form) {
			return false;
		}
		const auto handle = vm->GetObjectHandlePolicy()->GetHandleForObject(a_form->GetFormType(), a_form);
		auto*      args = RE::MakeFunctionArguments(std::forward<Args>(a_args)...);
		if (!vm->DispatchMethodCall(handle, a_class, a_fn, args, a_callback)) {
			REX::WARN("transform: {}.{} could not be dispatched", a_class, a_fn);
			return false;
		}
		return true;
	}

	// the same without a result
	template <class... Args>
	void CallMethod(RE::TESForm* a_form, const char* a_class, const char* a_fn, Args&&... a_args)
	{
		CallMethodCB(a_form, a_class, a_fn, {}, std::forward<Args>(a_args)...);
	}

	// Debug.Notification (top-left message)
	void Notify(const char* a_text)
	{
		auto* vm = VM();
		if (!vm) {
			return;
		}
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
		auto* args = RE::MakeFunctionArguments(RE::BSFixedString(a_text));
		vm->DispatchStaticCall("Debug", "Notification", args, callback);
	}

	// ===== Transformation VFX and SDT size =====

	// VisualEffect.Play(actor) done natively: a base form has no bound script object, so a VM dispatch fails.
	// dsdtransformVFX = art object only (no shader, no flags).
	// The nif runs Magic\BeginLoopEnd.hkx: mBegin 0.7 s, mLoop (looping, nothing visible), mEnd 0.833 s, played when the effect ends.
	// mBegin and mEnd are both a flash (crimson light rays, white glow billboard, heat-shimmer refraction plane); mEnd's is sharper (peak
	// at 33 ms) and is "the pulse". Ending the effect almost at once starts mEnd right away (if the graph lets it cut mBegin short;
	// otherwise it comes after mBegin, at 0.7 s). The nif's own sound is disabled.
	// In game: 3 s pulsed after the clip; no limit never pulsed and left the effect on; 0.7 s pulsed ~0.7 s in; 0.1 s showed no pulse.
	constexpr float kTransformVFXSeconds = 0.3f;

	// the transformation flash (dsdtransformVFX) on the actor
	void PlayVFX(RE::Actor* a_actor)
	{
		if (auto* art = g.transformVFX ? g.transformVFX->data.artObject : nullptr) {
			a_actor->ApplyArtObject(art, kTransformVFXSeconds);
		} else if (g.transformVFX && g.transformVFX->data.effectShader) {
			a_actor->ApplyEffectShader(g.transformVFX->data.effectShader, kTransformVFXSeconds);
		}
	}

	// SDT makes the actor larger by the SDT size factor (INI [SinDevilTrigger] sdtscale, menu slider; default 1.1); relative, so a
	// race/mod scale other than 1 survives. Each actor keeps the factor it got, so a slider change during SDT still undoes
	// exactly that factor. After a save load during SDT the factor isn't known (no co-save yet): the current setting is used.
	std::atomic<float>                    g_sdtScale{ 1.1f };
	std::unordered_map<RE::FormID, float> g_appliedScale;  // main thread

	// refScale is in percent; rounded so that grow + shrink gives the old scale back
	void ScaleBy(RE::Actor* a_actor, float a_factor)
	{
		const float percent = std::round(static_cast<float>(a_actor->refScale) * a_factor);
		a_actor->SetScale(percent / 100.0f);
	}

	// entering SDT: grow by the current factor and remember it
	void GrowSDT(RE::Actor* a_actor)
	{
		const float factor = g_sdtScale.load();
		g_appliedScale[a_actor->GetFormID()] = factor;
		ScaleBy(a_actor, factor);
	}

	// leaving SDT: undo the factor this actor got
	void ShrinkSDT(RE::Actor* a_actor)
	{
		float      factor = g_sdtScale.load();
		const auto it = g_appliedScale.find(a_actor->GetFormID());
		if (it != g_appliedScale.end()) {
			factor = it->second;
			g_appliedScale.erase(it);
		}
		ScaleBy(a_actor, 1.0f / factor);
	}

	// ===== Per-actor state (main thread) =====

	std::unordered_map<RE::FormID, std::uint32_t> g_generation;  // bumped by every stage; a delayed part runs only if unchanged
	std::set<RE::FormID>                          g_ending;      // an end is under way (no second one until the mode is gone)

	std::uint32_t Bump(RE::FormID a_id) { return ++g_generation[a_id]; }

	// ===== Untransform animation =====

	// inside the SDT transformation clip (intro marker on): cut it short; true when it did
	bool CutIntro(RE::Actor* a_actor)
	{
		if (a_actor->IsDead() || !Has(a_actor, g.intro)) {
			return false;
		}
		Cast(a_actor, g.introCancel);
		REX::INFO("{} {:08X}: transformation clip cut", Name(a_actor), a_actor->GetFormID());
		return true;
	}

	constexpr int kMarkerWaits = 10;  // × 50 ms: the OAR CANCEL section needs the marker on when the idle starts
	constexpr int kIdleTries = 8;     // Actor.PlayIdle refused (busy actor) → 100 ms later, marker re-cast if it ran out, wait, try again

	void WaitForCancelMarker(RE::FormID a_id, int a_mode, int a_waits, int a_tries);

	// Actor.PlayIdle(CombatIdleABC); a refusal retries up to kIdleTries times
	void TryUntransformIdle(RE::FormID a_id, int a_mode, int a_tries)
	{
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
		if (!actor || actor->IsDead() || actor->IsBleedingOut() || ModeOf(actor) >= 0) {
			return;  // gone, or transformed again in the meantime
		}
		auto callback = RE::make_smart<BoolResult>([a_id, a_mode, a_tries](bool a_ok) {
			auto* a = RE::TESForm::LookupByID<RE::Actor>(a_id);
			if (!a) {
				return;
			}
			const int attempt = kIdleTries - a_tries + 1;
			if (a_ok) {
				REX::INFO("{} {:08X}: untransform animation started (PlayIdle try {})", Name(a), a_id, attempt);
			} else if (a_tries > 1) {
				REX::INFO("{} {:08X}: untransform animation refused (PlayIdle try {}), retrying", Name(a), a_id, attempt);
				Later(100, [a_id, a_mode, a_tries]() {
					if (auto* b = RE::TESForm::LookupByID<RE::Actor>(a_id); b && !b->IsDead() && !Has(b, g.cancelEffect[a_mode])) {
						Cast(b, g.cancelSpell[a_mode]);
					}
					WaitForCancelMarker(a_id, a_mode, kMarkerWaits, a_tries - 1);
				});
			} else {
				REX::WARN("{} {:08X}: untransform animation refused {} times, given up", Name(a), a_id, kIdleTries);
			}
		});
		CallMethodCB(actor, "Actor", "PlayIdle", callback, static_cast<RE::TESIdleForm*>(g.combatIdle));
	}

	// polls every 50 ms until the cancel marker is on (at most kMarkerWaits times), then plays the idle
	void WaitForCancelMarker(RE::FormID a_id, int a_mode, int a_waits, int a_tries)
	{
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
		if (!actor || actor->IsDead()) {
			return;
		}
		if (!Has(actor, g.cancelEffect[a_mode])) {
			if (a_waits > 0) {
				Later(50, [a_id, a_mode, a_waits, a_tries]() { WaitForCancelMarker(a_id, a_mode, a_waits - 1, a_tries); });
				return;
			}
			REX::WARN("{} {:08X}: cancel marker not on after {} ms; idle played anyway", Name(actor), a_id, kMarkerWaits * 50);
		} else {
			REX::INFO("{} {:08X}: cancel marker on after {} ms", Name(actor), a_id, (kMarkerWaits - a_waits) * 50);
		}
		TryUntransformIdle(a_id, a_mode, a_tries);
	}

	// the untransform animation: cut the SDT intro clip if it plays, else cancel marker + CombatIdleABC (OAR "...Trigger CANCEL")
	void PlayUntransform(RE::Actor* a_actor, int a_mode)
	{
		if (a_actor->IsDead() || a_actor->IsBleedingOut()) {
			return;
		}
		if (a_mode == 1 && CutIntro(a_actor)) {
			return;
		}
		Cast(a_actor, g.cancelSpell[a_mode]);
		WaitForCancelMarker(a_actor->GetFormID(), a_mode, kMarkerWaits, kIdleTries);
	}

	// ===== Stages =====

	// activation cost; on refusal: sound, SDT intro clip cut, notification for the player
	bool PayActivation(RE::Actor* a_actor, int a_mode)
	{
		auto*       av = static_cast<RE::ActorValueOwner*>(a_actor);
		const float cost = Cost::ModeCost(a_actor, a_mode);
		if (av->GetActorValue(RE::ActorValue::kMagicka) >= cost) {
			av->DamageActorValue(RE::ActorValue::kMagicka, cost);
			return true;
		}
		PlaySound(g.uiNote, a_actor);
		CutIntro(a_actor);
		if (a_actor->IsPlayerRef()) {
			Notify(a_mode ? "Sin Devil Trigger: Not enough Magicka" : "Devil Trigger: Not enough Magicka");
		}
		REX::INFO("{} {:08X}: {} refused, {:.1f} magicka < cost {:.1f}", Name(a_actor), a_actor->GetFormID(), a_mode ? "SDT" : "DT",
			av->GetActorValue(RE::ActorValue::kMagicka), cost);
		return false;
	}

	// stage 0 = untransform. a_vfx4: also cast the yamato flame VFX (dsdtvfx4SP). Its effect has no sound (its On Hit sound doubled
	// the untransform sound below; a CastSpellImmediate spell plays only On Hit, not Release)
	void Stage0(RE::Actor* a_actor, bool a_vfx4 = false)
	{
		const auto t0 = std::chrono::steady_clock::now();
		Bump(a_actor->GetFormID());
		const bool sdt = Has(a_actor, g.mode[1]);
		const bool dt = Has(a_actor, g.mode[0]);
		if (a_vfx4) {
			Cast(a_actor, g.vfx4);
		}
		if (sdt) {
			PlaySound(g.flameOut, a_actor);
		} else {
			PlayVFX(a_actor);
			PlaySound(g.dtOff, a_actor);
		}
		a_actor->RemoveSpell(g.modeSpell[0]);
		a_actor->RemoveSpell(g.modeSpell[1]);
		if (sdt) {
			ShrinkSDT(a_actor);
		}
		if (MagickaFraction(a_actor) < 0.1f) {
			PlaySound(g.outOfMana, a_actor);
		}
		const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		REX::INFO("{} {:08X}: untransform (was {}), {:.1f} ms", Name(a_actor), a_actor->GetFormID(), sdt ? "SDT" : dt ? "DT" : "nothing", ms);
	}

	// DT (also from SDT)
	void Stage1(RE::Actor* a_actor)
	{
		Bump(a_actor->GetFormID());
		if (!PayActivation(a_actor, 0)) {
			return;
		}
		g_ending.erase(a_actor->GetFormID());
		const bool sdt = Has(a_actor, g.mode[1]);
		PlayVFX(a_actor);
		PlaySound(g.dtOn, a_actor);
		// add the new mode before removing the old one: the costume never drops for a frame
		a_actor->AddSpell(g.modeSpell[0]);
		a_actor->RemoveSpell(g.modeSpell[1]);
		if (sdt) {
			ShrinkSDT(a_actor);
		}
		PlaySound(g.dtOnVoice, a_actor);
		REX::INFO("{} {:08X}: DT{}", Name(a_actor), a_actor->GetFormID(), sdt ? " (from SDT)" : "");
	}

	// SDT: the ability, explosion and growth come 0.4 s after the sound
	void Stage2(RE::Actor* a_actor)
	{
		const auto id = a_actor->GetFormID();
		const auto generation = Bump(id);
		if (!PayActivation(a_actor, 1)) {
			return;
		}
		g_ending.erase(id);
		PlaySound(g.sdtOn, a_actor);
		Later(400, [id, generation]() {
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!actor || actor->IsDead() || g_generation[id] != generation) {
				return;  // another stage ran in the meantime
			}
			PlaySound(g.uiActi, actor);
			if (g.sdtExplosion) {
				actor->PlaceObjectAtMe(g.sdtExplosion, false);
			}
			const bool sdt = Has(actor, g.mode[1]);
			actor->AddSpell(g.modeSpell[1]);
			actor->RemoveSpell(g.modeSpell[0]);
			if (!sdt) {
				GrowSDT(actor);
			}
			REX::INFO("{} {:08X}: SDT", Name(actor), id);
		});
	}

	// ===== Lifecycle: mode end conditions =====

	// the game ended the mode: VFX + untransform (+ the untransform animation)
	void EndMode(RE::Actor* a_actor, bool a_animate, const char* a_why)
	{
		const auto id = a_actor->GetFormID();
		const int  mode = ModeOf(a_actor);
		if (mode < 0 || g_ending.contains(id)) {
			return;
		}
		g_ending.insert(id);
		REX::INFO("{} {:08X}: {} ends ({})", Name(a_actor), id, mode ? "SDT" : "DT", a_why);
		Cast(a_actor, g.vfxExp);
		Stage0(a_actor, true);
		if (a_animate) {
			PlayUntransform(a_actor, mode);
		}
	}

	// dying / unloaded NPC: just remove the abilities
	void RemoveModes(RE::Actor* a_actor)
	{
		Bump(a_actor->GetFormID());
		const bool sdt = Has(a_actor, g.mode[1]);
		a_actor->RemoveSpell(g.modeSpell[0]);
		a_actor->RemoveSpell(g.modeSpell[1]);
		if (sdt) {
			ShrinkSDT(a_actor);
		}
	}

	// per mode effect instance (effect updates run on worker threads, so behind g_lock)
	struct ModeState
	{
		RE::FormID    actor = 0;
		std::uint16_t uniqueID = 0;
		float         elapsed = 0.0f;
		float         outOfCombat = 0.0f;
		bool          done = false;
	};
	std::mutex                                       g_lock;
	std::unordered_map<RE::ActiveEffect*, ModeState> g_modeState;

	constexpr float kNpcOutOfCombatEnd = 1.0f;  // seconds out of combat before an NPC's mode ends

	// effect update thread: decide, act on the main thread
	void ModeUpdate(RE::ActiveEffect* a_effect, float a_delta)
	{
		auto* base = a_effect->GetBaseObject();
		const int mode = base == g.mode[0] ? 0 : base == g.mode[1] ? 1 : -1;
		if (mode < 0 || a_delta <= 0.0f || a_effect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) {
			return;
		}
		auto* actor = TargetActor(a_effect);
		if (!actor || actor->IsDead()) {
			return;
		}
		const bool  npc = !actor->IsPlayerRef();
		const float duration = g.duration[mode] ? g.duration[mode]->value : 0.0f;
		bool        animate = false;
		const char* why = nullptr;
		{
			std::scoped_lock lock(g_lock);
			auto&            s = g_modeState[a_effect];
			if (s.actor != actor->GetFormID() || s.uniqueID != a_effect->usUniqueID) {
				// the effect's own elapsed time is in the save: a loaded mode keeps counting instead of starting over
				s = { actor->GetFormID(), a_effect->usUniqueID, (std::max)(a_effect->elapsedSeconds - a_delta, 0.0f) };
			}
			if (s.done) {
				return;
			}
			s.elapsed += a_delta;
			if (duration > 0.0f && s.elapsed >= duration) {
				why = "duration over";
				animate = true;
			} else if (npc && actor->IsBleedingOut()) {
				why = "bleedout";
			} else if (npc) {
				s.outOfCombat = actor->IsInCombat() ? 0.0f : s.outOfCombat + a_delta;
				if (s.outOfCombat >= kNpcOutOfCombatEnd) {
					why = "combat over";
					animate = true;
				}
			}
			if (!why) {
				return;
			}
			s.done = true;
		}
		const auto id = actor->GetFormID();
		SKSE::GetTaskInterface()->AddTask([id, animate, why]() {
			if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id); a && !a->IsDead()) {
				EndMode(a, animate, why);
			}
		});
	}

	// ===== Tap / hold shout =====
	//
	// Shout C has three words. While the player holds the shout key with shout C equipped, the hold marker is cast once the key
	// has been held for sdtholdtime (INI, default 0.25 s); the OAR sections then pick SDT (held) or DT (tapped) when the exhale clip
	// starts. The engine releases on its own at the last word (fShoutTime2, 0.45 s), after the marker. One transformation per press:
	// ShoutClipAllowed.
	// A new press clears an old marker; the clip's end clears it too (ClipEnded); its 5 s duration is the fallback.

	// one transformation per key press: holding the key past the last word made the engine release the shout and,
	// with 0 s recovery, start the next one while the key was still down (base -> SDT -> DT -> ... cycling; SDT followed by DT).
	std::atomic<bool>          g_keyDown{ false };   // the player holds the shout key (shout C equipped)
	std::atomic<std::uint32_t> g_press{ 0 };         // bumped on every press
	std::atomic<std::uint32_t> g_pressFired{ UINT32_MAX };  // the press that already started a transformation clip
	// The player's ShoutHandler gets no "held" events of the shout button until it is released (ShoutHandlerCanProcess), so the
	// engine can't start the next shout. (A long voice recovery while the key is down instead cut both SDT clips ~0.3 s in.)
	std::atomic<bool>          g_blocking{ false };
	float                      g_holdSeconds = 0.25f;

	// main thread: a transformation clip of the player's shout C started; false = a second shout of the same key press (ignored)
	bool ShoutClipAllowed(RE::Actor* a_actor, const char* a_clipName)
	{
		if (!a_actor->IsPlayerRef() || a_actor->selectedPower != g.shoutC) {
			return true;
		}
		const auto press = g_press.load();
		if (g_pressFired.load() == press) {
			REX::INFO("hold shout: {} clip is a second shout of the same key press; ignored", a_clipName);
			return false;
		}
		g_pressFired = press;
		if (g_keyDown) {
			g_blocking = true;
			REX::INFO("hold shout: key still down at the {} clip; shout input blocked until it is released", a_clipName);
		}
		return true;
	}

	// seconds held for SDT (INI sdtholdtime); the first call logs the engine's own shout thresholds for comparison
	float HoldSeconds()
	{
		static const float seconds = []() {
			auto* setting = RE::GameSettingCollection::GetSingleton()->GetSetting("fShoutTime1");
			auto* setting2 = RE::GameSettingCollection::GetSingleton()->GetSetting("fShoutTime2");
			const float t1 = setting ? setting->GetFloat() : -1.0f;
			const float t2 = setting2 ? setting2->GetFloat() : -1.0f;
			// fShoutTime1 (0.1 s on 1.7.104) is shorter than a quick tap (0.04-0.10 s measured): own threshold (INI sdtholdtime)
			REX::INFO("transform: hold shout: fShoutTime1 {:.3f}, fShoutTime2 {:.3f}; SDT after {:.3f} s held (INI sdtholdtime)", t1, t2,
				g_holdSeconds);
			return g_holdSeconds;
		}();
		return seconds;
	}

	// shout button with shout C equipped: press = new press (old hold marker dispelled), held long enough = hold marker, release
	class InputSink final : public RE::BSTEventSink<RE::InputEvent*>
	{
	public:
		static InputSink* GetSingleton()
		{
			static InputSink sink;
			return &sink;
		}

		RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
		{
			if (!g_ready || !a_event) {
				return RE::BSEventNotifyControl::kContinue;
			}
			const auto& shoutEvent = RE::UserEvents::GetSingleton()->shout;
			for (auto* e = *a_event; e; e = e->next) {
				auto* button = e->AsButtonEvent();
				if (!button || button->QUserEvent() != shoutEvent) {
					continue;
				}
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!player || player->selectedPower != g.shoutC) {
					continue;
				}
				const float held = button->HeldDuration();
				if (button->IsDown()) {
					marked = false;
					++g_press;
					g_keyDown = true;
					g_blocking = false;  // a new press: the old one was released (even if its up event went to a menu)
					SKSE::GetTaskInterface()->AddTask([]() { DispelAll(RE::PlayerCharacter::GetSingleton(), g.hold); });
				} else if (button->IsHeld() && !marked && held >= HoldSeconds()) {
					marked = true;
					REX::INFO("hold shout: held {:.2f} s -> SDT marker", held);
					SKSE::GetTaskInterface()->AddTask([]() {
						if (auto* p = RE::PlayerCharacter::GetSingleton()) {
							Cast(p, g.holdSpell);
						}
					});
				} else if (button->IsUp()) {
					g_keyDown = false;
					REX::INFO("hold shout: released after {:.2f} s ({})", held, marked ? "SDT" : "DT");
					g_blocking = false;
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}

	private:
		bool marked = false;  // input thread only
	};

	// ===== Clips (trigger) =====
	//
	// The OAR sections cast a marker when their clip starts and dispel it when it ends. The stage (and the VFX that the clips'
	// PIE payloads used to cast) runs at the clip time where the payload annotation was, or at once when the clip ends earlier
	// (stagger, dodge, ...), so an interrupted clip still transforms.

	enum Clip : int
	{
		kDTOn,
		kDTOff,
		kSDTOn,
		kSDTOff
	};
	constexpr const char* kClipName[4] = { "DT on", "DT off", "SDT on", "SDT off" };
	constexpr int         kPayloadMs[4] = { 167, 33, 1900, 17 };  // the old PIE.$Dante... annotation times

	// the actor's current transformation clip; token tells a restarted clip from the old one, fired = its payload ran
	struct ClipState
	{
		int           clip = -1;
		std::uint32_t token = 0;
		bool          fired = false;
	};
	std::unordered_map<RE::FormID, ClipState> g_clips;  // main thread
	std::uint32_t                             g_clipToken = 0;

	// a VFX spell a_ms later (+ camera shake), like the payload's delayed lines
	void CastLater(RE::FormID a_actor, int a_ms, RE::SpellItem* a_spell, float a_shake = 0.0f)
	{
		Later(a_ms, [a_actor, a_spell, a_shake]() {
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actor);
			if (!actor || actor->IsDead()) {
				return;
			}
			Cast(actor, a_spell);
			if (a_shake > 0.0f) {
				RE::ShakeCamera(a_shake, actor->GetPosition(), 0.5f);
			}
		});
	}

	// the payloads of the old Payload Interpreter config (DanteDevilTrigger.ini), with the trigger spell replaced by the stage;
	// on/off is chosen like the trigger spells' conditions did (an ON clip while already in that mode turns it off)
	void RunPayload(RE::Actor* a_actor, int a_clip)
	{
		const auto id = a_actor->GetFormID();
		switch (a_clip) {
		case kDTOn:
			Has(a_actor, g.mode[0]) ? Stage0(a_actor) : Stage1(a_actor);
			Cast(a_actor, g.vfx2);
			RE::ShakeCamera(0.5f, a_actor->GetPosition(), 0.5f);
			Cast(a_actor, g.vfxExp);
			break;
		case kSDTOn:
			Has(a_actor, g.mode[1]) ? Stage0(a_actor) : Stage2(a_actor);
			Cast(a_actor, g.vfx1);
			CastLater(id, 400, g.vfx2, 1.0f);
			CastLater(id, 700, g.vfxExp);
			CastLater(id, 1000, g.vfx3);
			break;
		default:  // DT off, SDT off
			Stage0(a_actor, true);
			break;
		}
	}

	// runs the clip's payload once: at the payload time or when the clip ends, whichever comes first
	void FireClip(RE::FormID a_actor, std::uint32_t a_token, const char* a_when)
	{
		auto it = g_clips.find(a_actor);
		if (it == g_clips.end() || it->second.token != a_token || it->second.fired) {
			return;
		}
		it->second.fired = true;
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actor);
		if (!actor || actor->IsDead()) {
			return;
		}
		REX::INFO("{} {:08X}: {} clip {}", Name(actor), a_actor, kClipName[it->second.clip], a_when);
		RunPayload(actor, it->second.clip);
	}

	// main thread: a clip marker came on (a_fromSave: restored by a save load); schedules the payload
	void ClipStarted(RE::FormID a_actor, int a_clip, bool a_fromSave)
	{
		if (!a_fromSave) {
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actor);
			if (!actor || !ShoutClipAllowed(actor, kClipName[a_clip])) {
				return;
			}
		}
		const auto token = ++g_clipToken;
		g_clips[a_actor] = { a_clip, token, false };
		if (a_fromSave) {
			if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actor)) {
				REX::INFO("{} {:08X}: {} clip marker came from the save; stage runs at the payload time", Name(actor), a_actor, kClipName[a_clip]);
			}
		}
		Later(kPayloadMs[a_clip], [a_actor, token]() { FireClip(a_actor, token, "at the payload time"); });
	}

	// main thread: the clip marker went off (clip over or interrupted)
	void ClipEnded(RE::FormID a_actor, int a_clip)
	{
		if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actor); actor && actor->IsPlayerRef()) {
			DispelAll(actor, g.hold);  // the shout's choice is used up
		}
		auto it = g_clips.find(a_actor);
		if (it != g_clips.end() && it->second.clip == a_clip) {
			FireClip(a_actor, it->second.token, "ended early");
		}
	}

	// Marker effects seen by Start (or, for one loaded from a save, by its first Update). A save made during a transformation clip
	// holds the marker: on load the game restores it without Start, the clip plays again, and the stage would never run.
	std::mutex                                               g_markerLock;
	std::set<std::pair<RE::ActiveEffect*, std::uint16_t>> g_markers;

	// which clip marker an effect is (Clip), -1 for any other effect
	int ClipOf(RE::ActiveEffect* a_effect)
	{
		auto* base = a_effect->GetBaseObject();
		for (int i = 0; i < 4; ++i) {
			if (base && base == g.clip[i]) {
				return i;
			}
		}
		return -1;
	}

	// ScriptEffect Start / Finish of a clip marker → ClipStarted / ClipEnded on the main thread
	void ClipMarker(RE::ActiveEffect* a_effect, bool a_start)
	{
		const int clip = ClipOf(a_effect);
		auto*     actor = clip >= 0 ? TargetActor(a_effect) : nullptr;
		if (!actor) {
			return;
		}
		{
			std::scoped_lock lock(g_markerLock);
			if (a_start) {
				g_markers.insert({ a_effect, a_effect->usUniqueID });
			} else {
				g_markers.erase({ a_effect, a_effect->usUniqueID });
			}
		}
		const auto id = actor->GetFormID();
		SKSE::GetTaskInterface()->AddTask([id, clip, a_start]() { a_start ? ClipStarted(id, clip, false) : ClipEnded(id, clip); });
	}

	// ScriptEffect Update: a marker that never had a Start (restored from a save) starts its clip here
	void ClipMarkerUpdate(RE::ActiveEffect* a_effect)
	{
		const int clip = ClipOf(a_effect);
		if (clip < 0 || a_effect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) {
			return;
		}
		auto* actor = TargetActor(a_effect);
		if (!actor) {
			return;
		}
		{
			std::scoped_lock lock(g_markerLock);
			if (!g_markers.insert({ a_effect, a_effect->usUniqueID }).second) {
				return;  // known
			}
		}
		const auto id = actor->GetFormID();
		SKSE::GetTaskInterface()->AddTask([id, clip]() { ClipStarted(id, clip, true); });
	}

	// ===== Hooks =====

	// trigger spells dsdtrigger0/1/2SP (console, other mods): their effect runs the stage directly
	void TriggerStart(RE::ActiveEffect* a_effect)
	{
		auto* base = a_effect->GetBaseObject();
		int   stage = -1;
		for (int i = 0; i < 3; ++i) {
			if (base && base == g.trigger[i]) {
				stage = i;
			}
		}
		if (stage < 0) {
			return;
		}
		auto* actor = TargetActor(a_effect);
		if (!actor) {
			return;
		}
		const auto id = actor->GetFormID();
		SKSE::GetTaskInterface()->AddTask([id, stage]() {
			auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!a || (a->IsDead() && stage != 0)) {
				return;
			}
			stage == 0 ? Stage0(a) : stage == 1 ? Stage1(a) : Stage2(a);
		});
	}

	// ScriptEffect vtable hooks (Transform::Install): the mod's marker / trigger / mode / cost / Luce effects are script archetype
	// without a script, so these calls are their only "script"
	struct ScriptEffectStart
	{
		static void thunk(RE::ActiveEffect* a_this)
		{
			func(a_this);
			Luce::OnEffectStart(a_this);
			if (g_ready) {
				TriggerStart(a_this);
				ClipMarker(a_this, true);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ScriptEffectFinish
	{
		static void thunk(RE::ActiveEffect* a_this)
		{
			Luce::OnEffectFinish(a_this);
			if (g_ready) {
				ClipMarker(a_this, false);
			}
			func(a_this);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct ScriptEffectUpdate
	{
		static void thunk(RE::ActiveEffect* a_this, float a_delta)
		{
			func(a_this, a_delta);
			Luce::OnEffectUpdate(a_this, a_delta);
			if (g_ready) {
				Cost::OnUpdate(a_this, a_delta);
				ModeUpdate(a_this, a_delta);
				ClipMarkerUpdate(a_this);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// ShoutHandler::CanProcess (VTABLE_ShoutHandler, slot 1): held events of the shout button are dropped while g_blocking
	// (one transformation per key press). Down and up events always pass.
	struct ShoutHandlerCanProcess
	{
		static bool thunk(RE::ShoutHandler* a_this, RE::InputEvent* a_event)
		{
			if (g_blocking && a_event) {
				if (auto* button = a_event->AsButtonEvent(); button && button->IsHeld() &&
															 button->QUserEvent() == RE::UserEvents::GetSingleton()->shout) {
					return false;
				}
			}
			return func(a_this, a_event);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// ===== Events =====

	// dying and cell detach end a mode without the untransform animation
	class EventSink final :
		public RE::BSTEventSink<RE::TESDeathEvent>,
		public RE::BSTEventSink<RE::TESCellAttachDetachEvent>
	{
	public:
		static EventSink* GetSingleton()
		{
			static EventSink sink;
			return &sink;
		}

		// dying (dead = false): the player untransforms, NPCs just lose the abilities
		RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override
		{
			auto* actor = a_event && !a_event->dead && a_event->actorDying ? a_event->actorDying->As<RE::Actor>() : nullptr;
			if (actor) {
				const auto id = actor->GetFormID();
				SKSE::GetTaskInterface()->AddTask([id]() {
					auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
					if (!a || ModeOf(a) < 0) {
						return;
					}
					if (a->IsPlayerRef()) {
						EndMode(a, false, "dying");
					} else {
						REX::INFO("{} {:08X}: dying, mode removed", Name(a), id);
						RemoveModes(a);
					}
				});
			}
			return RE::BSEventNotifyControl::kContinue;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent* a_event,
			RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
		{
			auto* actor = a_event && !a_event->attached && a_event->reference ? a_event->reference->As<RE::Actor>() : nullptr;
			if (actor && !actor->IsPlayerRef()) {
				const auto id = actor->GetFormID();
				SKSE::GetTaskInterface()->AddTask([id]() {
					if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id); a && ModeOf(a) >= 0) {
						REX::INFO("{} {:08X}: cell detached, mode removed", Name(a), id);
						RemoveModes(a);
					}
				});
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// ===== Load-time setup =====

	// After every load: the player knows and has unlocked shout C's three words (each taught only once: teaching all on every load
	// shows the "word learned" messages each time) and has shout C, not A (DT) or B (SDT); A and B stay in the plugin for OAR, NPCs and
	// other mods. Game.IsWordUnlocked / TeachWord / UnlockWord and Actor.AddShout / RemoveShout run on the VM.
	void SetUpShouts(RE::PlayerCharacter* a_player)
	{
		auto* vm = VM();
		if (!vm) {
			return;
		}
		for (auto* word : g.words) {
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback(new BoolResult([word](bool a_unlocked) {
				if (a_unlocked) {
					return;
				}
				auto* vm = VM();
				RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
				vm->DispatchStaticCall("Game", "TeachWord", RE::MakeFunctionArguments(static_cast<RE::TESWordOfPower*>(word)), none);
				vm->DispatchStaticCall("Game", "UnlockWord", RE::MakeFunctionArguments(static_cast<RE::TESWordOfPower*>(word)), none);
				REX::INFO("transform: word {:08X} taught and unlocked", word->GetFormID());
			}));
			vm->DispatchStaticCall("Game", "IsWordUnlocked", RE::MakeFunctionArguments(static_cast<RE::TESWordOfPower*>(word)), callback);
		}
		auto* actor = static_cast<RE::Actor*>(a_player);
		CallMethod(actor, "Actor", "AddShout", static_cast<RE::TESShout*>(g.shoutC));
		CallMethod(actor, "Actor", "RemoveShout", static_cast<RE::TESShout*>(g.shoutA));  // false when the player doesn't have it
		CallMethod(actor, "Actor", "RemoveShout", static_cast<RE::TESShout*>(g.shoutB));
	}

	// A save made while transformed with the old Papyrus version of the mod (1.0.x, which equipped the DT armor) still wears the
	// DT armor. Take both DT armors off and out of the inventory and put the outfit stored by the old version back on (only when one
	// of them was worn), then scale 1.0. Only while the old wp DMC.esp is still loaded; without it the engine drops those items itself.
	void CleanUpLegacyArmor(RE::PlayerCharacter* a_player)
	{
		const auto inv = a_player->GetInventory([](RE::TESBoundObject& a_obj) {
			return &a_obj == g.legacyArmor[0] || &a_obj == g.legacyArmor[1];
		});
		if (inv.empty()) {
			return;
		}
		auto* actor = static_cast<RE::Actor*>(a_player);
		auto* equip = RE::ActorEquipManager::GetSingleton();
		bool  worn = false;
		for (const auto& [obj, data] : inv) {
			if (data.second && data.second->IsWorn()) {
				worn = true;
				equip->UnequipObject(actor, obj, nullptr, 1, nullptr, false, false, false);
			}
			CallMethod(actor, "ObjectReference", "RemoveItem", static_cast<RE::TESForm*>(obj), 99, true, static_cast<RE::TESObjectREFR*>(nullptr));
		}
		std::uint32_t restored = 0;
		if (worn && g.legacyOutfit) {
			auto restore = [&](RE::TESForm* a_form) {
				auto* armor = a_form ? a_form->As<RE::TESObjectARMO>() : nullptr;
				if (armor && a_player->GetItemCount(armor) > 0) {
					equip->EquipObject(actor, armor, nullptr, 1, nullptr, false, false, false);
					++restored;
				}
			};
			for (auto* form : g.legacyOutfit->forms) {
				restore(form);
			}
			if (g.legacyOutfit->scriptAddedTempForms) {  // StoreOutfit used AddForm: these are the stored items
				for (const auto id : *g.legacyOutfit->scriptAddedTempForms) {
					restore(RE::TESForm::LookupByID(id));
				}
			}
		}
		a_player->SetScale(1.0f);
		REX::INFO("transform: legacy DT armor removed ({} kind(s), worn {}), {} stored item(s) re-equipped, scale 1", inv.size(), worn, restored);
	}
}

namespace Transform
{
	// ===== Public =====

	bool Install()
	{
		// ScriptEffect vtable: slot 0x14 = Start() (1.7.104 ID 34887), 0x15 = Finish() (ID 34888), 4 = Update(float) (ID 34889)
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_ScriptEffect[0] };
		ScriptEffectStart::func = vtbl.write_vfunc(0x14, ScriptEffectStart::thunk);
		ScriptEffectFinish::func = vtbl.write_vfunc(0x15, ScriptEffectFinish::thunk);
		ScriptEffectUpdate::func = vtbl.write_vfunc(0x4, ScriptEffectUpdate::thunk);
		REX::INFO("transform: hooked ScriptEffect::Start/Finish/Update");

		REL::Relocation<std::uintptr_t> shoutVtbl{ RE::VTABLE_ShoutHandler[0] };
		ShoutHandlerCanProcess::func = shoutVtbl.write_vfunc(0x1, ShoutHandlerCanProcess::thunk);
		REX::INFO("transform: hooked ShoutHandler::CanProcess");
		return true;
	}

	bool OnDataLoaded()
	{
		bool ok = true;
		const RE::FormID trigger[3] = { 0x830, 0x822, 0x823 };
		for (int i = 0; i < 3; ++i) {
			g.trigger[i] = Lookup<RE::EffectSetting>(trigger[i], kPlugin, ok);
		}
		g.mode[0] = Lookup<RE::EffectSetting>(0x802, kPlugin, ok);
		g.mode[1] = Lookup<RE::EffectSetting>(0x838, kPlugin, ok);
		g.modeSpell[0] = Lookup<RE::SpellItem>(0x803, kPlugin, ok);
		g.modeSpell[1] = Lookup<RE::SpellItem>(0x847, kPlugin, ok);
		g.duration[0] = Lookup<RE::TESGlobal>(0x817, kPlugin, ok);
		g.duration[1] = Lookup<RE::TESGlobal>(0x814, kPlugin, ok);
		g.clip[kDTOn] = Lookup<RE::EffectSetting>(0x863, kPlugin, ok);
		g.clip[kDTOff] = Lookup<RE::EffectSetting>(0x865, kPlugin, ok);
		g.clip[kSDTOn] = Lookup<RE::EffectSetting>(0x85D, kPlugin, ok);
		g.clip[kSDTOff] = Lookup<RE::EffectSetting>(0x867, kPlugin, ok);
		g.vfx1 = Lookup<RE::SpellItem>(0x82B, kPlugin, ok);
		g.vfx2 = Lookup<RE::SpellItem>(0x82C, kPlugin, ok);
		g.vfx3 = Lookup<RE::SpellItem>(0x82D, kPlugin, ok);
		g.vfx4 = Lookup<RE::SpellItem>(0x82E, kPlugin, ok);
		g.vfxExp = Lookup<RE::SpellItem>(0x82F, kPlugin, ok);
		g.intro = Lookup<RE::EffectSetting>(0x85D, kPlugin, ok);
		g.introCancel = Lookup<RE::SpellItem>(0x860, kPlugin, ok);
		g.cancelEffect[0] = Lookup<RE::EffectSetting>(0x861, kPlugin, ok);
		g.cancelEffect[1] = Lookup<RE::EffectSetting>(0x85F, kPlugin, ok);
		g.cancelSpell[0] = Lookup<RE::SpellItem>(0x862, kPlugin, ok);
		g.cancelSpell[1] = Lookup<RE::SpellItem>(0x860, kPlugin, ok);
		g.combatIdle = Lookup<RE::TESIdleForm>(0x0B8C73, "Skyrim.esm", ok);
		g.transformVFX = Lookup<RE::BGSReferenceEffect>(0x819, kPlugin, ok);
		g.dtOn = Lookup<RE::TESForm>(0x81F, kPlugin, ok);
		g.dtOnVoice = Lookup<RE::TESForm>(0x806, kPlugin, ok);
		g.dtOff = Lookup<RE::TESForm>(0x81E, kPlugin, ok);
		g.sdtOn = Lookup<RE::TESForm>(0x815, kPlugin, ok);
		g.outOfMana = Lookup<RE::TESForm>(0x837, kPlugin, ok);
		g.flameOut = Lookup<RE::TESForm>(0x810, kPlugin, ok);
		g.legacyOutfit = Lookup<RE::BGSListForm>(0x807, kPlugin, ok);
		g.shoutA = Lookup<RE::TESShout>(0x809, kPlugin, ok);
		g.shoutB = Lookup<RE::TESShout>(0x840, kPlugin, ok);
		g.words[0] = Lookup<RE::TESWordOfPower>(0x827, kPlugin, ok);
		g.words[1] = Lookup<RE::TESWordOfPower>(0x828, kPlugin, ok);
		g.words[2] = Lookup<RE::TESWordOfPower>(0x829, kPlugin, ok);
		g.shoutC = Lookup<RE::TESShout>(0x826, kPlugin, ok);
		g.hold = Lookup<RE::EffectSetting>(0x869, kPlugin, ok);
		g.holdSpell = Lookup<RE::SpellItem>(0x86A, kPlugin, ok);
		// optional: JCE sounds/explosion (master of the ESP, so normally present), legacy armors
		bool optional = true;
		g.uiActi = Lookup<RE::TESForm>(0x8E9, kJCE, optional);
		g.uiNote = Lookup<RE::TESForm>(0x8EB, kJCE, optional);
		g.sdtExplosion = Lookup<RE::TESBoundObject>(0x890, kJCE, optional);
		if (RE::TESDataHandler::GetSingleton()->LookupModByName(kLegacyArmorPlugin)) {  // only on setups that still load the 1.0.x plugin
			g.legacyArmor[0] = Lookup<RE::TESBoundObject>(0x806, kLegacyArmorPlugin, optional);
			g.legacyArmor[1] = Lookup<RE::TESBoundObject>(0x80C, kLegacyArmorPlugin, optional);
		}
		if (!ok) {
			REX::ERROR("transform: forms missing; transformations are off");
			return false;
		}
		auto* events = RE::ScriptEventSourceHolder::GetSingleton();
		events->AddEventSink<RE::TESDeathEvent>(EventSink::GetSingleton());
		events->AddEventSink<RE::TESCellAttachDetachEvent>(EventSink::GetSingleton());
		if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
			input->AddEventSink<RE::InputEvent*>(InputSink::GetSingleton());
		}
		g_ready = true;
		HoldSeconds();  // logs the thresholds
		REX::INFO("transform: ready");
		return true;
	}

	void OnGameLoaded()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!g_ready || !player) {
			return;
		}
		SetUpShouts(player);
		CleanUpLegacyArmor(player);
	}

	void SetSdtScale(float a_factor)
	{
		g_sdtScale = std::clamp(a_factor, kSdtScaleMin, kSdtScaleMax);
	}

	float SdtScale() { return g_sdtScale.load(); }

	void SetHoldSeconds(float a_seconds)
	{
		g_holdSeconds = a_seconds > 0.0f ? a_seconds : 0.25f;
	}

	void OutOfMagicka(RE::FormID a_actor, int a_mode)
	{
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actor);
		if (!actor || actor->IsDead() || ModeOf(actor) != a_mode) {
			return;
		}
		// no outofmanasfx here: Stage0 plays it (magicka < 10 %); playing it here too gives two voice lines
		EndMode(actor, true, "out of magicka");
	}

	void Forget(RE::FormID a_actor)
	{
		g_ending.erase(a_actor);
		std::scoped_lock lock(g_lock);
		std::erase_if(g_modeState, [a_actor](const auto& a_entry) { return a_entry.second.actor == a_actor; });
	}

	void Reset()
	{
		++g_epoch;
		g_appliedScale.clear();
		g_keyDown = false;
		g_blocking = false;
		g_pressFired = UINT32_MAX;
		{
			std::scoped_lock lock(g_markerLock);
			g_markers.clear();
		}
		g_clips.clear();
		g_ending.clear();
		g_generation.clear();
		std::scoped_lock lock(g_lock);
		g_modeState.clear();
	}
}
