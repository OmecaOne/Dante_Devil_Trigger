#pragma once

// The Luce: the SDT power attack's sword volley.
//
// Trigger: the moveset's power attack loop clip runs PIE.$Luce, which casts dsdtluceSP (0x84E). Its effect dsdtluceMGEF (0x84D) is a
// script-archetype effect without a script; ScriptEffect::Start/Finish/Update (hooked in transform.cpp) drive this module.
// While the effect is active, 30 emitters each fire dsdtlucefireSP every 0.1-0.8 s from a random point on a sphere around the actor:
//   target (TDM lock for the player, combat target for NPCs): aimed at it;
//   no target: on gravity arcs that converge ahead (on the nearest actor in a corridor ahead, else 1500 units away).
// Firing stops with the effect. Emitter timers survive the short gap between two casts of the looping clip.
// End of the attack: hkbClipGenerator::Deactivate (vtable slot 7, 1.7.104 ID 59254) on BFCO/MCO_PowerAttackLoop1.hkx dispels the effect at once;
// the spell's 1 s duration remains the fallback. The clips and their annotations are not edited.

namespace Luce
{
	// at plugin load: hkbClipGenerator::Deactivate hook
	void Install();

	// at data load: resolve the forms and the TDM API
	void OnDataLoaded();

	// before a save loads / new game
	void Reset();

	// ScriptEffect hooks; they ignore every effect except dsdtluceMGEF
	void OnEffectStart(RE::ActiveEffect* a_effect);
	void OnEffectFinish(RE::ActiveEffect* a_effect);
	void OnEffectUpdate(RE::ActiveEffect* a_effect, float a_delta);
}
