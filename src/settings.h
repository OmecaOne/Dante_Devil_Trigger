#pragma once

// Settings: Data\OmecaOneDanteSDT.ini, written into the forms the transformation uses, and changed live by the menu (menu.cpp).
// - Perk entries of dsdtPerkStage1/2 (attack, power attack, power attack stamina, incoming damage, incoming stagger): read by the
//   engine whenever it evaluates them, so a change applies at once.
// - Effect magnitudes of the mode abilities dsdtmodeASP/BSP (movement, stamina regen, defense, resist, shout cooldown, attack speed,
//   health regen): an active ability keeps its magnitudes, so a change applies at the next transformation.
// - Globals read by effect conditions and the DLL (duration, cost, water walking, no fall damage): saved in the save, so they are set
//   again after every load; a change applies at once.
// - Module settings: cost per second + drain delay (cost.cpp), SDT hold time and size (transform.cpp).
// Attack speed: the INI gives a bonus (0.15 = +15 %). Comprehensive Attack Rate Patch makes WeaponSpeedMult default to 1, so the
// bonus is added as is; without it the base is 0 ("normal speed") and the magnitude gets +1.
// Shout cooldown: the INI gives a multiplier (0.5 = half); the engine subtracts the magnitude from ShoutRecoveryMult's 1, so the
// magnitude is 1 - value.
// NPC settings (<dt|sdt>npcmodecost) are still read when present but are not in the shipped INI or the menu (2.0: player only).

namespace Settings
{
	// one INI value: [DevilTrigger] / [SinDevilTrigger] <dt|sdt><key>
	struct Option
	{
		int         mode;  // 0 = DT, 1 = SDT
		const char* key;   // without the dt/sdt prefix
		const char* label;
		const char* help;
		float       min;
		float       max;
		const char* format;  // slider format; nullptr = checkbox (0/1, written as true/false)
		float       defaultValue;
	};

	// every option the menu shows, DT first, then SDT
	std::span<const Option> Options();

	// INI section name of a mode ("DevilTrigger" / "SinDevilTrigger")
	const char* Section(int a_mode);

	// at data load: read the INI and apply everything
	void Load();

	// after a save load / new game: globals again (they are saved), attack speed offset from the player's WeaponSpeedMult base
	void OnGameLoaded();

	// current value of an option (INI value, else its default)
	float Get(const Option& a_option);

	// menu: new value; applied on the main thread (SKSE task), not saved
	void Set(const Option& a_option, float a_value);

	// menu: write the option's current value into the INI
	void Save(const Option& a_option);
}
