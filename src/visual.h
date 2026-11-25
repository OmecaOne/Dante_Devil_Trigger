#pragma once

// Visual-only transformation, built after Fitting Room's worn-pass design (maartenharms/fitting-room, GPL-3):
// the DT/SDT costume is drawn over whatever the actor wears instead of being equipped. Works for any actor (player and NPCs).

namespace Visual
{
	enum class Costume : int
	{
		kNone = -1,
		kDT = 0,
		kSDT = 1,
	};

	// at plugin load: hooks the worn pass (ID 24725+0x1EF) and the worn-mask call (ID 24724+0x80); false when the sites don't match
	bool Install();

	// at data load: resolves the costume armors (dsdtarmorA_wings_closed / dsdtarmorB_wings_open)
	void OnDataLoaded();

	// main thread: the actor's mode changed (or may have); rebuilds its equipment 3D when the costume changes
	void SetCostume(RE::Actor* a_actor, Costume a_costume);

	// before a save loads / new game: forget every actor's state
	void Reset();
}
