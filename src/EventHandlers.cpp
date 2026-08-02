#include "PCH.h"
#include "EventHandlers.h"
#include "FOVManager.h"
#include "Helpers.h"
#include "Settings.h"

namespace FOVSlider
{
	// ============================================================
	// MenuSink - PipBoy / VATS / Terminal open/close
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
		static const RE::BSFixedString kVATSMenu   = "VATSMenu";
		static const RE::BSFixedString kTerminal    = "TerminalMenu";
		static const RE::BSFixedString kLoading     = "LoadingMenu";
		static const RE::BSFixedString kFader       = "FaderMenu";
		static const RE::BSFixedString kExamine     = "ExamineMenu";

		// Trace EVERY menu event so we can see what fires around any
		// reported FOV drift. Most are ignored (we only act on PipBoy,
		// VATS, and TerminalMenu) but the user-facing diagnostics need to
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
		} else if (name == kVATSMenu) {
			if (open) {
				fov->OnVATSBegin();
			} else {
				fov->OnVATSEnd();
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
		} else if (name == kLoading || name == kFader) {
			// ---- Screen occluders + engine-restore triggers ----
			// Both menus fully cover the frame while open. Track that
			// state so the load workers and drift watcher know when an
			// instant FOV hard-set is invisible to the player
			// (IsScreenCovered()).
			if (name == kLoading) {
				fov->loadingMenuOpen.store(open);
			} else {
				fov->faderMenuOpen.store(open);
			}

			// On close, engage the drift watcher's hot-poll mode: the
			// diagnostic log identified these closes as preceding the
			// engine's own default-FOV writes by 0.5 - 2 s. A fixed-delay
			// rewrite would race the engine; the hot-poll watcher reacts
			// to whatever the engine ACTUALLY writes, whenever it writes
			// it. (Fader fires both as a load-screen fade-out AND as a
			// generic UI fader; back-to-back triggers just extend the
			// hot deadline.)
			if (!open) {
				logger::info("[FOVSlider] {} closed - engaging drift hot mode",
					name == kLoading ? "LoadingMenu" : "FaderMenu");
				fov->TriggerDriftHotMode(settings->driftWatchHotDurationMs.load());
			}
		} else if (!open && name == kExamine) {
			// ExamineMenu = workbenches, chem stations, cooking
			// stations, etc. The engine's camera-override teardown
			// for these writes default FOVs ~1.7 s after the menu
			// closes, which is why hot mode needs to last that
			// long (>= ~3 s).
			logger::info("[FOVSlider] ExamineMenu closed - engaging drift hot mode");
			fov->TriggerDriftHotMode(settings->driftWatchHotDurationMs.load());
		}

		return RE::BSEventNotifyControl::kContinue;
	}

	// ============================================================
	// Player animation event receiver hook
	// ============================================================
	namespace
	{
		// TESObjectREFR's BSTEventSink<BSAnimationGraphEvent> base is the
		// fourth vtable in PlayerCharacter::VTABLE. Its ProcessEvent method is
		// vfunc 1. This vtable ID and base order are shared by OG, NG, and AE.
		using ProcessEventFn = RE::BSEventNotifyControl(*)(
			void*,
			const RE::BSAnimationGraphEvent&,
			RE::BSTEventSource<RE::BSAnimationGraphEvent>*);

		ProcessEventFn g_originalPlayerProcessEvent = nullptr;
		std::atomic_bool g_playerEventHookInstalled{ false };

		RE::BSEventNotifyControl HookedPlayerProcessEvent(
			void* a_this,
			const RE::BSAnimationGraphEvent& a_event,
			RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_source)
		{
			AnimSink::GetSingleton()->ProcessEvent(a_event);

			// Always continue through the previously installed function. This
			// preserves the engine handler and chains correctly with plugins
			// that hook this same vtable before or after FOV Slider.
			return g_originalPlayerProcessEvent(a_this, a_event, a_source);
		}

		void InstallPlayerAnimationEventHook()
		{
			if (g_playerEventHookInstalled.exchange(true)) {
				return;
			}

			REL::Relocation<std::uintptr_t> playerAnimationEventVtable{
				RE::PlayerCharacter::VTABLE[3]
			};
			g_originalPlayerProcessEvent = reinterpret_cast<ProcessEventFn>(
				playerAnimationEventVtable.write_vfunc(1, &HookedPlayerProcessEvent));

			if (!g_originalPlayerProcessEvent) {
				g_playerEventHookInstalled.store(false);
				REX::FAIL("[FOVSlider] Player animation event vtable hook returned a null original");
			}

			logger::info(
				"[FOVSlider] Player animation event hook installed "
				"(PlayerCharacter::VTABLE[3], vfunc 1)");
		}

		// ============================================================
		// FirstPersonState::Update hook - same-frame FOV cut guard
		// ============================================================
		// First attempt hooked PlayerCamera::Update (TESCamera vfunc 03)
		// and NEVER fired - the 15:39 session log shows the guard arming
		// three times with zero disarm lines while the engine's 90->105
		// ramp ran untouched, so the engine calls PlayerCamera::Update
		// directly (devirtualized), not through the vtable.
		//
		// TESCameraState::Update CANNOT be devirtualized: TESCamera holds
		// polymorphic state objects and dispatches currentState->Update
		// through the state's vtable every frame. Update is slot 0x0B on
		// the primary vtable (BSInputEventUser base occupies 00-08, then
		// Begin=09, End=0A, Update=0B - see TESCameraState.h). Hooking
		// FirstPersonState's vtable gives us a per-frame callback exactly
		// while the first-person camera is active, which is precisely
		// when the post-terminal cut needs same-frame correction: the
		// engine's own FOV writes happen inside this state update, so
		// correcting after the original returns lands before the renderer
		// consumes worldFOV and the wrong value is never displayed.
		using CameraStateUpdateFn = void (*)(
			RE::TESCameraState*,
			RE::BSTSmartPointer<RE::TESCameraState>&);

		CameraStateUpdateFn g_originalFirstPersonStateUpdate = nullptr;
		std::atomic_bool    g_firstPersonStateHookInstalled{ false };

		void HookedFirstPersonStateUpdate(
			RE::TESCameraState*                       a_this,
			RE::BSTSmartPointer<RE::TESCameraState>&  a_nextState)
		{
			g_originalFirstPersonStateUpdate(a_this, a_nextState);
			FOVManager::GetSingleton()->OnCameraUpdateFrame();
		}

		void InstallFirstPersonStateUpdateHook()
		{
			if (g_firstPersonStateHookInstalled.exchange(true)) {
				return;
			}

			REL::Relocation<std::uintptr_t> firstPersonStateVtable{
				RE::VTABLE::FirstPersonState[0]
			};
			g_originalFirstPersonStateUpdate = reinterpret_cast<CameraStateUpdateFn>(
				firstPersonStateVtable.write_vfunc(11, &HookedFirstPersonStateUpdate));

			if (!g_originalFirstPersonStateUpdate) {
				g_firstPersonStateHookInstalled.store(false);
				REX::FAIL("[FOVSlider] FirstPersonState::Update vtable hook returned a null original");
			}

			logger::info(
				"[FOVSlider] FirstPersonState update hook installed "
				"(FirstPersonState::VTABLE[0], vfunc 11)");
		}
	}

	// ============================================================
	// AnimSink - sighted state + camera override (terminal furniture)
	// ============================================================
	void AnimSink::ProcessEvent(const RE::BSAnimationGraphEvent& a_event)
	{
		const auto& evt = a_event.tag;
		auto* fov = FOVManager::GetSingleton();

		// Sighted-state hooks for the optional First-Person Aim FOV feature.
		// The engine emits these in both casings depending on the .hkx
		// annotation, so we accept both.
		if (evt == "sightedStateEnter" || evt == "SightedStateEnter") {
			logger::info("[FOVSlider] anim event '{}'", evt.c_str());
			fov->OnSightedStateEnter();
			return;
		}
		if (evt == "sightedStateExit" || evt == "SightedStateExit") {
			logger::info("[FOVSlider] anim event '{}'", evt.c_str());
			fov->OnSightedStateExit();
			return;
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
			return;
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
				fov->GetContext() == FOVContext::VATS     ? "VATS"     :
				fov->GetContext() == FOVContext::Aiming   ? "Aiming"   : "Default");
			fov->LogEngineSnapshot("CameraOverrideEnd");

			if (hadTerm && fov->GetContext() == FOVContext::Terminal) {
				fov->OnTerminalExited();
			} else {
				// Non-terminal furniture (workbenches, chairs, power-armor
				// stations) gets the same late first-person camera cut at
				// the menu-override FOV. FP gunplay's resume-from-block
				// lerp used to paper over it by hard-writing worldFOV;
				// now that its EXECs are worldFOV-neutral, the guard owns
				// this window for all furniture.
				fov->SuppressCameraTransitionZoom("CameraOverrideEnd");
			}
			return;
		}
	}

	// ============================================================
	// Top-level registration
	// ============================================================
	void RegisterEventSinks()
	{
		MenuSink::GetSingleton()->Register();
		InstallPlayerAnimationEventHook();
		InstallFirstPersonStateUpdateHook();
	}

	void OnGameLoaded()
	{
		// The PlayerCharacter vtable hook survives graph rebuilds. UI sink
		// registration is idempotent and is refreshed defensively.
		MenuSink::GetSingleton()->Register();
	}
}
