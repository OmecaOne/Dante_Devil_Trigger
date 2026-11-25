#pragma once

// Transformation handler.
//
// Trigger: the OAR sections "DevilTrigger ON/OFF" and "SinDevilTrigger ON/OFF" cast a clip marker (dsdtclipDTon/DToff/SDToff, dsdtintro
// for SDT on; script archetype without a script) when their clip starts and dispel it when it ends. Hooks ScriptEffect::Start/Finish
// (vtable slots 0x14/0x15, 1.7.104 IDs 34887/34888). The stage and the VFX of the old PIE payload run at the payload's clip time
// (DT on 0.167 s, SDT on 1.9 s, off 0.017/0.033 s), or at once when the clip ends before that. Casting dsdtrigger0/1/2SP (console,
// other mods) still works: their effects run the stages directly, the spells' conditions picking on or off.
// Stages (main thread):
//   0 = untransform: sound (SDT: flame out; else VFX + DT off sound), remove both mode abilities, SDT scale undone,
//       out-of-magicka sound below 10 %.
//   1 = DT: activation cost (or refusal: sound, cut the SDT intro clip, notification for the player), VFX + sounds, DT ability on,
//       SDT ability off (added first, so the costume never drops for a frame).
//   2 = SDT: activation cost, sound, 0.4 s later the activation sound + explosion, SDT ability on, DT ability off, scale x 1.1.
// Lifecycle (ScriptEffect::Update on dsdtmodeAMGEF / dsdtmodeBMGEF, also script archetype without a script):
//   mode duration over, out of magicka (from cost.cpp), NPC out of combat for 1 s → VFX + untransform + untransform animation;
//   NPC bleedout → the same without the animation; dying: player untransforms, NPCs just lose the abilities; NPC cell detach → abilities removed.
// Untransform animation: inside the SDT transformation clip cast the intro cancel marker; otherwise cast
//   the cancel marker and play Skyrim.esm CombatIdleABC, which the OAR "...Trigger CANCEL" sections turn into the untransform clip.

namespace Transform
{
	// at plugin load: ScriptEffect::Start/Update hooks
	bool Install();

	// at data load: resolve the forms; false when something is missing (the handler then stays off)
	bool OnDataLoaded();

	// after a save load / new game: shout C and its words for the player, and undo a transformation that the old Papyrus version
	// of the mod (1.0.x) left equipped (legacy DT armor)
	void OnGameLoaded();

	// SDT size factor (INI [SinDevilTrigger] sdtscale, menu slider), applied at the next SDT transformation
	inline constexpr float kSdtScaleMin = 0.5f;
	inline constexpr float kSdtScaleMax = 2.0f;
	void  SetSdtScale(float a_factor);
	float SdtScale();

	// at data load, before OnDataLoaded: seconds the shout key must be held for SDT (INI [SinDevilTrigger] sdtholdtime)
	void SetHoldSeconds(float a_seconds);

	// main thread: the actor ran out of magicka in a mode (0 = DT, 1 = SDT)
	void OutOfMagicka(RE::FormID a_actor, int a_mode);

	// main thread: the actor has no mode any more
	void Forget(RE::FormID a_actor);

	// before a save loads / new game
	void Reset();
}
