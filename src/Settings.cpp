#include "PCH.h"
#include "Settings.h"

namespace FOVSlider
{
	static constexpr const char* kSection_Display     = "Display";
	static constexpr const char* kSection_Interp      = "Interpolation";
	static constexpr const char* kSection_GameLoad    = "GameLoad";
	static constexpr const char* kSection_Diagnostics = "Diagnostics";

	std::filesystem::path Settings::GetIniPath() const
	{
		// Next to the DLL: Data\F4SE\Plugins\FOV Slider F4SE.ini
		return std::filesystem::current_path() / "Data" / "F4SE" / "Plugins" / "FOV Slider F4SE.ini";
	}

	bool Settings::Load()
	{
		// Track migration / first-write state outside the locked section
		// so we can call Save() (which takes the same lock) afterward
		// without deadlocking.
		bool needPersist = false;

		{
			std::lock_guard lock(ioMtx);

		const auto path = GetIniPath();

		CSimpleIniA ini;
		ini.SetUnicode();

		const bool exists = std::filesystem::exists(path);
		if (exists) {
			const SI_Error rc = ini.LoadFile(path.string().c_str());
			if (rc < 0) {
				logger::warn("[FOVSlider] Failed to parse '{}', falling back to defaults", path.string());
			}
		} else {
			logger::info("[FOVSlider] '{}' not found - writing defaults", path.string());
		}

		auto getF = [&](const char* section, const char* key, float def) {
			return static_cast<float>(ini.GetDoubleValue(section, key, def));
		};
		auto getI = [&](const char* section, const char* key, int def) {
			return static_cast<int>(ini.GetLongValue(section, key, def));
		};
		auto getB = [&](const char* section, const char* key, bool def) {
			return ini.GetBoolValue(section, key, def);
		};

		firstPersonFOV.store(         getF(kSection_Display, "fFirstPersonFOV",          80.0f));
		thirdPersonFOV.store(         getF(kSection_Display, "fThirdPersonFOV",          80.0f));
		viewmodelFOV.store(           getF(kSection_Display, "fViewmodelFOV",            80.0f));
		pipBoyFOV.store(              getF(kSection_Display, "fPipBoyFOV",               80.0f));
		terminalFOV.store(            getF(kSection_Display, "fTerminalFOV",             80.0f));
		cameraDistance.store(         getF(kSection_Display, "fCameraDistance",          15.0f));
		enableFirstPersonAimFOV.store(getB(kSection_Display, "bEnableFirstPersonAimFOV", false));
		firstPersonAimFOV.store(      getF(kSection_Display, "fFirstPersonAimFOV",       80.0f));
		thirdPersonAimFOV.store(      getF(kSection_Display, "fThirdPersonAimFOV",       50.0f));

		interpFrames.store(           getI(kSection_Interp,  "iFrames",     12));
		interpFramesFast.store(       getI(kSection_Interp,  "iFramesFast", 6));

		loadBurstDurationMs.store(    getI(kSection_GameLoad, "iLoadBurstDurationMs", 500));
		loadBurstStepMs.store(        getI(kSection_GameLoad, "iLoadBurstStepMs",     8));
		loadRetryCount.store(         getI(kSection_GameLoad, "iRetryCount",          6));
		loadRetryInterval.store(      getF(kSection_GameLoad, "fRetryIntervalSec",    0.5f));

		verboseLogging.store(         getB(kSection_Diagnostics, "bVerboseLogging",        true));
		logEveryEngineWrite.store(    getB(kSection_Diagnostics, "bLogEveryEngineWrite",   false));
		logEveryConsoleCommand.store( getB(kSection_Diagnostics, "bLogEveryConsoleCommand", true));
		driftWatchIntervalMs.store(    getI(kSection_Diagnostics, "iDriftWatchIntervalMs",   50));
		driftWatchHotIntervalMs.store( getI(kSection_Diagnostics, "iDriftWatchHotIntervalMs", 16));
		driftWatchHotDurationMs.store( getI(kSection_Diagnostics, "iDriftWatchHotDurationMs", 3500));
		driftAutoCorrect.store(        getB(kSection_Diagnostics, "bDriftAutoCorrect",       true));
		driftCorrectDurationMs.store(  getI(kSection_Diagnostics, "iDriftCorrectDurationMs", 250));

		// ---- Migrate stale aggressive values from earlier dev iterations ----
		// Prior versions of this plugin shipped with iDriftCorrectDurationMs=50
		// (way too fast - the lerp looked like a snap) and
		// iDriftWatchIntervalMs=250 (too slow to catch engine writes within
		// a frame). Both are user-modifiable on disk, so we only migrate
		// them when the value is "obviously the old default" - if the user
		// deliberately set 60 ms because they wanted fast corrections, we
		// leave it alone.
		if (driftCorrectDurationMs.load() == 50) {
			driftCorrectDurationMs.store(250);
			logger::info("[FOVSlider] Migrated iDriftCorrectDurationMs 50 -> 250 (smoother lerp)");
			needPersist = true;
		}
		if (driftWatchIntervalMs.load() == 250) {
			driftWatchIntervalMs.store(50);
			logger::info("[FOVSlider] Migrated iDriftWatchIntervalMs 250 -> 50 (faster cold poll)");
			needPersist = true;
		}

		// Flag a persist if the file didn't exist - guarantees a
		// well-formed file on disk for users to inspect.
		if (!exists) needPersist = true;

		logger::info("[FOVSlider] Loaded settings from '{}'", path.string());
		logger::info("[FOVSlider]  1stP={:.1f} 3rdP={:.1f} VM={:.1f} PB={:.1f} TM={:.1f} ND={:.2f}",
			firstPersonFOV.load(), thirdPersonFOV.load(), viewmodelFOV.load(),
			pipBoyFOV.load(), terminalFOV.load(), cameraDistance.load());
		logger::info("[FOVSlider]  AimEnabled={} 1stPAim={:.1f} 3rdPAim={:.1f}",
			enableFirstPersonAimFOV.load(),
			firstPersonAimFOV.load(), thirdPersonAimFOV.load());

		}  // end of locked section

		// Persist outside the lock to avoid deadlocking with Save().
		if (needPersist) {
			Save();
		}

		return true;
	}

	bool Settings::Save()
	{
		std::lock_guard lock(ioMtx);

		const auto path = GetIniPath();

		// Make sure the parent dir exists; on a clean install the user might
		// have only the bare F4SE\Plugins folder set up.
		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);

		CSimpleIniA ini;
		ini.SetUnicode();

		// Load existing first so we preserve user comments / unknown keys.
		if (std::filesystem::exists(path)) {
			ini.LoadFile(path.string().c_str());
		}

		auto setF = [&](const char* section, const char* key, float v) {
			ini.SetDoubleValue(section, key, static_cast<double>(v));
		};
		auto setI = [&](const char* section, const char* key, int v) {
			ini.SetLongValue(section, key, v);
		};
		auto setB = [&](const char* section, const char* key, bool v) {
			ini.SetBoolValue(section, key, v);
		};

		setF(kSection_Display, "fFirstPersonFOV",          firstPersonFOV.load());
		setF(kSection_Display, "fThirdPersonFOV",          thirdPersonFOV.load());
		setF(kSection_Display, "fViewmodelFOV",            viewmodelFOV.load());
		setF(kSection_Display, "fPipBoyFOV",               pipBoyFOV.load());
		setF(kSection_Display, "fTerminalFOV",             terminalFOV.load());
		setF(kSection_Display, "fCameraDistance",          cameraDistance.load());
		setB(kSection_Display, "bEnableFirstPersonAimFOV", enableFirstPersonAimFOV.load());
		setF(kSection_Display, "fFirstPersonAimFOV",       firstPersonAimFOV.load());
		setF(kSection_Display, "fThirdPersonAimFOV",       thirdPersonAimFOV.load());

		setI(kSection_Interp,  "iFrames",                  interpFrames.load());
		setI(kSection_Interp,  "iFramesFast",              interpFramesFast.load());

		setI(kSection_GameLoad, "iLoadBurstDurationMs", loadBurstDurationMs.load());
		setI(kSection_GameLoad, "iLoadBurstStepMs",     loadBurstStepMs.load());
		setI(kSection_GameLoad, "iRetryCount",          loadRetryCount.load());
		setF(kSection_GameLoad, "fRetryIntervalSec",    loadRetryInterval.load());

		setB(kSection_Diagnostics, "bVerboseLogging",         verboseLogging.load());
		setB(kSection_Diagnostics, "bLogEveryEngineWrite",    logEveryEngineWrite.load());
		setB(kSection_Diagnostics, "bLogEveryConsoleCommand", logEveryConsoleCommand.load());
		setI(kSection_Diagnostics, "iDriftWatchIntervalMs",      driftWatchIntervalMs.load());
		setI(kSection_Diagnostics, "iDriftWatchHotIntervalMs",   driftWatchHotIntervalMs.load());
		setI(kSection_Diagnostics, "iDriftWatchHotDurationMs",   driftWatchHotDurationMs.load());
		setB(kSection_Diagnostics, "bDriftAutoCorrect",          driftAutoCorrect.load());
		setI(kSection_Diagnostics, "iDriftCorrectDurationMs",    driftCorrectDurationMs.load());

		const SI_Error rc = ini.SaveFile(path.string().c_str());
		if (rc < 0) {
			logger::error("[FOVSlider] Failed to save '{}'", path.string());
			return false;
		}
		logger::trace("[FOVSlider] Saved settings to '{}'", path.string());
		return true;
	}
}
