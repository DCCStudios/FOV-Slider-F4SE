#include "PCH.h"
#include "Menu.h"
#include "Settings.h"
#include "FOVManager.h"
#include "Helpers.h"

#include "RE/Bethesda/PlayerCharacter.h"
#include "RE/Bethesda/TESCamera.h"

namespace FOVSlider::Menu
{
	using namespace ImGuiMCP;

	// ============================================================
	// Helpers
	// ============================================================
	static bool SliderFloatTooltip(const char* label, float* v, float lo, float hi,
	                               const char* fmt, const char* tooltip)
	{
		const bool changed = SliderFloat(label, v, lo, hi, fmt);
		if (IsItemHovered() && tooltip && tooltip[0]) {
			SetTooltip("%s", tooltip);
		}
		return changed;
	}

	static bool CheckboxTooltip(const char* label, bool* v, const char* tooltip)
	{
		const bool changed = Checkbox(label, v);
		if (IsItemHovered() && tooltip && tooltip[0]) {
			SetTooltip("%s", tooltip);
		}
		return changed;
	}

	// Working copies of the atomics so ImGui can edit them by reference.
	// We snapshot at the top of Render() and write back via OnXxxChanged.
	struct Working
	{
		float firstPersonFOV{};
		float thirdPersonFOV{};
		float viewmodelFOV{};
		float pipBoyFOV{};
		float terminalFOV{};
		float cameraDistance{};
		bool  enableFirstPersonAimFOV{};
		float firstPersonAimFOV{};
		float thirdPersonAimFOV{};
	};

	static Working SnapshotSettings()
	{
		auto* s = Settings::GetSingleton();
		Working w;
		w.firstPersonFOV          = s->firstPersonFOV.load();
		w.thirdPersonFOV          = s->thirdPersonFOV.load();
		w.viewmodelFOV            = s->viewmodelFOV.load();
		w.pipBoyFOV               = s->pipBoyFOV.load();
		w.terminalFOV             = s->terminalFOV.load();
		w.cameraDistance          = s->cameraDistance.load();
		w.enableFirstPersonAimFOV = s->enableFirstPersonAimFOV.load();
		w.firstPersonAimFOV       = s->firstPersonAimFOV.load();
		w.thirdPersonAimFOV       = s->thirdPersonAimFOV.load();
		return w;
	}

	// ============================================================
	// Registration
	// ============================================================
	void Register()
	{
		if (!F4SEMenuFramework::IsInstalled()) {
			logger::warn("[FOVSlider] F4SE Menu Framework not installed - in-game menu disabled");
			return;
		}

		F4SEMenuFramework::SetSection("FOV Slider F4SE");
		F4SEMenuFramework::AddSectionItem("FOV Settings", Render);

		// Non-pausing floating window for live FOV diagnostics.
		// Second arg = false: stays visible during normal gameplay
		// (does NOT block player input or close with the Mod Control
		// Panel). Closed by default; toggled via the checkbox in the
		// FOV Settings page.
		DebugWindow::Handle = F4SEMenuFramework::AddWindow(DebugWindow::Render, false);

		logger::info("[FOVSlider] Menu registered with F4SE Menu Framework");
	}

	// ============================================================
	// Render
	// ============================================================
	void __stdcall Render()
	{
		auto* fov = FOVManager::GetSingleton();
		Working w = SnapshotSettings();

		Text("FOV Slider F4SE");
		TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
		            "F4SE plugin replacement for Simple FOV Slider");
		Text("Active context: %s",
		     fov->GetContext() == FOVContext::Default  ? "Default" :
		     fov->GetContext() == FOVContext::PipBoy   ? "Pip-Boy"  :
		     fov->GetContext() == FOVContext::Terminal ? "Terminal" :
		     fov->GetContext() == FOVContext::Aiming   ? "Aiming"   : "?");

		// Debug popout toggle. Reads/writes the framework-owned
		// IsOpen flag directly so the checkbox stays in sync if the
		// user closes the window via its own Close button (or any
		// future hotkey).
		if (DebugWindow::Handle) {
			bool dbgOpen = DebugWindow::Handle->IsOpen.load();
			if (Checkbox("Show Debug Popout##fovslider_dbg", &dbgOpen)) {
				DebugWindow::Handle->IsOpen.store(dbgOpen);
			}
			if (IsItemHovered()) {
				SetTooltip("Floating, non-pausing window that shows live FOV\n"
				           "values, current context, camera state, and\n"
				           "FPInertia status. Stays visible during gameplay.");
			}
		}

		// Verbose-logging toggle. Mirrors [Diagnostics] bVerboseLogging
		// in the INI. When on, we trace every state transition and
		// SetEngineFloatSetting call to FOVSliderF4SE.log so the user
		// can post-mortem any FOV drift incident.
		bool verbose = Settings::GetSingleton()->verboseLogging.load();
		if (Checkbox("Verbose Logging##fovslider_verbose", &verbose)) {
			Settings::GetSingleton()->verboseLogging.store(verbose);
			Settings::GetSingleton()->Save();
			spdlog::set_level(verbose ? spdlog::level::trace : spdlog::level::info);
			spdlog::flush_on(verbose ? spdlog::level::trace : spdlog::level::info);
			logger::info("[FOVSlider] Verbose logging {} via menu", verbose ? "ENABLED" : "disabled");
		}
		if (IsItemHovered()) {
			SetTooltip("Logs every context transition, console command,\n"
			           "and engine setting change to FOVSliderF4SE.log.\n"
			           "Combined with the drift watcher (see INI), it\n"
			           "lets you trace exactly where unwanted FOV writes\n"
			           "come from.");
		}
		Separator();

		Text("Display");
		Spacing();

		if (SliderFloatTooltip("First-Person FOV##fp", &w.firstPersonFOV, 60.0f, 130.0f, "%.0f",
		                       "[Default: 80] The FOV in first-person.")) {
			fov->OnFirstPersonFOVChanged(w.firstPersonFOV);
		}

		if (SliderFloatTooltip("Third-Person FOV##tp", &w.thirdPersonFOV, 60.0f, 130.0f, "%.0f",
		                       "[Default: 80] The FOV in third-person.")) {
			fov->OnThirdPersonFOVChanged(w.thirdPersonFOV);
		}

		if (SliderFloatTooltip("Viewmodel FOV##vm", &w.viewmodelFOV, 30.0f, 160.0f, "%.0f",
		                       "[Default: 80] Default viewmodel FOV (arms / weapon).\n"
		                       "FPInertia's Weapon-Based FOV (WBFOV) takes priority\n"
		                       "when an entry exists for the equipped weapon.")) {
			fov->OnViewmodelFOVChanged(w.viewmodelFOV);
		}

		if (SliderFloatTooltip("Pip-Boy FOV##pb", &w.pipBoyFOV, 60.0f, 120.0f, "%.0f",
		                       "[Default: 80] Viewmodel FOV while the Pip-Boy menu is open.")) {
			fov->OnPipBoyFOVChanged(w.pipBoyFOV);
		}

		if (SliderFloatTooltip("Terminal FOV##tm", &w.terminalFOV, 60.0f, 120.0f, "%.0f",
		                       "[Default: 80] Viewmodel FOV while at a terminal.")) {
			fov->OnTerminalFOVChanged(w.terminalFOV);
		}

		if (SliderFloatTooltip("Camera Near Distance##nd", &w.cameraDistance, 0.0f, 20.0f, "%.2f",
		                       "[Default: 15.0] Camera near-clip distance. Lower values may cause\n"
		                       "visual bugs (sun flicker) but can reduce object clipping at high FOV.")) {
			fov->OnCameraDistanceChanged(w.cameraDistance);
		}

		Spacing();
		Separator();
		Text("Aim FOV");
		Spacing();

		if (CheckboxTooltip("Enable First-Person Aim FOV", &w.enableFirstPersonAimFOV,
		                    "[Default: Off] Smoothly snap the camera FOV when entering ADS.")) {
			fov->OnEnableFirstPersonAimFOVChanged(w.enableFirstPersonAimFOV);
		}

		if (!w.enableFirstPersonAimFOV) {
			BeginDisabled(true);
		}
		if (SliderFloatTooltip("First-Person Aim FOV##fpaim", &w.firstPersonAimFOV, 30.0f, 130.0f, "%.0f",
		                       "[Default: 80] FOV while aiming in first-person (when enabled).")) {
			fov->OnFirstPersonAimFOVChanged(w.firstPersonAimFOV);
		}
		if (!w.enableFirstPersonAimFOV) {
			EndDisabled();
		}

		if (SliderFloatTooltip("Third-Person Aim FOV##tpaim", &w.thirdPersonAimFOV, 30.0f, 130.0f, "%.0f",
		                       "[Default: 50] FOV while aiming in third-person.")) {
			fov->OnThirdPersonAimFOVChanged(w.thirdPersonAimFOV);
		}

		Spacing();
		Separator();

		if (Button("Re-apply All Settings")) {
			fov->ApplyAllSettings();
		}
		if (IsItemHovered()) {
			SetTooltip("Force-re-applies every FOV setting now.\n"
			           "Useful if another mod has stomped on your FOV.");
		}
		SameLine();
		if (Button("Reload from INI")) {
			Settings::GetSingleton()->Load();
			fov->ApplyAllSettings();
		}
	}

	// ============================================================
	// Debug popout window
	// ============================================================
	namespace DebugWindow
	{
		// Translate a CameraStates enum value into a human-readable
		// label. The enum is `unsigned`, so we accept it as such.
		static const char* CameraStateLabel(unsigned a_state)
		{
			switch (a_state) {
				case RE::CameraState::kFirstPerson:  return "kFirstPerson";
				case RE::CameraState::kAutoVanity:   return "kAutoVanity";
				case RE::CameraState::kVATS:         return "kVATS";
				case RE::CameraState::kFree:         return "kFree";
				case RE::CameraState::kIronSights:   return "kIronSights";
				case RE::CameraState::kPCTransition: return "kPCTransition";
				case RE::CameraState::kTween:        return "kTween";
				case RE::CameraState::kAnimated:     return "kAnimated";
				case RE::CameraState::k3rdPerson:    return "k3rdPerson";
				case RE::CameraState::kFurniture:    return "kFurniture";
				case RE::CameraState::kMount:        return "kMount";
				case RE::CameraState::kBleedout:     return "kBleedout";
				case RE::CameraState::kDialogue:     return "kDialogue";
				default:                             return "(unknown)";
			}
		}

		static const char* ContextLabel(FOVContext a_ctx)
		{
			switch (a_ctx) {
				case FOVContext::Default:  return "Default";
				case FOVContext::PipBoy:   return "Pip-Boy";
				case FOVContext::Terminal: return "Terminal";
				case FOVContext::Aiming:   return "Aiming";
				default:                   return "?";
			}
		}

		// One row of the saved-vs-engine comparison table. Highlights
		// the row red when the values disagree by more than 0.05 (the
		// visible mismatch we care about; rounding noise is below that).
		static void CompareRow(const char* a_label, float a_saved, float a_engine, bool a_engineValid)
		{
			TableNextRow();
			TableSetColumnIndex(0);
			Text("%s", a_label);
			TableSetColumnIndex(1);
			Text("%.2f", a_saved);
			TableSetColumnIndex(2);
			if (!a_engineValid) {
				TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "n/a");
			} else {
				const bool drift = std::fabs(a_saved - a_engine) > 0.05f;
				const ImVec4 col = drift
					? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
					: ImVec4(0.6f, 1.0f, 0.6f, 1.0f);
				TextColored(col, "%.2f", a_engine);
			}
		}

		void __stdcall Render()
		{
			// ---- Window placement ----
			// Anchored to the upper-right on first appearance. After
			// that the user can drag/resize freely - ImGuiCond_Appearing
			// only applies to the first frame the window opens.
			auto* viewport = GetMainViewport();

			ImVec2 windowSize{ viewport->Size.x * 0.30f, viewport->Size.y * 0.55f };
			ImVec2 windowPos{
				viewport->Pos.x + viewport->Size.x - windowSize.x - 20.0f,
				viewport->Pos.y + 20.0f
			};
			SetNextWindowPos(windowPos, ImGuiCond_Appearing, ImVec2{ 0.0f, 0.0f });
			SetNextWindowSize(windowSize, ImGuiCond_Appearing);

			Begin("FOV Slider Debug##FOVSliderF4SE", nullptr, ImGuiWindowFlags_NoCollapse);

			auto* fov      = FOVManager::GetSingleton();
			auto* settings = Settings::GetSingleton();

			// ---- Live snapshot ----
			const FOVContext ctx     = fov->GetContext();
			const bool       inPA    = IsPlayerInPowerArmor();
			const bool       inADS   = IsPlayerInIronSights();
			auto*            furniture = GetPlayerCurrentFurniture();
			const float      lastVm  = fov->GetLastAppliedViewmodel();
			const float      lastCam = fov->GetLastAppliedCamera();

			unsigned cameraState = static_cast<unsigned>(-1);
			if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->currentState) {
				cameraState = static_cast<unsigned>(camera->currentState->id.get());
			}

			// ---- State block ----
			TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "State");
			Spacing();
			Text("Context:       %s", ContextLabel(ctx));
			Text("Camera state:  %s", cameraState == static_cast<unsigned>(-1)
				? "(unavailable)" : CameraStateLabel(cameraState));
			Text("Power Armor:   %s", inPA  ? "yes" : "no");
			Text("Iron Sights:   %s", inADS ? "yes" : "no");
			if (furniture) {
				Text("Furniture:     0x%08X", furniture->formID);
			} else {
				Text("Furniture:     none");
			}

			Spacing();
			Separator();

			// ---- FOV comparison table ----
			TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "FOV (saved vs engine)");
			TextWrapped("Green = engine matches our saved value. Red = drift "
			            "(another mod / engine re-init has clobbered the value).");
			Spacing();

			float ePersp1 = 0.0f, ePersp3 = 0.0f, eAim3 = 0.0f, eNear = 0.0f;
			const bool h1   = FOVManager::ReadEngineFloatSetting("fDefault1stPersonFOV:Display", ePersp1);
			const bool h3   = FOVManager::ReadEngineFloatSetting("fDefaultWorldFOV:Display",     ePersp3);
			const bool ha3  = FOVManager::ReadEngineFloatSetting("f3rdPersonAimFOV:Camera",      eAim3);
			const bool hnd  = FOVManager::ReadEngineFloatSetting("fNearDistance:Display",        eNear);

			const ImGuiTableFlags tflags =
				ImGuiTableFlags_RowBg |
				ImGuiTableFlags_BordersOuter |
				ImGuiTableFlags_BordersInnerV |
				ImGuiTableFlags_SizingStretchProp;

			if (BeginTable("fovslider_dbg_table", 3, tflags)) {
				TableSetupColumn("Setting");
				TableSetupColumn("Saved");
				TableSetupColumn("Engine");
				TableHeadersRow();

				CompareRow("1st-person FOV",     settings->firstPersonFOV.load(),    ePersp1, h1);
				CompareRow("3rd-person FOV",     settings->thirdPersonFOV.load(),    ePersp3, h3);
				CompareRow("3rd-person aim FOV", settings->thirdPersonAimFOV.load(), eAim3,   ha3);
				CompareRow("Near distance",      settings->cameraDistance.load(),    eNear,   hnd);

				// Viewmodel / Pip-Boy / Terminal FOV are NOT readable
				// back from any engine collection - they're applied via
				// the `fov X Y` console command, which mutates a
				// runtime-only camera value. We instead show what we
				// last issued through the lastApplied* cache.
				TableNextRow();
				TableSetColumnIndex(0); Text("Viewmodel FOV (saved)");
				TableSetColumnIndex(1); Text("%.2f", settings->viewmodelFOV.load());
				TableSetColumnIndex(2); TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(runtime only)");

				TableNextRow();
				TableSetColumnIndex(0); Text("Pip-Boy FOV");
				TableSetColumnIndex(1); Text("%.2f", settings->pipBoyFOV.load());
				TableSetColumnIndex(2); TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(runtime only)");

				TableNextRow();
				TableSetColumnIndex(0); Text("Terminal FOV");
				TableSetColumnIndex(1); Text("%.2f", settings->terminalFOV.load());
				TableSetColumnIndex(2); TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(runtime only)");

				EndTable();
			}

			Spacing();
			Separator();

			// ---- Last `fov X Y` we issued ----
			TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "Last `fov X Y` issued");
			Spacing();
			if (lastVm < 0.0f) {
				Text("Viewmodel (X): (none yet)");
			} else {
				Text("Viewmodel (X): %.2f", lastVm);
			}
			if (lastCam < 0.0f) {
				Text("Camera    (Y): (none yet)");
			} else {
				Text("Camera    (Y): %.2f", lastCam);
			}

			Spacing();
			Separator();

			// ---- Aim FOV ----
			TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "Aim FOV");
			Spacing();
			Text("First-person aim enabled: %s",
			     settings->enableFirstPersonAimFOV.load() ? "yes" : "no");
			Text("First-person aim FOV:     %.2f", settings->firstPersonAimFOV.load());
			Text("Third-person aim FOV:     %.2f", settings->thirdPersonAimFOV.load());

			Spacing();
			Separator();

			// ---- FPInertia ----
			TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "FPInertia");
			Spacing();
			if (auto info = F4SE::GetPluginInfo("FPInertia"); info.has_value()) {
				Text("Detected: yes (api version %u)", info->version);
				TextWrapped("Cross-plugin handshake is active. Pip-Boy / Terminal "
				            "transitions lock FPInertia's WBFOV applies until our "
				            "lerp completes.");
			} else {
				Text("Detected: no");
				TextWrapped("Viewmodel FOV is fully owned by this plugin. Per-weapon "
				            "WBFOV overrides require FPInertia.");
			}

			Spacing();
			Separator();

			// ---- Drift-watcher controls ----
			TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "Drift Watcher");
			Spacing();
			bool autoCorrect = settings->driftAutoCorrect.load();
			if (Checkbox("Auto-correct drift##dbg", &autoCorrect)) {
				settings->driftAutoCorrect.store(autoCorrect);
				settings->Save();
			}
			if (IsItemHovered()) {
				SetTooltip("When the drift watcher sees the engine\n"
				           "1st-person FOV diverge from your saved value,\n"
				           "smoothly lerp back to saved (same lerp shape\n"
				           "as Pip-Boy / iron sights / game-load).\n"
				           "Required to defeat the engine's late post-\n"
				           "load FOV restore.");
			}

			int correctMs = settings->driftCorrectDurationMs.load();
			if (SliderInt("Correct duration (ms)##dbg", &correctMs, 50, 1000)) {
				settings->driftCorrectDurationMs.store(correctMs);
				settings->Save();
			}
			if (IsItemHovered()) {
				SetTooltip("How long the auto-correct lerp takes.\n"
				           "Same shape as the load burst, just on a\n"
				           "shorter timeline since you're in active\n"
				           "gameplay. 250 ms feels invisible at 60 fps.");
			}

			Spacing();
			Separator();

			// ---- Actions ----
			if (Button("Re-apply All##dbg")) {
				fov->ApplyAllSettings();
			}
			if (IsItemHovered()) {
				SetTooltip("Snap to saved values (instant).");
			}
			SameLine();
			if (Button("Smooth Re-apply##dbg")) {
				fov->LerpAllSettings(correctMs, 8);
			}
			if (IsItemHovered()) {
				SetTooltip("Smoothly lerp engine values back to saved\n"
				           "over `Correct duration`. Same code path the\n"
				           "drift auto-correct uses; useful for testing\n"
				           "the lerp without forcing the engine to drift\n"
				           "first.");
			}
			SameLine();
			if (Button("Reload INI##dbg")) {
				settings->Load();
				fov->ApplyAllSettings();
			}
			SameLine();
			// Dumps a snapshot of all four FOV-related engine values
			// to FOVSliderF4SE.log under the tag "Manual". Use this
			// to mark a moment of interest while reproducing a bug -
			// e.g. press it the instant you see drift in the table
			// above and the log will record the exact engine state
			// at that timestamp for later inspection.
			if (Button("Dump Snapshot##dbg")) {
				fov->LogEngineSnapshot("Manual");
			}
			if (IsItemHovered()) {
				SetTooltip("Writes a SNAPSHOT line to FOVSliderF4SE.log\n"
				           "with all four FOV settings (saved + engine).\n"
				           "Press while you can see drift to capture\n"
				           "the exact engine state for later analysis.");
			}
			SameLine();
			if (Button("Close##dbg")) {
				if (Handle) Handle->IsOpen.store(false);
			}

			End();
		}
	}
}
