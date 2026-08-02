#include "PCH.h"
#include "Settings.h"
#include "FOVManager.h"
#include "EventHandlers.h"
#include "Menu.h"

namespace Plugin
{
	static constexpr auto NAME    = "FOVSliderF4SE"sv;
	static constexpr auto VERSION = REL::Version{ 1, 0, 0 };
}

namespace
{
	// Re-apply the spdlog level based on the loaded settings. Called
	// after Settings::Load so the user can flip verbosity in the INI
	// without needing a debug build.
	void ApplyLogLevelFromSettings()
	{
		const bool verbose = FOVSlider::Settings::GetSingleton()->verboseLogging.load();
		const auto lvl     = verbose ? spdlog::level::trace : spdlog::level::info;
		spdlog::set_level(lvl);
		spdlog::flush_on(lvl);
		logger::info("[FOVSlider] Log level set to {} ({})",
			verbose ? "trace" : "info",
			verbose ? "bVerboseLogging=true" : "bVerboseLogging=false");
	}

	// ============================================================
	// F4SE messaging callback
	// ============================================================
	void MessageCallback(F4SE::MessagingInterface::Message* msg)
	{
		if (!msg) return;

		switch (msg->type) {
		case F4SE::MessagingInterface::kPostLoad:
			// Every plugin has returned from F4SEPlugin_Load at this point.
			// Registering here makes Menu Framework discovery independent of
			// the DLL ordering in plugins.txt.
			FOVSlider::Menu::Register();
			break;

		case F4SE::MessagingInterface::kPreLoadGame:
			// Before the engine starts reading the save: pin the INI
			// values now so every load-pipeline read-back already sees
			// the user's settings (no post-load correction needed).
			FOVSlider::FOVManager::GetSingleton()->OnPreLoadGame();
			break;

		case F4SE::MessagingInterface::kGameDataReady:
			logger::info("[FOVSlider] kGameDataReady - initializing");
			FOVSlider::Settings::GetSingleton()->Load();
			ApplyLogLevelFromSettings();
			// Force a Save now: it carries the disk-INI syncs
			// (Fallout4Custom.ini + Fallout4.ini) so the files the
			// engine's load-time restore reads already hold the user's
			// values for this and every future session.
			FOVSlider::Settings::GetSingleton()->Save();
			FOVSlider::FOVManager::GetSingleton()->Init();
			FOVSlider::RegisterEventSinks();
			FOVSlider::FOVManager::GetSingleton()->LogEngineSnapshot("kGameDataReady/before-apply");
			// Apply initial state. The world camera FOV won't have been
			// fully initialized yet on cold-boot, but the apply will land
			// once kPostLoadGame / kNewGame retries kick in.
			FOVSlider::FOVManager::GetSingleton()->ApplyAllSettings();
			break;

		case F4SE::MessagingInterface::kNewGame:
			logger::info("[FOVSlider] kNewGame - resetting session load flag and applying settings");
			FOVSlider::Settings::GetSingleton()->Load();
			ApplyLogLevelFromSettings();
			// Fresh session: reset the flag so ScheduleLoadRetry runs
			// the full initial-apply sequence, just like the first load.
			FOVSlider::FOVManager::GetSingleton()->initialLoadApplied.store(false);
			FOVSlider::FOVManager::GetSingleton()->LogEngineSnapshot("NewGame/before");
			FOVSlider::OnGameLoaded();
			FOVSlider::FOVManager::GetSingleton()->ScheduleLoadRetry();
			break;

		case F4SE::MessagingInterface::kPostLoadGame:
			FOVSlider::Settings::GetSingleton()->Load();
			ApplyLogLevelFromSettings();
			FOVSlider::OnGameLoaded();
			if (!FOVSlider::FOVManager::GetSingleton()->initialLoadApplied.load()) {
				// First load in this session: full retry sequence to defeat
				// the engine's late camera initialization window.
				logger::info("[FOVSlider] kPostLoadGame - initial load, running ScheduleLoadRetry");
				FOVSlider::FOVManager::GetSingleton()->LogEngineSnapshot("PostLoadGame-initial/before");
				FOVSlider::FOVManager::GetSingleton()->ScheduleLoadRetry();
			} else {
				// Subsequent load in the same session: hard-set values
				// while the load screen still covers the frame (instant
				// and invisible), then let the drift watcher's hot mode
				// smooth out anything the engine writes after the fade.
				// No phase-2 retry lerps - those caused visible pops on
				// quick in-session loads before covered-window gating
				// existed.
				logger::info("[FOVSlider] kPostLoadGame - subsequent load (session), covered reassert + drift hot mode");
				FOVSlider::FOVManager::GetSingleton()->ScheduleCoveredReassert();
				FOVSlider::FOVManager::GetSingleton()->TriggerDriftHotMode(3500);
			}
			break;

		case F4SE::MessagingInterface::kPostPostLoad:
		{
			auto* mgr = FOVSlider::FOVManager::GetSingleton();

			// Other plugins are now loaded - safe to log dependency status.
			// The FP gunplay plugin registered as "FPInertia" before its
			// rename; check the current name first, then the legacy one, so
			// the WBFOV coordination (FSRF/FSLK) works with either build.
			const F4SE::PluginInfo* info = F4SE::GetPluginInfo("FPGunplayOverhaul");
			const char* fpName = info ? "FPGunplayOverhaul" : nullptr;
			if (!info) {
				info   = F4SE::GetPluginInfo("FPInertia");
				fpName = info ? "FPInertia" : nullptr;
			}

			if (info) {
				logger::info("[FOVSlider] {} v{} detected — coordinating camera/vm applies on load transitions",
					fpName, info->version);
				mgr->fpPluginName = fpName;
				mgr->fpInertiaPresent.store(true);
			} else {
				logger::info("[FOVSlider] FPGunplayOverhaul/FPInertia not detected - viewmodel FOV is owned solely by this plugin");
				mgr->fpPluginName.clear();
				mgr->fpInertiaPresent.store(false);
			}
			break;
		}

		default:
			break;
		}
	}
}

// ============================================================
// F4SE Plugin Query
// ============================================================
extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name        = Plugin::NAME.data();
	a_info->version     = 1;

	if (a_f4se->IsEditor()) {
		return false;
	}

	const auto ver = a_f4se->RuntimeVersion();
	if (ver < F4SE::RUNTIME_1_10_163) {
		return false;
	}
	return true;
}

// ============================================================
// F4SE Plugin Load
// ============================================================
extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se, {
		.log = true,
		.logName = Plugin::NAME.data(),
		.trampoline = false,
	});

	logger::info("{} v{}.{}.{} loading", Plugin::NAME,
	             Plugin::VERSION[0], Plugin::VERSION[1], Plugin::VERSION[2]);
	logger::info(
		"Runtime {} selected ({})",
		a_f4se->RuntimeVersion().string(),
		REX::FModule::IsRuntimeOG() ? "OG" :
			REX::FModule::IsRuntimeNG() ? "NG" : "AE");

	auto* messaging = F4SE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(MessageCallback)) {
		logger::critical("[FOVSlider] Failed to register messaging listener");
		return false;
	}

	logger::info("[FOVSlider] Plugin loaded; waiting for kGameDataReady");
	return true;
}
