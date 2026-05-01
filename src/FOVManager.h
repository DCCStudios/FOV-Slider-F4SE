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

		// Apply ALL settings from scratch INSTANTLY. Used by the menu
		// "Re-apply All" button and by callers that need an immediate
		// hard-set (e.g. the very first kGameDataReady apply, before
		// the player can see anything).
		void ApplyAllSettings();

		// Smoothly lerp the engine's camera FOV settings from their
		// current values to the saved targets over `durationMs` ms,
		// stepping every `stepMs` ms (default 8 = ~1 frame at 120 fps).
		//
		// Settings that don't visibly transition (3rd-person aim FOV,
		// near distance) are applied instantly at the start.
		// Settings that DO transition (1st-person FOV, 3rd-person
		// world FOV) are lerped per-step (writes both INI and runtime).
		//
		// `a_includeViewmodel` controls the final stage:
		//   - true  (Phase 1 game-load, manual "Smooth Re-apply"):
		//       Issues `fov X Y` viewmodel apply at the end and
		//       dispatches FSRF to FPInertia so its WBFOV defaults
		//       refresh.
		//   - false (Phase 2 game-load retries, anywhere we just want
		//       to make sure the camera/INI is at target):
		//       Skips both. Avoids re-triggering FPInertia's WBFOV
		//       apply, which clobbers `PlayerCamera::firstPersonFOV`
		//       via the engine's `fov X Y` quirk (X overwrites BOTH
		//       camera fields regardless of Y) and would otherwise
		//       create a 30 deg flash + 250 ms drift-correct ramp on
		//       every Phase 2 retry.
		//
		// NOTE: pure-runtime drift correction does NOT call this
		// function at all - use SmoothCorrectRuntimeFOV() for that
		// path. This function still writes INI and is fine for any
		// caller that legitimately wants the engine source-of-truth
		// pulled to target.
		//
		// Cancels any in-flight lerp via the interpGeneration counter.
		// Spawns a detached worker thread; returns immediately.
		void LerpAllSettings(int durationMs, int stepMs = 8, bool a_includeViewmodel = true);

		// Schedule a deferred re-apply of all settings to defeat the
		// engine's late camera initialization on game load. Spawns a
		// detached worker that runs the smooth load-lerp followed by
		// slower safety retries (each retry is also a smooth lerp).
		void ScheduleLoadRetry();

		// Engage the drift watcher's "hot poll" mode for `durationMs`
		// ms (clamped to at least 100 ms). During the hot window, the
		// watcher polls every `iDriftWatchHotIntervalMs` (default 16 ms,
		// = 1 frame at 60 fps) instead of `iDriftWatchIntervalMs`
		// (default 50 ms), so engine stray-writes get detected and
		// corrected within a single frame.
		//
		// Called by event handlers on menus that are known to trigger
		// engine FOV restores - LoadingMenu close, FaderMenu close,
		// ExamineMenu close (workbench teardown). Multiple calls are
		// idempotent in that the deadline is set to MAX(existing,
		// now + durationMs), so back-to-back triggers (e.g.
		// LoadingMenu → FaderMenu) keep the hot window alive without
		// shortening it.
		void TriggerDriftHotMode(int durationMs);

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

		// ---- Debug introspection ----
		// Last X/Y arguments we passed to a `fov X Y` console command.
		// -1.0f means "never applied since plugin init / last reset".
		// Used by the debug popout window so the user can spot a stale
		// runtime camera FOV (engine setting vs runtime mismatch).
		float GetLastAppliedViewmodel() const { return lastAppliedViewmodel.load(); }
		float GetLastAppliedCamera() const    { return lastAppliedCamera.load(); }

		// Read a live engine float setting. Wraps the same code path
		// the apply primitives use, exposed so the debug window can
		// poll without duplicating the collection-walk.
		static bool ReadEngineFloatSetting(const char* a_key, float& a_out)
		{
			return TryReadEngineFloatSetting(a_key, a_out);
		}

		// Dump a snapshot of all four FOV-related engine settings to the
		// log along with our saved values, prefixed by `a_phase`. Use
		// this at every interesting boundary (game-load entry/exit,
		// every context transition entry/exit, every animation event)
		// so you can grep the log and reconstruct the timeline.
		void LogEngineSnapshot(const char* a_phase) const;

	private:
		FOVManager() = default;

		// ---- Atomic primitives ----
		// Sets a float setting in the engine's live INI / GameSetting
		// collections. This is the engine-internal equivalent of Papyrus
		// Utility::SetINIFloat - it updates the in-memory value that the
		// engine itself reads every frame, so changes apply instantly with
		// no INI disk write or restart needed.
		//
		// `a_caller` tags the call site so the diagnostics log can
		// pinpoint who clobbered the value. Pass a short literal like
		// "ApplyFirstPersonFOV" or "BurstStep".
		static bool SetEngineFloatSetting(const char* a_key, float a_value,
		                                  const char* a_caller = "?");

		// Read a float setting from any of the engine's INI/GameSetting
		// collections. Returns false if the key is absent.
		static bool TryReadEngineFloatSetting(const char* a_key, float& a_out);

		// Run a console command via the engine's ScriptCompiler. Same
		// pipeline Papyrus mods use through ConsoleUtilF4 - the only
		// reliable way to set just the viewmodel FOV via `fov X Y` without
		// touching the world camera FOV. `a_caller` tags the call site
		// for the diagnostics log.
		static bool ExecuteConsoleCommand(std::string_view a_cmd,
		                                  const char* a_caller = "?");

		// Write the runtime camera FOV fields on PlayerCamera directly.
		// These (offsets 0x168 / 0x16C) are the values the renderer reads
		// every frame - the engine copies INI -> runtime on certain
		// transitions, but otherwise the runtime stays at whatever was
		// last set via `fov X Y` or a direct write.
		//
		// Returns false if PlayerCamera isn't available (very early init,
		// MainMenu before world is ready). The two arguments are
		// optional in the sense that NaN means "don't touch this field".
		//
		// Why this matters: a smooth lerp on the INI value is INVISIBLE
		// because the renderer doesn't read INI per frame. Calling this
		// from each lerp step makes the lerp actually visible, and also
		// front-runs any engine "restore from save" write since the
		// next step (8 ms later) will overwrite it.
		static bool WriteRuntimeCameraFOV(float a_firstPersonFOV,
		                                  float a_worldFOV,
		                                  const char* a_caller = "?");

		// Read the runtime camera FOV fields. Same caveats as Write -
		// returns false if PlayerCamera is null.
		static bool ReadRuntimeCameraFOV(float& a_firstPersonFOV,
		                                 float& a_worldFOV);

		// Smoothly corrects PlayerCamera::firstPersonFOV/worldFOV to
		// the saved targets without touching INI, viewmodel, or
		// FPInertia. Used by the drift watcher when something else
		// (FPInertia's `fov X Y` re-apply, an engine restore on load,
		// etc.) clobbered the runtime camera fields. We can't go
		// through the full apply chain because issuing our own
		// `fov X Y` here would re-trigger FPInertia's WBFOV apply and
		// create the feedback loop the user reported as "constant
		// fov lerping and popping". Cancels in-flight lerps via
		// interpGeneration. Spawns a detached thread; returns
		// immediately.
		void SmoothCorrectRuntimeFOV(int durationMs, int stepMs = 8);

		// Background thread spawned on Init() (when iDriftWatchIntervalMs
		// > 0). Polls the engine's `fDefault1stPersonFOV:Display` and
		// emits a WARN log line whenever it drifts >0.5 deg from our
		// saved firstPersonFOV value. The canonical "who is writing 90
		// behind my back" detector.
		void RunDriftWatcher();

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

		// Drift watcher control. We launch the thread once on Init();
		// `driftWatcherStop` lets it exit cleanly on plugin teardown
		// (not currently called - F4SE plugins don't have a teardown
		// hook - but the variable is here for future use).
		std::atomic<bool>  driftWatcherStarted{ false };
		std::atomic<bool>  driftWatcherStop{ false };

		// Counter incremented on entry to LerpAllSettings and
		// decremented on exit. The drift watcher checks this and
		// skips drift detection while a lerp is running, otherwise
		// the lerp's own mid-transition writes (e.g. 92 deg while
		// lerping 90 -> 105) would look like drift and trigger a
		// recursive auto-correct.
		std::atomic<int>   activeLerps{ 0 };

		// Steady-clock deadline (in ms-since-epoch) for the drift
		// watcher's hot-poll mode. The watcher uses
		// `iDriftWatchHotIntervalMs` while now < deadline,
		// `iDriftWatchIntervalMs` otherwise. 0 = hot mode never
		// engaged (cold polling only).
		//
		// Stored as int64_t (ms count) rather than time_point because
		// std::atomic<time_point> requires extra trait checks and the
		// ms representation is sufficient for our 16 ms+ resolution.
		std::atomic<std::int64_t> driftHotDeadlineMs{ 0 };
	};
}
