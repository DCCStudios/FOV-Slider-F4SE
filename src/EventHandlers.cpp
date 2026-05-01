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
		static const RE::BSFixedString kPipBoy   = "PipboyMenu";
		static const RE::BSFixedString kTerminal = "TerminalMenu";

		if (name == kPipBoy) {
			if (open) {
				FOVManager::GetSingleton()->OnPipBoyOpening();
			} else {
				FOVManager::GetSingleton()->OnPipBoyClosing();
			}
		} else if (name == kTerminal) {
			// TerminalMenu close is a reliable signal. The OPEN side is
			// already handled via the CameraOverrideStart animation event
			// (AnimSink) which fires before the menu is on-screen.
			//
			// We pop context only if we're still in Terminal mode. The
			// original Papyrus mod's bug: Esc-to-close didn't reset the
			// FOV. Catching the menu-close here makes recovery reliable.
			if (!open && FOVManager::GetSingleton()->GetContext() == FOVContext::Terminal) {
				FOVManager::GetSingleton()->OnTerminalExited();
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
			fov->OnSightedStateEnter();
			return RE::BSEventNotifyControl::kContinue;
		}
		if (evt == "sightedStateExit" || evt == "SightedStateExit") {
			fov->OnSightedStateExit();
			return RE::BSEventNotifyControl::kContinue;
		}

		// Terminal entry: the player triggers `CameraOverrideStart` when
		// the camera-override begins for furniture they're sitting on.
		// We then check whether the furniture is a Terminal.
		if (evt == "CameraOverrideStart" || evt == "cameraOverrideStart") {
			// `currentFurniture` lives on AIProcess->MiddleHighProcessData,
			// not directly on Actor; the helper does the null-check chain.
			if (auto* furn = GetPlayerCurrentFurniture()) {
				if (auto* base = furn->GetObjectReference()) {
					if (base->Is(RE::ENUM_FORM_ID::kTERM)) {
						fov->OnTerminalEntered();
						wasOnTerminal.store(true);
					}
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}

		// CameraOverrideEnd: belt-and-suspenders exit pathway in case
		// both TerminalMenu close + OnGetUp paths miss (verified to fire
		// on the player graph even when Esc closes the terminal).
		if (evt == "CameraOverrideEnd" || evt == "cameraOverrideEnd") {
			if (wasOnTerminal.exchange(false) &&
			    fov->GetContext() == FOVContext::Terminal) {
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
