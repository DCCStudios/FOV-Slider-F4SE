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
