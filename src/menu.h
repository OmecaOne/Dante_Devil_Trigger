#pragma once

// SKSE Menu Framework page "Dante's Devil Trigger" (optional: without the framework nothing is registered).
// Every INI setting of both forms (settings.cpp, Settings::Options): slider or checkbox + Default button per setting, applied at
// once and written to OmecaOneDanteSDT.ini when the control is released.

namespace Menu
{
	// at data load, after the INI was read
	void Register();
}
