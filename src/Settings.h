#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>

namespace FOVSlider
{
	// All user-tunable values live here as atomics so the menu thread,
	// the messaging thread, and the main game thread can read them
	// concurrently without locking.
	class Settings
	{
	public:
		static Settings* GetSingleton()
		{
			static Settings s;
			return &s;
		}

		// ---- Display values (degrees) ----
		std::atomic<float> firstPersonFOV{ 80.0f };
		std::atomic<float> thirdPersonFOV{ 80.0f };
		std::atomic<float> viewmodelFOV{ 80.0f };
		std::atomic<float> pipBoyFOV{ 80.0f };
		std::atomic<float> terminalFOV{ 80.0f };
		std::atomic<float> cameraDistance{ 15.0f };
		std::atomic<bool>  enableFirstPersonAimFOV{ false };
		std::atomic<float> firstPersonAimFOV{ 80.0f };
		std::atomic<float> thirdPersonAimFOV{ 50.0f };

		// ---- Interpolation tuning ----
		std::atomic<int>   interpFrames{ 12 };
		std::atomic<int>   interpFramesFast{ 6 };

		// ---- Game-load retry tuning ----
		// The engine re-applies the camera FOV during/after a save load,
		// clobbering anything we set too early. The plain "apply every
		// 500 ms" loop the original Papyrus mod used leaves a visible
		// FOV pop in the first ~100-300 ms post-load (the engine's own
		// camera re-init window).
		//
		// We solve this with a two-phase strategy:
		//
		//   Phase 1 (`iLoadBurstDurationMs`, `iLoadBurstStepMs`):
		//     Frame-paced burst — re-assert camera FOV every 8 ms
		//     (~1 frame at 120 fps) for the first 500 ms after load.
		//     This makes the engine's re-init invisible because OUR
		//     value is what the renderer reads on every frame.
		//
		//   Phase 2 (`iRetryCount`, `fRetryIntervalSec`):
		//     Slower safety net — N retries every 500 ms after the
		//     burst, catching delayed engine re-init passes (cell
		//     load triggers, scripted intros, etc.).
		std::atomic<int>   loadBurstDurationMs{ 500 };
		std::atomic<int>   loadBurstStepMs{ 8 };
		std::atomic<int>   loadRetryCount{ 6 };
		std::atomic<float> loadRetryInterval{ 0.5f };

		// ---- Diagnostics ----
		// `bVerboseLogging` controls the spdlog level at runtime. When
		// true (default), every state transition, console command, and
		// engine setting write is logged at info/trace level. Set to
		// false in production-quality runs once the FOV pop is gone.
		std::atomic<bool> verboseLogging{ true };

		// `bLogEveryEngineWrite` causes EVERY in-memory INI write
		// (SetEngineFloatSetting) to log a line. Useful for diagnosing
		// "who is clobbering my FOV?" - turn this on, reproduce the
		// drift, and grep the log for the exact write that introduced
		// the wrong value. Off by default because the load burst
		// alone can issue ~250 writes per game load.
		std::atomic<bool> logEveryEngineWrite{ false };

		// `bLogEveryConsoleCommand` logs every `fov X Y` we issue.
		// Cheap; on by default. Disable only if you're chasing a
		// non-FOV-related bug and the noise is in the way.
		std::atomic<bool> logEveryConsoleCommand{ true };

		// `iDriftWatchIntervalMs` runs a background thread that polls
		// `fDefault1stPersonFOV:Display` every N ms. It emits a WARN
		// log line whenever the engine value differs from our saved
		// value by more than 0.5 deg, AND - if `bDriftAutoCorrect` is
		// true (default) - smoothly lerps the engine value back to
		// pull it back. This is what defeats the engine's
		// late post-load FOV re-init (which fires 2-4 seconds after
		// kPostLoadGame, well after our scheduled retries end) and
		// any other external agent that overwrites the FOV at runtime.
		// Set to 0 to disable polling entirely.
		//
		// Default 50 ms = 20 polls/sec; gives ~3-4 frame detection at
		// 60 fps. Each poll is one INI-collection lookup + one float
		// compare, well under 1 us, so the CPU cost is negligible.
		std::atomic<int> driftWatchIntervalMs{ 50 };

		// `iDriftWatchHotIntervalMs` - tighter polling interval used
		// for `iDriftWatchHotDurationMs` ms after specific menu-close
		// events (LoadingMenu, FaderMenu, ExamineMenu) where we know
		// the engine is about to write its default FOVs. Default 16 ms
		// = 1 frame at 60 fps so we catch the engine's stray writes
		// within a single frame. Outside the hot window the watcher
		// reverts to `iDriftWatchIntervalMs`.
		std::atomic<int> driftWatchHotIntervalMs{ 16 };

		// `iDriftWatchHotDurationMs` - how long the hot-poll mode stays
		// engaged after a trigger event (in ms). The engine can do FOV
		// restores up to ~2 seconds AFTER LoadingMenu/FaderMenu close,
		// and up to ~1.7 seconds after ExamineMenu close (workbench
		// teardown), so 3500 ms is a safe upper bound for all observed
		// cases. Each trigger event resets the hot deadline to
		// `now + this duration`, so back-to-back triggers (e.g.
		// LoadingMenu then FaderMenu) keep the hot window alive.
		std::atomic<int> driftWatchHotDurationMs{ 3500 };

		// `bDriftAutoCorrect` - when the drift watcher detects drift
		// (and we are NOT in Aiming context, where the camera FOV is
		// intentionally different), smoothly lerp the engine values
		// back to our saved targets. Covers all three drift modes the
		// diagnostic log identified (engine post-load FOV restore,
		// savegame state restore, and any other mod's stray write).
		// On by default.
		std::atomic<bool> driftAutoCorrect{ true };

		// `iDriftCorrectDurationMs` - how long the auto-correct lerp
		// takes (in ms). Same lerp shape as the load burst, just on a
		// shorter timeline since the player is in active gameplay.
		// Default 250 = 1/4 second; smooth enough to be invisible at
		// 60 fps, fast enough that the user doesn't notice the FOV
		// shifting.
		std::atomic<int> driftCorrectDurationMs{ 250 };

		// ---- IO ----
		// Returns the resolved INI path (next to the DLL).
		std::filesystem::path GetIniPath() const;

		// Load from INI; missing file => write defaults.
		bool Load();

		// Save current atomic state back to INI.
		bool Save();

	private:
		Settings() = default;
		Settings(const Settings&)            = delete;
		Settings& operator=(const Settings&) = delete;

		// Serializes concurrent Save() calls so we don't half-write the file.
		mutable std::mutex ioMtx;
	};
}
