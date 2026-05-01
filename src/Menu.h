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

		// ============================================================
		// Debug popout window
		// Non-pausing floating window that shows live FOV state side
		// by side with our saved settings. Toggled from the main FOV
		// Settings page. Stays visible during normal gameplay so the
		// user can watch the values change as they enter/leave Pip-Boy,
		// terminals, ADS, furniture, etc.
		// ============================================================
		namespace DebugWindow
		{
			// Framework-owned window handle. We manipulate IsOpen to
			// toggle visibility from the main settings checkbox.
			// Initialized in Menu::Register().
			inline MENU_WINDOW Handle{ nullptr };

			void __stdcall Render();
		}
	}
}
