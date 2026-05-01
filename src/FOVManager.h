#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

namespace FOVSlider
{
	// Tracks the current "context" that owns the viewmodel + camera FOV.
	// When the player closes the Pip-Boy / leaves a terminal / exits ADS,
	// we pop back to the appropriate context to derive the correct FOV.
	enum class FOVContext : std::uint8_t
	{
		Default = 0,    // ordinary gameplay - viewmodel = ViewmodelFOV (or WBFOV override from FPInertia)
		PipBoy,         // PipBoy menu open
		Terminal,       // Terminal furniture / camera override active
		Aiming          // First-person ADS (sightedStateEnter)
	};

	class FOVManager
	{
	public:
		static FOVManager* GetSingleton()
		{
			static FOVManager s;
			return &s;
		}

		// Called once at plugin init (before kPostLoadGame fires).
		void Init();

		// Apply ALL settings from scratch. Used on game load and when the
		// user toggles a global option.
		void ApplyAllSettings();

		// Schedule a deferred re-apply of all settings to defeat the
		// engine's late camera initialization on game load. Spawns a
		// detached worker that calls ApplyAllSettings() N times spaced
		// out by the configured retry interval.
		void ScheduleLoadRetry();

		// ---- Setting-change callbacks (one per slider) ----
		// These are how the menu UI informs the manager that the user moved
		// a slider. Each updates Settings + applies live.
		void OnFirstPersonFOVChanged(float v);
		void OnThirdPersonFOVChanged(float v);
		void OnViewmodelFOVChanged(float v);
		void OnPipBoyFOVChanged(float v);
		void OnTerminalFOVChanged(float v);
		void OnCameraDistanceChanged(float v);
		void OnEnableFirstPersonAimFOVChanged(bool v);
		void OnFirstPersonAimFOVChanged(float v);
		void OnThirdPersonAimFOVChanged(float v);

		// ---- Context transitions (driven by event handlers) ----
		void OnPipBoyOpening();
		void OnPipBoyClosing();
		void OnTerminalEntered();    // player sat at terminal furniture
		void OnTerminalExited();     // OnGetUp from terminal
		void OnSightedStateEnter();
		void OnSightedStateExit();

		// ---- Queries ----
		FOVContext GetContext() const { return context.load(); }

		// Compute the viewmodel FOV that *should* be active right now,
		// considering both our context and the user's setting. We do NOT
		// query FPInertia's WBFOV here - FPInertia does the per-weapon
		// override on top of whatever we set, so we always return the
		// appropriate "default" for the context.
		float GetTargetViewmodelFOV() const;

		// Compute the camera (1st-person world) FOV that should be active.
		float GetTargetCameraFOV() const;

	private:
		FOVManager() = default;

		// ---- Atomic primitives ----
		// Sets a float setting in the engine's live INI / GameSetting
		// collections. This is the engine-internal equivalent of Papyrus
		// Utility::SetINIFloat - it updates the in-memory value that the
		// engine itself reads every frame, so changes apply instantly with
		// no INI disk write or restart needed.
		static bool SetEngineFloatSetting(const char* a_key, float a_value);

		// Read a float setting from any of the engine's INI/GameSetting
		// collections. Returns false if the key is absent.
		static bool TryReadEngineFloatSetting(const char* a_key, float& a_out);

		// Run a console command via the engine's ScriptCompiler. Same
		// pipeline Papyrus mods use through ConsoleUtilF4 - the only
		// reliable way to set just the viewmodel FOV via `fov X Y` without
		// touching the world camera FOV.
		static bool ExecuteConsoleCommand(std::string_view a_cmd);

		// Apply primitives - safe to call from main thread.
		void ApplyFirstPersonFOV(float fov);
		void ApplyThirdPersonFOV(float fov);
		void ApplyThirdPersonAimFOV(float fov);
		void ApplyCameraDistance(float distance);

		// Apply the viewmodel FOV. X = viewmodel, Y = current camera FOV
		// (derived from settings, never read from the live PlayerCamera
		// state, to match the FPInertia approach and avoid camera drift).
		void ApplyViewmodelFOV(float vmFov);

		// Drive a smooth transition of the viewmodel FOV from `from` -> `to`
		// using the configured frame count. Spawns a short-lived detached
		// thread that issues 1ms-spaced `fov X Y` commands. We always set
		// the FINAL value as a normal apply too, so even if the lerp gets
		// pre-empted we end up at the right place.
		void InterpolateViewmodelFOV(float from, float to, int frames);

		// Notify FPInertia that our defaults changed so it re-reads them.
		// Uses the F4SE messaging interface; harmless if FPInertia is
		// absent.
		void NotifyFPInertia();

		// Tell FPInertia to pause / resume its WBFOV applies. Called from
		// context transitions so FPInertia stops fighting our Pip-Boy /
		// Terminal / Aiming overrides.
		void NotifyFPInertiaLock(bool locked);

		// Same as NotifyFPInertiaLock(false), but delayed by `ms`
		// milliseconds (spawned on a worker thread). Used when handing
		// control back to FPInertia after a smooth viewmodel-FOV lerp:
		// we wait until the lerp settles so FPInertia's per-weapon
		// override doesn't fight the in-flight transition.
		void ScheduleFPInertiaUnlock(int ms);

		// ---- State ----
		std::atomic<FOVContext> context{ FOVContext::Default };

		// Lock held while spawning interpolation threads so two rapid
		// transitions don't fight each other.
		std::mutex transitionMtx;

		// Generation counter for canceling in-flight interpolations when a
		// new transition is requested before the old one finishes.
		std::atomic<std::uint64_t> interpGeneration{ 0 };

		// Last-applied FOV cache - prevents redundant `fov X Y` commands
		// when nothing changed.
		std::atomic<float> lastAppliedViewmodel{ -1.0f };
		std::atomic<float> lastAppliedCamera{ -1.0f };
	};
}
