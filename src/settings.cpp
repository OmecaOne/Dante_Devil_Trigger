#include "settings.h"

#include "cost.h"
#include "transform.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace
{
	// ===== Forms =====

	constexpr std::string_view kPlugin = "OmecaOneDanteSDT.esp";
	constexpr const char*      kIniPath = "Data/OmecaOneDanteSDT.ini";

	// local form IDs in OmecaOneDanteSDT.esp
	namespace ID
	{
		constexpr RE::FormID PerkDT = 0x84A, PerkSDT = 0x84B;
		constexpr RE::FormID SpellDT = 0x803, SpellSDT = 0x847;
		constexpr RE::FormID CostEffectDT = 0x820, CostEffectSDT = 0x821;  // dsdtcostAMGEF / dsdtcostBMGEF (in the mode abilities)

		constexpr RE::FormID MoveSpeed = 0x848;    // SpeedMult
		constexpr RE::FormID StaminaRate = 0x849;  // StaminaRateMult
		constexpr RE::FormID Defense = 0x80E;      // ArmorPerks
		constexpr RE::FormID ResistFire = 0x80C;   // ResistFire (copy of Dragon Aspect, second AV has weight 0)
		constexpr RE::FormID ShoutRate = 0x80D;    // ShoutRecoveryMult
		constexpr RE::FormID AttackSpeed = 0x844;  // WeaponSpeedMult + LeftWeaponSpeedMult
		constexpr RE::FormID HealRate = 0x818;     // HealRate

		constexpr RE::FormID DurationDT = 0x817, CostDT = 0x831, DurationSDT = 0x814, CostSDT = 0x82A;
		constexpr RE::FormID WaterWalkDT = 0x859, NoFallDT = 0x85B, WaterWalkSDT = 0x83C, NoFallSDT = 0x841;

		constexpr RE::FormID AbResistFrost = 0x24315, AbResistShock = 0x24318;  // Skyrim.esm
	}

	// one INI section and its forms per mode: [0] = DT, [1] = SDT
	struct Mode
	{
		const char* section;
		const char* prefix;  // INI key prefix: "dt" / "sdt"
		RE::FormID  perk, spell, duration, cost, waterWalk, noFall, costEffect;
	};
	constexpr Mode kModes[] = {
		{ "DevilTrigger", "dt", ID::PerkDT, ID::SpellDT, ID::DurationDT, ID::CostDT, ID::WaterWalkDT, ID::NoFallDT, ID::CostEffectDT },
		{ "SinDevilTrigger", "sdt", ID::PerkSDT, ID::SpellSDT, ID::DurationSDT, ID::CostSDT, ID::WaterWalkSDT, ID::NoFallSDT, ID::CostEffectSDT },
	};

	// ===== Options (menu order; defaults = the shipped INI) =====

	constexpr const char* kCostHelp = "Duration 0: magicka per second while transformed (and on activation). Otherwise paid once.";
	constexpr const char* kLater = "Applies at the next transformation.";

	constexpr Settings::Option kOptions[] = {
		// Devil Trigger
		{ 0, "modeduration", "Duration (s)##dt", "0 = lasts while you have magicka.", 0.0f, 600.0f, "%.0f", 0.0f },
		{ 0, "modecost", "Magicka cost##dt", kCostHelp, 0.0f, 100.0f, "%.1f", 5.0f },
		{ 0, "costdelay", "Drain delay (s)##dt", "Seconds after the transformation before the per-second drain starts.", 0.0f, 30.0f, "%.1f", 3.0f },
		{ 0, "attackbonus", "Attack damage x##dt", "Normal attack damage multiplier.", 0.5f, 5.0f, "%.2f", 1.2f },
		{ 0, "powerattackbonus", "Power attack damage x##dt", "Power attack damage multiplier.", 0.5f, 5.0f, "%.2f", 1.2f },
		{ 0, "speed", "Attack speed bonus##dt", "Melee attack speed bonus (0.15 = +15 %). " "Applies at the next transformation.", 0.0f, 2.0f, "%.2f", 0.15f },
		{ 0, "movementspeed", "Movement speed bonus##dt", kLater, 0.0f, 100.0f, "%.0f", 10.0f },
		{ 0, "damagereduction", "Incoming damage x##dt", "Incoming damage multiplier.", 0.0f, 2.0f, "%.2f", 0.8f },
		{ 0, "staggerreduction", "Incoming stagger x##dt", "Incoming stagger multiplier (stagger mods may override it).", 0.0f, 2.0f, "%.2f", 0.5f },
		{ 0, "powerstaminacost", "Power attack stamina x##dt", "Power attack stamina cost multiplier.", 0.0f, 2.0f, "%.2f", 0.9f },
		{ 0, "staminaregen", "Stamina regen bonus##dt", kLater, 0.0f, 200.0f, "%.0f", 10.0f },
		{ 0, "regen", "Health regen##dt", "Added to HealRate (% of max health per second; vanilla base 0.7). " "Applies at the next transformation.", 0.0f, 20.0f, "%.1f", 3.0f },
		{ 0, "defense", "Armor bonus##dt", kLater, 0.0f, 500.0f, "%.0f", 75.0f },
		{ 0, "resist", "Fire/frost/shock resist (%)##dt", kLater, 0.0f, 100.0f, "%.0f", 20.0f },
		{ 0, "voicebonus", "Shout cooldown x##dt", "0.5 = half the cooldown, 1 = unchanged. " "Applies at the next transformation.", 0.0f, 1.0f, "%.2f", 0.5f },
		{ 0, "nofalldmg", "No fall damage##dt", "", 0.0f, 1.0f, nullptr, 1.0f },
		{ 0, "waterwalking", "Water walking##dt", "", 0.0f, 1.0f, nullptr, 1.0f },
		// Sin Devil Trigger
		{ 1, "modeduration", "Duration (s)##sdt", "0 = lasts while you have magicka.", 0.0f, 600.0f, "%.0f", 0.0f },
		{ 1, "modecost", "Magicka cost##sdt", kCostHelp, 0.0f, 100.0f, "%.1f", 10.0f },
		{ 1, "costdelay", "Drain delay (s)##sdt", "Seconds after the transformation before the per-second drain starts.", 0.0f, 30.0f, "%.1f", 3.0f },
		{ 1, "attackbonus", "Attack damage x##sdt", "Normal attack damage multiplier.", 0.5f, 5.0f, "%.2f", 1.5f },
		{ 1, "powerattackbonus", "Power attack damage x##sdt", "Power attack damage multiplier.", 0.5f, 5.0f, "%.2f", 1.5f },
		{ 1, "speed", "Attack speed bonus##sdt", "Melee attack speed bonus (0.3 = +30 %). " "Applies at the next transformation.", 0.0f, 2.0f, "%.2f", 0.3f },
		{ 1, "movementspeed", "Movement speed bonus##sdt", kLater, 0.0f, 100.0f, "%.0f", 20.0f },
		{ 1, "damagereduction", "Incoming damage x##sdt", "Incoming damage multiplier.", 0.0f, 2.0f, "%.2f", 0.5f },
		{ 1, "staggerreduction", "Incoming stagger x##sdt", "Incoming stagger multiplier (stagger mods may override it).", 0.0f, 2.0f, "%.2f", 0.0f },
		{ 1, "powerstaminacost", "Power attack stamina x##sdt", "Power attack stamina cost multiplier.", 0.0f, 2.0f, "%.2f", 0.8f },
		{ 1, "staminaregen", "Stamina regen bonus##sdt", kLater, 0.0f, 200.0f, "%.0f", 20.0f },
		{ 1, "regen", "Health regen##sdt", "Added to HealRate (0 = none, as in DMC5). " "Applies at the next transformation.", 0.0f, 20.0f, "%.1f", 0.0f },
		{ 1, "defense", "Armor bonus##sdt", kLater, 0.0f, 500.0f, "%.0f", 75.0f },
		{ 1, "resist", "Fire/frost/shock resist (%)##sdt", kLater, 0.0f, 100.0f, "%.0f", 50.0f },
		{ 1, "voicebonus", "Shout cooldown x##sdt", "0.2 = a fifth of the cooldown. " "Applies at the next transformation.", 0.0f, 1.0f, "%.2f", 0.2f },
		{ 1, "nofalldmg", "No fall damage##sdt", "", 0.0f, 1.0f, nullptr, 1.0f },
		{ 1, "waterwalking", "Water walking##sdt", "", 0.0f, 1.0f, nullptr, 1.0f },
		{ 1, "holdtime", "Shout hold time for SDT (s)##sdt", "Hold the Devil Trigger shout this long for SDT; a shorter tap gives DT.", 0.05f, 1.5f, "%.2f", 0.25f },
		{ 1, "scale", "SDT size##sdt", "Size factor while in SDT (1.10 = 10 % larger). Applies at the next SDT transformation.",
			Transform::kSdtScaleMin, Transform::kSdtScaleMax, "%.2f", 1.1f },
	};

	// ===== State =====

	std::mutex                         g_lock;  // g_ini (menu thread + main thread)
	std::map<std::string, std::string> g_ini;   // "section.key" (lower case) -> value
	float                              g_attackSpeedOffset = 0.0f;  // 1 when the player's WeaponSpeedMult base is 0 (no CARP)

	// ===== INI =====

	std::string Lower(std::string s)
	{
		std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	}

	std::string Trim(const std::string& s)
	{
		const auto b = s.find_first_not_of(" \t\r\n");
		const auto e = s.find_last_not_of(" \t\r\n");
		return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
	}

	std::string IniKey(const Mode& a_mode, std::string_view a_key)
	{
		return Lower(a_mode.section) + "." + a_mode.prefix + std::string(a_key);
	}

	// Data\OmecaOneDanteSDT.ini into g_ini (comments after ';' dropped)
	void LoadIni()
	{
		std::ifstream in(kIniPath);
		if (!in) {
			REX::ERROR("{} not found; using the values stored in {}", kIniPath, kPlugin);
			return;
		}
		std::scoped_lock lock(g_lock);
		std::string      line, section;
		while (std::getline(in, line)) {
			line = Trim(line);
			if (line.empty() || line[0] == ';' || line[0] == '#') {
				continue;
			}
			if (line.front() == '[' && line.back() == ']') {
				section = Lower(Trim(line.substr(1, line.size() - 2)));
				continue;
			}
			const auto eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			auto value = line.substr(eq + 1);
			if (const auto c = value.find(';'); c != std::string::npos) {
				value = value.substr(0, c);
			}
			g_ini[section + "." + Lower(Trim(line.substr(0, eq)))] = Trim(value);
		}
		REX::INFO("read {} values from {}", g_ini.size(), kIniPath);
	}

	// [section] <prefix><key> as a number (true/false = 1/0); nullopt when missing or not a number
	std::optional<float> IniFloat(const Mode& a_mode, std::string_view a_key)
	{
		std::string raw;
		{
			std::scoped_lock lock(g_lock);
			const auto       it = g_ini.find(IniKey(a_mode, a_key));
			if (it == g_ini.end()) {
				return std::nullopt;
			}
			raw = it->second;
		}
		const auto v = Lower(raw);
		if (v == "true") {
			return 1.0f;
		}
		if (v == "false") {
			return 0.0f;
		}
		try {
			return std::stof(v);
		} catch (...) {
			REX::WARN("[{}] {}{} = '{}' is not a number; ignored", a_mode.section, a_mode.prefix, a_key, raw);
			return std::nullopt;
		}
	}

	// writes a_key=a_value into [a_section] of the INI (replaces the key, or adds it below the section header)
	bool SaveIniValue(const char* a_section, const std::string& a_key, const std::string& a_value)
	{
		std::vector<std::string> lines;
		{
			std::ifstream in(kIniPath);
			if (!in) {
				REX::WARN("settings: {} not found; {} not saved", kIniPath, a_key);
				return false;
			}
			for (std::string line; std::getline(in, line);) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				lines.push_back(line);
			}
		}
		const auto  section = Lower(a_section);
		const auto  key = Lower(a_key);
		std::string current;
		std::size_t header = std::string::npos;
		bool        done = false;
		for (std::size_t i = 0; i < lines.size() && !done; ++i) {
			const auto t = Trim(lines[i]);
			if (t.size() > 2 && t.front() == '[' && t.back() == ']') {
				current = Lower(Trim(t.substr(1, t.size() - 2)));
				if (current == section) {
					header = i;
				}
				continue;
			}
			const auto eq = t.find('=');
			if (current == section && eq != std::string::npos && t[0] != ';' && Lower(Trim(t.substr(0, eq))) == key) {
				lines[i] = a_key + "=" + a_value;
				done = true;
			}
		}
		if (!done) {
			if (header == std::string::npos) {
				lines.push_back(std::format("[{}]", a_section));
				header = lines.size() - 1;
			}
			lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(header) + 1, { a_key + "=" + a_value, "" });
		}
		std::ofstream out(kIniPath, std::ios::trunc);
		if (!out) {
			REX::WARN("settings: can't write {}", kIniPath);
			return false;
		}
		for (const auto& line : lines) {
			out << line << "\r\n";
		}
		REX::INFO("settings: saved [{}] {}={}", a_section, a_key, a_value);
		return true;
	}

	// ===== Applying the INI to the forms =====

	// a form of OmecaOneDanteSDT.esp (or a_file) by its local ID; logs a missing one
	template <class T>
	T* Form(RE::FormID a_local, std::string_view a_file = kPlugin)
	{
		auto* form = RE::TESDataHandler::GetSingleton()->LookupForm<T>(a_local, a_file);
		if (!form) {
			REX::ERROR("form {:03X} not found in {}", a_local, a_file);
		}
		return form;
	}

	// the mode perk's one-value entry points (attack, power attack, power attack stamina, incoming damage, incoming stagger)
	void SetPerkValues(const Mode& a_mode, bool a_log)
	{
		auto* perk = Form<RE::BGSPerk>(a_mode.perk);
		if (!perk) {
			return;
		}
		using EP = RE::BGSEntryPoint::ENTRY_POINT;
		const std::pair<EP, const char*> keys[] = {
			{ EP::kModAttackDamage, "attackbonus" },
			{ EP::kModPowerAttackDamage, "powerattackbonus" },
			{ EP::kModPowerAttackStamina, "powerstaminacost" },
			{ EP::kModIncomingDamage, "damagereduction" },
			{ EP::kModIncomingStagger, "staggerreduction" },
		};
		for (auto* entry : perk->perkEntries) {
			if (!entry || entry->GetType() != RE::PERK_ENTRY_TYPE::kEntryPoint) {
				continue;
			}
			auto* ep = static_cast<RE::BGSEntryPointPerkEntry*>(entry);
			auto* data = ep->functionData;
			if (!data || data->GetType() != RE::BGSEntryPointFunctionData::ENTRY_POINT_FUNCTION_DATA::kOneValue) {
				continue;
			}
			for (const auto& [point, key] : keys) {
				if (ep->entryData.entryPoint.get() == point) {
					if (const auto v = IniFloat(a_mode, key)) {
						static_cast<RE::BGSEntryPointFunctionDataOneValue*>(data)->data = *v;
						if (a_log) {
							REX::INFO("  {}{} = {}", a_mode.prefix, key, *v);
						}
					}
				}
			}
		}
	}

	// the mode ability's effect magnitudes (all other buffs)
	void SetSpellMagnitudes(const Mode& a_mode, bool a_log)
	{
		auto* spell = Form<RE::SpellItem>(a_mode.spell);
		if (!spell) {
			return;
		}
		const auto* frost = Form<RE::EffectSetting>(ID::AbResistFrost, "Skyrim.esm");
		const auto* shock = Form<RE::EffectSetting>(ID::AbResistShock, "Skyrim.esm");

		for (auto* effect : spell->effects) {
			if (!effect || !effect->baseEffect) {
				continue;
			}
			auto*       base = effect->baseEffect;
			const char* key = nullptr;
			float       offset = 0.0f;
			bool        subtract = false;
			if (base == frost || base == shock) {
				key = "resist";
			} else if (base->GetFile(0) && base->GetFile(0)->GetFilename() == kPlugin) {
				switch (base->GetLocalFormID()) {
				case ID::MoveSpeed:   key = "movementspeed"; break;
				case ID::StaminaRate: key = "staminaregen"; break;
				case ID::Defense:     key = "defense"; break;
				case ID::ResistFire:  key = "resist"; break;
				case ID::ShoutRate:   key = "voicebonus"; subtract = true; break;
				case ID::AttackSpeed: key = "speed"; offset = g_attackSpeedOffset; break;
				case ID::HealRate:    key = "regen"; break;
				default: break;
				}
			}
			if (!key) {
				continue;
			}
			if (const auto v = IniFloat(a_mode, key)) {
				// ShoutRecoveryMult works like vanilla's Fortify Shout effects: the magnitude is subtracted from 1
				// (magnitude -0.5 gives 1.5)
				effect->effectItem.magnitude = subtract ? 1.0f - *v : *v + offset;
				if (a_log) {
					REX::INFO("  {}{} = {} -> {} magnitude {}", a_mode.prefix, key, *v, base->GetName(), effect->effectItem.magnitude);
				}
			}
		}
	}

	// duration, cost, water walking, no fall damage (read by effect conditions; saved, so set again after every load)
	void SetGlobals()
	{
		for (const auto& mode : kModes) {
			const std::pair<RE::FormID, const char*> globals[] = {
				{ mode.duration, "modeduration" }, { mode.cost, "modecost" }, { mode.waterWalk, "waterwalking" }, { mode.noFall, "nofalldmg" }
			};
			for (const auto& [id, key] : globals) {
				auto*      global = Form<RE::TESGlobal>(id);
				const auto v = IniFloat(mode, key);
				if (global && v) {
					global->value = *v;
				}
			}
		}
	}

	// the modules' settings: cost (both modes), SDT hold time and size
	void ConfigureModules(int a_index, bool a_log)
	{
		const auto&    mode = kModes[a_index];
		Cost::Settings s;
		if (auto* global = Form<RE::TESGlobal>(mode.cost)) {
			s.rate = global->value;  // the value stored in the ESP when the INI has no key
		}
		s.rate = IniFloat(mode, "modecost").value_or(s.rate);
		s.npcPercent = IniFloat(mode, "npcmodecost").value_or(s.npcPercent);  // not in the shipped INI (2.0: player only)
		s.delay = IniFloat(mode, "costdelay").value_or(s.delay);
		Cost::Configure(a_index, Form<RE::EffectSetting>(mode.costEffect), s, a_log);
		if (a_index == 1) {
			Transform::SetHoldSeconds(IniFloat(mode, "holdtime").value_or(0.25f));
			Transform::SetSdtScale(IniFloat(mode, "scale").value_or(1.1f));
		}
	}

	void ApplyMode(int a_index, bool a_log)
	{
		const auto& mode = kModes[a_index];
		if (a_log) {
			REX::INFO("[{}]", mode.section);
		}
		SetPerkValues(mode, a_log);
		SetSpellMagnitudes(mode, a_log);
		ConfigureModules(a_index, a_log);
	}

	std::string FormatValue(const Settings::Option& a_option, float a_value)
	{
		if (!a_option.format) {
			return a_value != 0.0f ? "true" : "false";
		}
		return std::format("{:.4g}", a_value);
	}
}

namespace Settings
{
	std::span<const Option> Options() { return kOptions; }

	const char* Section(int a_mode) { return kModes[a_mode].section; }

	void Load()
	{
		LoadIni();
		for (int i = 0; i < 2; ++i) {
			ApplyMode(i, true);
		}
		SetGlobals();
	}

	void OnGameLoaded()
	{
		SetGlobals();  // globals are stored in the save
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		auto*       avOwner = static_cast<RE::ActorValueOwner*>(player);
		const float offset = avOwner->GetBaseActorValue(RE::ActorValue::kWeaponSpeedMult) == 0.0f ? 1.0f : 0.0f;
		if (offset != g_attackSpeedOffset) {
			g_attackSpeedOffset = offset;
			REX::WARN("player's WeaponSpeedMult base is {} (Comprehensive Attack Rate Patch {}): attack speed magnitudes {}",
				offset ? 0 : 1, offset ? "missing?" : "active", offset ? "get +1" : "as in the INI");
			for (const auto& mode : kModes) {
				SetSpellMagnitudes(mode, true);
			}
		}
	}

	float Get(const Option& a_option)
	{
		return IniFloat(kModes[a_option.mode], a_option.key).value_or(a_option.defaultValue);
	}

	void Set(const Option& a_option, float a_value)
	{
		{
			std::scoped_lock lock(g_lock);
			g_ini[IniKey(kModes[a_option.mode], a_option.key)] = FormatValue(a_option, a_value);
		}
		const int mode = a_option.mode;
		SKSE::GetTaskInterface()->AddTask([mode]() {
			ApplyMode(mode, false);
			SetGlobals();
		});
	}

	void Save(const Option& a_option)
	{
		const auto& mode = kModes[a_option.mode];
		SaveIniValue(mode.section, std::string(mode.prefix) + a_option.key, FormatValue(a_option, Get(a_option)));
	}
}
