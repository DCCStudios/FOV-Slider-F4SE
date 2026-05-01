#pragma once

#include "F4SEMenuFramework.h"

namespace FOVSlider
{
	namespace Menu
	{
		// Register UI with F4SE Menu Framework. Must be called after
		// kGameDataReady (the framework's DLL is checked at runtime).
		void Register();

		// Render callback wired into the F4SE Menu Framework section.
		void __stdcall Render();
	}
}
