#include "PCH.h"
#include "EventHandlers.h"
#include "FOVManager.h"
#include "Helpers.h"
#include "Settings.h"

namespace FOVSlider
{
	// ============================================================
	// MenuSink - PipBoy / Terminal open/close
	// ============================================================
	void MenuSink::Register()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			logger::warn("[FOVSlider] UI singleton unavailable - menu sink not registered");
			return;
		}
		// UI::RegisterSink<T>() forwards to GetEventSource<T>()->RegisterSink(sink).
		// BSTEventSource::RegisterSink is idempotent if the sink is already
		// registered (de-dupes internally), so calling on every game load is safe.
		ui->RegisterSink<RE::MenuOpenCloseEvent>(this);
		logger::info("[FOVSlider] Registered MenuOpenCloseEvent sink");
	}

	RE::BSEventNotifyControl MenuSink::ProcessEvent(
		const RE::MenuOpenCloseEvent&                a_event,
		RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
	{
		const auto& name = a_event.menuName;
		const bool  open = a_event.opening;

		// BSFixedString interns; compare to interned constants.
		static const RE::BSFixedString kPipBoy      = "PipboyMenu";
		static const RE::BSFixedString kTerminal    = "TerminalMenu";
		static const RE::BSFixedString kLoading     = "LoadingMenu";
		static const RE::BSFixedString kFader       = "FaderMenu";
		static const RE::BSFixedString kExamine     = "ExamineMenu";

		// Trace EVERY menu event so we can see what fires around any
		// reported FOV drift. Most are ignored (we only act on PipBoy
		// and TerminalMenu) but the user-facing diagnostics need to
		// show what we DID see.
		logger::trace("[FOVSlider] MenuOpenCloseEvent name='{}' opening={}",
			name.c_str() ? name.c_str() : "(null)", open);

		auto* fov      = FOVManager::GetSingleton();
		auto* settings = Settings::GetSingleton();

		if (name == kPipBoy) {
			if (open) {
				fov->OnPipBoyOpening();
			} else {
				fov->OnPipBoyClosing();
			}
		} else if (name == kTerminal) {
			// TerminalMenu close is a reliable signal. The OPEN side is
			// already handled via the CameraOverrideStart animation event
			// (AnimSink) which fires before the menu is on-screen.
			//
			// We pop context only if we're still in Terminal mode. The
			// original Papyrus mod's bug: Esc-to-close didn't reset the
			// FOV. Catching the menu-close here makes recovery reliable.
			if (!open && fov->GetContext() == FOVContext::Terminal) {
				fov->OnTerminalExited();
			}
		} else if (!open) {
			// ---- Engine-restore trigger menus ----
			// The diagnostic log identified three menus whose close
			// causes the engine to write its own default FOVs at
			// some point afterward. We engage the drift watcher's
			// hot-poll mode right when these close so any stray
			// engine write gets caught within ~16 ms instead of the
			// cold 50 ms cadence.
			//
			// Why we don't rewrite our values directly here: the
			// engine's restore happens 0.5 - 2 seconds AFTER the
			// menu-close, with the timing depending on system load.
			// A fixed-delay write would race the engine. The hot-poll
			// watcher reacts to whatever the engine ACTUALLY writes,
			// whenever it writes it.
			if (name == kLoading) {
				logger::info("[FOVSlider] LoadingMenu closed - engaging drift hot mode");
				fov->TriggerDriftHotMode(settings->driftWatchHotDurationMs.load());
			} else if (name == kFader) {
				// Fader fires both as a load-screen fade-out AND as
				// a generic UI fader, so the hot window we engage
				// here is the same as Loading - back-to-back triggers
				// just extend the deadline.
				logger::info("[FOVSlider] FaderMenu closed - engaging drift hot mode");
				fov->TriggerDriftHotMode(settings->driftWatchHotDurationMs.load());
			} else if (name == kExamine) {
				// ExamineMenu = workbenches, chem stations, cooking
				// stations, etc. The engine's camera-override teardown
				// for these writes default FOVs ~1.7 s after the menu
				// closes, which is why hot mode needs to last that
				// long (>= ~3 s).
				logger::info("[FOVSlider] ExamineMenu closed - engaging drift hot mode");
				fov->TriggerDriftHotMode(settings->driftWatchHotDurationMs.load());
			}
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	// ============================================================
	// AnimSink - sighted state + camera override (terminal furniture)
	// ============================================================
	bool AnimSink::Register()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) return false;

		RE::BSScrapArray<RE::BSTEventSource<RE::BSAnimationGraphEvent>*> sources;
		if (!RE::BGSAnimationSystemUtils::GetEventSourcePointersFromGraph(player, sources)) {
			return false;
		}
		if (sources.empty()) return false;

		for (auto* src : sources) {
			if (src) src->RegisterSink(this);
		}
		registered.store(true);
		logger::info("[FOVSlider] Registered animation event sink ({} sources)", sources.size());
		return true;
	}

	void AnimSink::Unregister()
	{
		if (!registered.load()) return;
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) return;

		RE::BSScrapArray<RE::BSTEventSource<RE::BSAnimationGraphEvent>*> sources;
		if (RE::BGSAnimationSystemUtils::GetEventSourcePointersFromGraph(player, sources)) {
			for (auto* src : sources) {
				if (src) src->UnregisterSink(this);
			}
		}
		registered.store(false);
	}

	RE::BSEventNotifyControl AnimSink::ProcessEvent(
		const RE::BSAnimationGraphEvent&                a_event,
		RE::BSTEventSource<RE::BSAnimationGraphEvent>*)
	{
		const auto& evt = a_event.animEvent;
		auto* fov = FOVManager::GetSingleton();

		// Sighted-state hooks for the optional First-Person Aim FOV feature.
		// The engine emits these in both casings depending on the .hkx
		// annotation, so we accept both.
		if (evt == "sightedStateEnter" || evt == "SightedStateEnter") {
			logger::info("[FOVSlider] anim event '{}'", evt.c_str());
			fov->OnSightedStateEnter();
			return RE::BSEventNotifyControl::kContinue;
		}
		if (evt == "sightedStateExit" || evt == "SightedStateExit") {
			logger::info("[FOVSlider] anim event '{}'", evt.c_str());
			fov->OnSightedStateExit();
			return RE::BSEventNotifyControl::kContinue;
		}

		// Terminal entry: the player triggers `CameraOverrideStart` when
		// the camera-override begins for furniture they're sitting on.
		// We then check whether the furniture is a Terminal.
		//
		// Note: non-terminal furniture (workbenches, crafting stations,
		// power-armor stations) is intentionally NOT handled here. The
		// 3rd-person FOV clobber that happens when FPInertia issues
		// `fov X Y` while the player is in a 3rd-person furniture view
		// is a self-policing concern that lives inside FPInertia's
		// WeaponFOV::Update loop (it checks the active PlayerCamera
		// state and skips its apply when not in 1st-person / iron-
		// sights). Keeping that logic in FPInertia means it works for
		// users who run FPInertia without this plugin too.
		if (evt == "CameraOverrideStart" || evt == "cameraOverrideStart") {
			// `currentFurniture` lives on AIProcess->MiddleHighProcessData,
			// not directly on Actor; the helper does the null-check chain.
			auto* furn = GetPlayerCurrentFurniture();
			std::uint32_t furnFormID  = furn ? furn->formID : 0u;
			std::uint32_t baseFormID  = 0u;
			std::uint32_t baseTypeID  = 0u;
			bool          isTerminal  = false;

			if (furn) {
				if (auto* base = furn->GetObjectReference()) {
					baseFormID = base->formID;
					baseTypeID = static_cast<std::uint32_t>(*base->formType);
					isTerminal = base->Is(RE::ENUM_FORM_ID::kTERM);
				}
			}
			logger::info("[FOVSlider] anim event 'CameraOverrideStart' furniture=0x{:08X} base=0x{:08X} type={} isTerminal={}",
				furnFormID, baseFormID, baseTypeID, isTerminal ? "yes" : "no");
			fov->LogEngineSnapshot("CameraOverrideStart");

			if (isTerminal) {
				fov->OnTerminalEntered();
				wasOnTerminal.store(true);
			}
			return RE::BSEventNotifyControl::kContinue;
		}

		// CameraOverrideEnd: belt-and-suspenders exit pathway in case
		// both TerminalMenu close + OnGetUp paths miss (verified to fire
		// on the player graph even when Esc closes the terminal).
		if (evt == "CameraOverrideEnd" || evt == "cameraOverrideEnd") {
			const bool hadTerm = wasOnTerminal.exchange(false);
			logger::info("[FOVSlider] anim event 'CameraOverrideEnd' wasOnTerminal={} ctx={}",
				hadTerm ? "yes" : "no",
				fov->GetContext() == FOVContext::Terminal ? "Terminal" :
				fov->GetContext() == FOVContext::PipBoy   ? "PipBoy"   :
				fov->GetContext() == FOVContext::Aiming   ? "Aiming"   : "Default");
			fov->LogEngineSnapshot("CameraOverrideEnd");

			if (hadTerm && fov->GetContext() == FOVContext::Terminal) {
				fov->OnTerminalExited();
			}
			return RE::BSEventNotifyControl::kContinue;
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	// ============================================================
	// Top-level registration
	// ============================================================
	void RegisterEventSinks()
	{
		MenuSink::GetSingleton()->Register();

		// AnimSink may fail until the player's graph is ready; spawn a
		// retrying worker so we don't block the messaging callback.
		std::thread([]() {
			auto* sink = AnimSink::GetSingleton();
			for (int i = 0; i < 30; ++i) {  // ~15s max
				if (sink->IsRegistered()) return;
				if (sink->Register()) return;
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
			}
			logger::warn("[FOVSlider] Animation event sink failed to register after 30 retries");
		}).detach();
	}

	void OnGameLoaded()
	{
		// MenuOpenCloseEvent registrations survive game loads, but the
		// animation graph is rebuilt - re-register the anim sink.
		AnimSink::GetSingleton()->Unregister();
		std::thread([]() {
			auto* sink = AnimSink::GetSingleton();
			for (int i = 0; i < 30; ++i) {
				if (sink->Register()) return;
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
			}
			logger::warn("[FOVSlider] Animation event sink failed to re-register after game load");
		}).detach();
	}
}
