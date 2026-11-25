#pragma once

// Magicka cost of the transformation:
// - Drain: while an actor's cost effect (dsdtcostAMGEF / dsdtcostBMGEF, script archetype, no script) is active, its magicka drains
//   continuously at the mode's cost per second, after a short delay (the activation cost is paid at the start). The effect's own
//   condition (mode duration <= 0) decides whether the mode drains at all. Hook: ScriptEffect::Update (vtable slot 4), which the
//   engine calls every frame for every active script effect with the frame's effect time (the hook is in transform.cpp).
// - Out of magicka: when the actor can't pay the next frame's share, Transform::OutOfMagicka ends that actor's mode.
// - Activation cost: Transform's stages 1/2 (ModeCost).
// Player: the INI's <dt|sdt>modecost points. NPCs: <dt|sdt>npcmodecost percent of their maximum magicka (at least 1 point).

namespace Cost
{
	struct Settings
	{
		float rate = 0.0f;         // player: magicka per second (and on activation)
		float npcPercent = 5.0f;   // NPCs: percent of maximum magicka per second (and on activation)
		float delay = 3.0f;        // seconds after the mode starts before the drain begins
	};

	// at data load and on menu changes (main thread): the cost effects (index 0 = DT, 1 = SDT) and their settings
	void Configure(int a_mode, RE::EffectSetting* a_costEffect, const Settings& a_settings, bool a_log = true);

	// magicka per second, also the activation cost; a_mode 0 = DT, 1 = SDT
	float ModeCost(RE::Actor* a_actor, int a_mode);

	// ScriptEffect::Update (any thread): drains magicka when the effect is a cost effect
	void OnUpdate(RE::ActiveEffect* a_effect, float a_delta);

	// main thread: the actor has no mode any more (drop its drain state)
	void Forget(RE::FormID a_actor);

	// before a save loads / new game
	void Reset();
}
