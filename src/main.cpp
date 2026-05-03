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
	// ============================================================
	// Logging setup
	// ============================================================
	void InitializeLogging()
	{
		auto path = F4SE::log::log_directory();
		if (!path) {
			F4SE::stl::report_and_fail("[FOVSlider] F4SE log_directory missing"sv);
		}
		*path /= std::format("{}.log"sv, Plugin::NAME);

		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log  = std::make_shared<spdlog::logger>("global", std::move(sink));
		// Start at info; the actual level is applied AFTER Settings::Load
		// so the user-configurable [Diagnostics] bVerboseLogging takes
		// effect on the very first session.
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		set_default_logger(std::move(log));
	}

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
		case F4SE::MessagingInterface::kGameDataReady:
			logger::info("[FOVSlider] kGameDataReady - initializing");
			FOVSlider::Settings::GetSingleton()->Load();
			ApplyLogLevelFromSettings();
			FOVSlider::FOVManager::GetSingleton()->Init();
			FOVSlider::RegisterEventSinks();
			FOVSlider::Menu::Register();
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
				// Subsequent load in the same session: FOV values are
				// already correct in runtime. The engine may reset a few
				// INI settings (fDefaultWorldFOV, fDefault3rdPersonAimFOV)
				// during the load transition, but the drift watcher will
				// snap them back within one hot-mode poll cycle. No lerp,
				// no `fov X Y`, no retry loop needed - this is what was
				// causing the visible pops on every in-session save load.
				logger::info("[FOVSlider] kPostLoadGame - subsequent load (session), skipping retry - drift watcher will correct INI");
				FOVSlider::FOVManager::GetSingleton()->TriggerDriftHotMode(3500);
			}
			break;

		case F4SE::MessagingInterface::kPostPostLoad:
			// Other plugins are now loaded - safe to log dependency status.
			if (auto info = F4SE::GetPluginInfo("FPInertia"); info.has_value()) {
				logger::info("[FOVSlider] FPInertia v{} detected - viewmodel FOV will hand off to its WBFOV when an entry exists; runtime PlayerCamera FOV ownership delegated to FPInertia",
					info->version);
				FOVSlider::FOVManager::GetSingleton()->fpInertiaPresent.store(true);
			} else {
				logger::info("[FOVSlider] FPInertia not detected - viewmodel FOV is owned solely by this plugin");
				FOVSlider::FOVManager::GetSingleton()->fpInertiaPresent.store(false);
			}
			break;

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
	if (ver < F4SE::RUNTIME_1_10_162) {
		return false;
	}
	return true;
}

// ============================================================
// F4SE Plugin Load
// ============================================================
extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
	InitializeLogging();
	logger::info("{} v{}.{}.{} loading", Plugin::NAME,
	             Plugin::VERSION[0], Plugin::VERSION[1], Plugin::VERSION[2]);

	F4SE::Init(a_f4se);

	auto* messaging = F4SE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(MessageCallback)) {
		logger::critical("[FOVSlider] Failed to register messaging listener");
		return false;
	}

	logger::info("[FOVSlider] Plugin loaded; waiting for kGameDataReady");
	return true;
}
