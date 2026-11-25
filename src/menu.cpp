#include "menu.h"

#include "settings.h"

#include <SKSEMCP/SKSEMenuFramework.hpp>

#include <vector>

namespace
{
	// one row: slider (or checkbox) + Default button; a change is applied at once and saved when the control is released
	void RenderOption(const Settings::Option& a_option)
	{
		ImGuiMCP::PushID(a_option.label);
		float value = Settings::Get(a_option);
		bool  changed = false;
		bool  save = false;
		if (!a_option.format) {
			bool on = value != 0.0f;
			if (ImGuiMCP::Checkbox(a_option.label, &on)) {
				value = on ? 1.0f : 0.0f;
				changed = save = true;
			}
		} else {
			if (ImGuiMCP::SliderFloat(a_option.label, &value, a_option.min, a_option.max, a_option.format)) {
				changed = true;
			}
			save = ImGuiMCP::IsItemDeactivatedAfterEdit();
		}
		if (a_option.help && *a_option.help) {
			ImGuiMCP::SetItemTooltip("%s", a_option.help);
		}
		ImGuiMCP::SameLine();
		if (ImGuiMCP::Button("Default")) {
			value = a_option.defaultValue;
			changed = save = true;
		}
		if (changed) {
			Settings::Set(a_option, value);
		}
		if (save) {
			Settings::Save(a_option);
		}
		ImGuiMCP::PopID();
	}

	void RenderMode(int a_mode, const char* a_title)
	{
		if (!ImGuiMCP::CollapsingHeader(a_title)) {
			return;
		}
		for (const auto& option : Settings::Options()) {
			if (option.mode == a_mode) {
				RenderOption(option);
			}
		}
	}

	void __stdcall RenderSettings()
	{
		ImGuiMCP::TextWrapped("%s", "Changes apply at once and are saved to OmecaOneDanteSDT.ini. Effect bonuses (attack speed, movement, "
									"stamina, health regen, armor, resist, shout cooldown) apply at the next transformation. Hover a "
									"setting for details.");
		RenderMode(0, "Devil Trigger");
		RenderMode(1, "Sin Devil Trigger");
	}
}

namespace Menu
{
	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			REX::INFO("menu: SKSE Menu Framework not installed; no settings page");
			return;
		}
		SKSEMenuFramework::SetSection("Dante's Devil Trigger");
		SKSEMenuFramework::AddSectionItem("Settings", RenderSettings);
		REX::INFO("menu: settings page registered");
	}
}
