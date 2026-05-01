#include "PCH.h"
#include "Menu.h"
#include "Settings.h"
#include "FOVManager.h"

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
}
