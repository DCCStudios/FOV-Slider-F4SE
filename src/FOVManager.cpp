#include "PCH.h"
#include "FOVManager.h"
#include "Settings.h"
#include "Helpers.h"

#include <type_traits>

namespace FOVSlider
{
	// FPInertia listens for these message types (handler in FPInertia/main.cpp):
	//   FSRF - refresh WBFOV defaults from disk
	//   FSLK - external override lock state. 1-byte payload: 0=unlock, 1=lock.
	static constexpr std::uint32_t kFPInertia_RefreshMsg = 0x46535246;  // 'FSRF'
	static constexpr std::uint32_t kFPInertia_LockMsg    = 0x46534C4B;  // 'FSLK'

	// ============================================================
	// Engine-setting helpers
	// ============================================================
	bool FOVManager::TryReadEngineFloatSetting(const char* a_key, float& a_out)
	{
		if (auto* prefs = RE::INIPrefSettingCollection::GetSingleton()) {
			if (auto* s = prefs->GetSetting(a_key); s && s->GetType() == RE::Setting::SETTING_TYPE::kFloat) {
				a_out = s->GetFloat();
				return true;
			}
		}
		if (auto* ini = RE::INISettingCollection::GetSingleton()) {
			if (auto* s = ini->GetSetting(a_key); s && s->GetType() == RE::Setting::SETTING_TYPE::kFloat) {
				a_out = s->GetFloat();
				return true;
			}
		}
		if (auto* gs = RE::GameSettingCollection::GetSingleton()) {
			for (auto& kv : gs->settings) {
				if (kv.second && kv.second->GetKey() == a_key &&
				    kv.second->GetType() == RE::Setting::SETTING_TYPE::kFloat) {
					a_out = kv.second->GetFloat();
					return true;
				}
			}
		}
		return false;
	}

	bool FOVManager::SetEngineFloatSetting(const char* a_key, float a_value)
	{
		// Try INIPrefSettingCollection first (the Papyrus Utility::SetINIFloat
		// path also resolves here for "*:Display" keys).
		if (auto* prefs = RE::INIPrefSettingCollection::GetSingleton()) {
			if (auto* s = prefs->GetSetting(a_key); s && s->GetType() == RE::Setting::SETTING_TYPE::kFloat) {
				s->SetFloat(a_value);
				return true;
			}
		}
		if (auto* ini = RE::INISettingCollection::GetSingleton()) {
			if (auto* s = ini->GetSetting(a_key); s && s->GetType() == RE::Setting::SETTING_TYPE::kFloat) {
				s->SetFloat(a_value);
				return true;
			}
		}
		if (auto* gs = RE::GameSettingCollection::GetSingleton()) {
			for (auto& kv : gs->settings) {
				if (kv.second && kv.second->GetKey() == a_key &&
				    kv.second->GetType() == RE::Setting::SETTING_TYPE::kFloat) {
					kv.second->SetFloat(a_value);
					return true;
				}
			}
		}
		logger::warn("[FOVSlider] Engine setting '{}' not found - couldn't apply {:.2f}", a_key, a_value);
		return false;
	}

	bool FOVManager::ExecuteConsoleCommand(std::string_view a_command)
	{
		// Same recipe as FPInertia's WeaponFOV.cpp:
		//   1. ConcreteFormFactory<RE::Script>::Create()
		//   2. SetText(command)
		//   3. CompileAndRun with a real ScriptCompiler instance and
		//      kSystemWindow compiler-name. nullptr / kDefault silently no-op.
		//   4. Suppress the console-history line so we don't spam.
		auto* factory = RE::ConcreteFormFactory<RE::Script>::GetFormFactory();
		if (!factory) {
			logger::warn("[FOVSlider] Script form factory unavailable - cannot run '{}'", a_command);
			return false;
		}
		auto* script = factory->Create();
		if (!script) {
			logger::warn("[FOVSlider] Script create failed - cannot run '{}'", a_command);
			return false;
		}

		auto* log = RE::ConsoleLog::GetSingleton();
		std::remove_cvref_t<decltype(log->buffer)> savedBuffer{};
		if (log) savedBuffer = log->buffer;

		RE::ScriptCompiler compiler;
		script->SetText(a_command);
		script->CompileAndRun(&compiler, RE::COMPILER_NAME::kSystemWindow, nullptr);

		const bool ok = script->header.isCompiled;

		if (log) log->buffer = std::move(savedBuffer);
		delete script;

		if (!ok) {
			logger::warn("[FOVSlider] Failed to compile '{}'", a_command);
		}
		return ok;
	}

	// ============================================================
	// Initialization
	// ============================================================
	void FOVManager::Init()
	{
		context.store(FOVContext::Default);
		lastAppliedViewmodel.store(-1.0f);
		lastAppliedCamera.store(-1.0f);
	}

	// ============================================================
	// Apply primitives
	// ============================================================
	void FOVManager::ApplyFirstPersonFOV(float fov)
	{
		// fDefault1stPersonFOV:Display lives in INIPrefSettingCollection
		// (Fallout4Prefs.ini). Writing the in-memory value updates the
		// camera every frame; no script command needed.
		SetEngineFloatSetting("fDefault1stPersonFOV:Display", fov);
	}

	void FOVManager::ApplyThirdPersonFOV(float fov)
	{
		SetEngineFloatSetting("fDefaultWorldFOV:Display", fov);
	}

	void FOVManager::ApplyThirdPersonAimFOV(float fov)
	{
		// Lives in GameSettingCollection (esm-defined default key).
		SetEngineFloatSetting("f3rdPersonAimFOV:Camera", fov);
	}

	void FOVManager::ApplyCameraDistance(float distance)
	{
		SetEngineFloatSetting("fNearDistance:Display", distance);
	}

	void FOVManager::ApplyViewmodelFOV(float vmFov)
	{
		if (vmFov < 30.0f)  vmFov = 30.0f;
		if (vmFov > 160.0f) vmFov = 160.0f;

		// X = viewmodel, Y = camera. We feed back the user's current
		// 1st-person camera setting (from the engine settings) so we never
		// disturb the world camera. The engine writes
		// `fDefault1stPersonFOV:Display` every time the user moves the
		// slider, so reading it back gives us the user's live preference.
		float camFov = 90.0f;
		if (!TryReadEngineFloatSetting("fDefault1stPersonFOV:Display", camFov)) {
			camFov = Settings::GetSingleton()->firstPersonFOV.load();
		}
		if (camFov < 30.0f || camFov > 160.0f) camFov = 90.0f;

		// Skip if nothing changed - executing the script compiler is
		// surprisingly expensive when called every couple frames.
		const float prevVm = lastAppliedViewmodel.load();
		const float prevCam = lastAppliedCamera.load();
		if (std::fabs(prevVm - vmFov) < 0.001f &&
		    std::fabs(prevCam - camFov) < 0.001f) {
			return;
		}

		const std::string cmd = std::format("fov {:.4f} {:.4f}", vmFov, camFov);
		if (ExecuteConsoleCommand(cmd)) {
			lastAppliedViewmodel.store(vmFov);
			lastAppliedCamera.store(camFov);
		}
	}

	// ============================================================
	// Smooth interpolation
	// ============================================================
	void FOVManager::InterpolateViewmodelFOV(float from, float to, int frames)
	{
		if (frames <= 1 || std::fabs(to - from) < 0.05f) {
			ApplyViewmodelFOV(to);
			return;
		}

		const std::uint64_t myGen = ++interpGeneration;

		// 1ms per step matches the original Papyrus pacing closely enough
		// to look identical at 60 fps but isn't tied to game framerate.
		std::thread([this, from, to, frames, myGen]() {
			const float delta = to - from;
			const float step  = delta / static_cast<float>(frames);
			float       cur   = from;

			for (int i = 0; i < frames; ++i) {
				if (interpGeneration.load() != myGen) return;  // superseded
				cur += step;
				ApplyViewmodelFOV(cur);
				std::this_thread::sleep_for(std::chrono::milliseconds(8));
			}
			// Always land on `to` exactly so floating-point drift doesn't
			// leave us 0.05 deg off-target.
			if (interpGeneration.load() == myGen) {
				ApplyViewmodelFOV(to);
			}
		}).detach();
	}

	// ============================================================
	// Target FOV computation
	// ============================================================
	float FOVManager::GetTargetCameraFOV() const
	{
		auto* s = Settings::GetSingleton();
		switch (context.load()) {
		case FOVContext::Aiming:
			if (s->enableFirstPersonAimFOV.load()) {
				return s->firstPersonAimFOV.load();
			}
			return s->firstPersonFOV.load();
		case FOVContext::PipBoy:
		case FOVContext::Terminal:
		case FOVContext::Default:
		default:
			return s->firstPersonFOV.load();
		}
	}

	float FOVManager::GetTargetViewmodelFOV() const
	{
		auto* s = Settings::GetSingleton();
		switch (context.load()) {
		case FOVContext::PipBoy:    return s->pipBoyFOV.load();
		case FOVContext::Terminal:  return s->terminalFOV.load();
		case FOVContext::Aiming:
		case FOVContext::Default:
		default:
			return s->viewmodelFOV.load();
		}
	}

	// ============================================================
	// Setting-change callbacks (from menu UI)
	// ============================================================
	void FOVManager::OnFirstPersonFOVChanged(float v)
	{
		Settings::GetSingleton()->firstPersonFOV.store(v);
		Settings::GetSingleton()->Save();

		// Only apply 1st-person FOV directly when we're not currently
		// aiming - aiming has its own value.
		if (context.load() != FOVContext::Aiming || !Settings::GetSingleton()->enableFirstPersonAimFOV.load()) {
			ApplyFirstPersonFOV(v);
		}
		NotifyFPInertia();
	}

	void FOVManager::OnThirdPersonFOVChanged(float v)
	{
		Settings::GetSingleton()->thirdPersonFOV.store(v);
		Settings::GetSingleton()->Save();
		ApplyThirdPersonFOV(v);
	}

	void FOVManager::OnViewmodelFOVChanged(float v)
	{
		Settings::GetSingleton()->viewmodelFOV.store(v);
		Settings::GetSingleton()->Save();

		// Re-applying the viewmodel FOV resets both 1st- and 3rd-person
		// camera FOVs (engine quirk of `fov X` vs `fov X Y`), so we set
		// them back immediately afterward.
		if (context.load() == FOVContext::Default) {
			ApplyViewmodelFOV(v);
		}
		// Always re-assert camera FOVs so the engine quirk doesn't bite us.
		ApplyFirstPersonFOV(Settings::GetSingleton()->firstPersonFOV.load());
		ApplyThirdPersonFOV(Settings::GetSingleton()->thirdPersonFOV.load());

		NotifyFPInertia();
	}

	void FOVManager::OnPipBoyFOVChanged(float v)
	{
		Settings::GetSingleton()->pipBoyFOV.store(v);
		Settings::GetSingleton()->Save();
		// If we're currently inside the PipBoy, apply live.
		if (!IsPlayerInPowerArmor() && context.load() == FOVContext::PipBoy) {
			ApplyViewmodelFOV(v);
		}
	}

	void FOVManager::OnTerminalFOVChanged(float v)
	{
		Settings::GetSingleton()->terminalFOV.store(v);
		Settings::GetSingleton()->Save();
		if (context.load() == FOVContext::Terminal) {
			ApplyViewmodelFOV(v);
		}
	}

	void FOVManager::OnCameraDistanceChanged(float v)
	{
		Settings::GetSingleton()->cameraDistance.store(v);
		Settings::GetSingleton()->Save();
		ApplyCameraDistance(v);
	}

	void FOVManager::OnEnableFirstPersonAimFOVChanged(bool v)
	{
		Settings::GetSingleton()->enableFirstPersonAimFOV.store(v);
		Settings::GetSingleton()->Save();
		// If we're currently aiming, recompute camera FOV instantly.
		if (context.load() == FOVContext::Aiming) {
			ApplyFirstPersonFOV(GetTargetCameraFOV());
		}
	}

	void FOVManager::OnFirstPersonAimFOVChanged(float v)
	{
		Settings::GetSingleton()->firstPersonAimFOV.store(v);
		Settings::GetSingleton()->Save();
		if (IsPlayerInIronSights() && Settings::GetSingleton()->enableFirstPersonAimFOV.load()) {
			ApplyFirstPersonFOV(v);
		}
	}

	void FOVManager::OnThirdPersonAimFOVChanged(float v)
	{
		Settings::GetSingleton()->thirdPersonAimFOV.store(v);
		Settings::GetSingleton()->Save();
		ApplyThirdPersonAimFOV(v);
	}

	// ============================================================
	// Apply ALL settings (game load, retries, manual refresh)
	// ============================================================
	void FOVManager::ApplyAllSettings()
	{
		auto* s = Settings::GetSingleton();

		// Camera (1st-person world) FOV. This is the value FPInertia's
		// WeaponFOV picks up as the Y-arg of `fov X Y`, so we set it
		// FIRST.
		ApplyFirstPersonFOV(s->firstPersonFOV.load());

		// 3rd-person world FOV.
		ApplyThirdPersonFOV(s->thirdPersonFOV.load());

		// 3rd-person ADS aim FOV (camera-collection setting).
		ApplyThirdPersonAimFOV(s->thirdPersonAimFOV.load());

		// Camera near distance (clip plane). Original mod calls this
		// "Camera Distance" - it's actually fNearDistance.
		ApplyCameraDistance(s->cameraDistance.load());

		// Viewmodel FOV - context-aware (defaults to s->viewmodelFOV).
		ApplyViewmodelFOV(GetTargetViewmodelFOV());

		// Tell FPInertia to re-read its defaults so its WBFOV agrees with
		// our refreshed viewmodel value.
		NotifyFPInertia();
	}

	// ============================================================
	// Game-load retry loop
	// ============================================================
	//
	// Why this is so aggressive:
	//   The engine re-initializes the first-person camera FOV some time
	//   AFTER kPostLoadGame fires (it reads camera state from the save,
	//   then re-applies it during the first ~100-300 ms post-load). The
	//   plain "apply once and retry every 500 ms" approach the original
	//   Papyrus mod used leaves a clearly visible FOV pop in that gap.
	//
	//   The fix is a frame-paced micro-burst: re-assert the camera FOV
	//   every ~8 ms (≈1 frame at 120 fps, 2 frames at 60 fps) for the
	//   first 500 ms after load. Camera FOV is a memory write to
	//   INIPrefSettingCollection, so hammering it costs essentially
	//   nothing - and it guarantees the renderer reads OUR value on
	//   every frame in that window, making the pop imperceptible.
	//
	//   Viewmodel FOV (the `fov X Y` console command) is expensive
	//   because it goes through ScriptCompiler. We apply it once at the
	//   start of the burst, once at the end, and then again as part of
	//   the slower phase-2 retries. The viewmodel is also occluded by
	//   the load-screen fade for most of the burst so a delayed apply
	//   isn't visible anyway.
	void FOVManager::ScheduleLoadRetry()
	{
		// Bump the generation so any in-flight interpolation can be
		// canceled before the retries start fighting it.
		++interpGeneration;

		std::thread([this]() {
			auto* s = Settings::GetSingleton();

			// ---- PHASE 1: frame-paced burst ----
			// 8 ms steps = ~1 frame at 120 fps. Run for the full
			// `iLoadBurstDurationMs` window (default 500 ms), which
			// covers the engine's typical 100-300 ms re-init pass plus
			// safety margin.
			const auto burstStart = std::chrono::steady_clock::now();
			const auto burstLen   = std::chrono::milliseconds(
				std::max(50, s->loadBurstDurationMs.load()));
			const auto burstStep  = std::chrono::milliseconds(
				std::max(1, s->loadBurstStepMs.load()));

			// One viewmodel apply at the *start* of the burst so
			// FPInertia and the engine see our viewmodel FOV before
			// the load-screen fade ends. lastAppliedCamera is reset
			// here so the dedup check inside ApplyViewmodelFOV doesn't
			// suppress the apply on the next call.
			lastAppliedCamera.store(-1.0f);
			lastAppliedViewmodel.store(-1.0f);
			ApplyViewmodelFOV(GetTargetViewmodelFOV());

			while (std::chrono::steady_clock::now() - burstStart < burstLen) {
				// These four are all cheap memory writes to the engine's
				// in-memory INI / GameSetting collections - no script
				// compile, no IO, no allocation. Spamming is fine.
				ApplyFirstPersonFOV(s->firstPersonFOV.load());
				ApplyThirdPersonFOV(s->thirdPersonFOV.load());
				ApplyThirdPersonAimFOV(s->thirdPersonAimFOV.load());
				ApplyCameraDistance(s->cameraDistance.load());
				std::this_thread::sleep_for(burstStep);
			}

			// One viewmodel apply at the *end* of the burst as well -
			// in case FPInertia / another mod issued `fov X Y` during
			// the burst window and overwrote our hands FOV.
			lastAppliedCamera.store(-1.0f);
			lastAppliedViewmodel.store(-1.0f);
			ApplyViewmodelFOV(GetTargetViewmodelFOV());
			NotifyFPInertia();

			logger::trace("[FOVSlider] Load burst complete ({} ms, ~{} applies)",
				static_cast<long long>(burstLen.count()),
				static_cast<long long>(burstLen.count() / std::max<long long>(1, burstStep.count())));

			// ---- PHASE 2: slow safety-net retries ----
			// Catches delayed engine re-init passes (cell load triggers,
			// scripted intro sequences) that happen well after the
			// initial save-load. Cheap to keep around, no perf concern.
			const int   count    = std::max(1, s->loadRetryCount.load());
			const float interval = std::max(0.05f, s->loadRetryInterval.load());
			for (int i = 0; i < count; ++i) {
				std::this_thread::sleep_for(std::chrono::duration<float>(interval));
				ApplyAllSettings();
				logger::trace("[FOVSlider] Game-load safety retry {}/{} applied", i + 1, count);
			}
		}).detach();
	}

	// ============================================================
	// Context transitions
	// ============================================================
	void FOVManager::OnPipBoyOpening()
	{
		auto* s = Settings::GetSingleton();
		// In Power Armor the pipboy is a HUD overlay that doesn't change
		// the viewmodel - skip the override (matches the original mod).
		if (IsPlayerInPowerArmor()) return;

		std::lock_guard lock(transitionMtx);
		context.store(FOVContext::PipBoy);
		// Lock FIRST so FPInertia stops applying before our lerp begins.
		NotifyFPInertiaLock(true);

		const float fromVm = s->viewmodelFOV.load();
		const float toVm   = s->pipBoyFOV.load();
		InterpolateViewmodelFOV(fromVm, toVm, s->interpFramesFast.load());

		// Re-assert 3rd-person FOV in case anything reset it.
		ApplyThirdPersonFOV(s->thirdPersonFOV.load());
	}

	void FOVManager::OnPipBoyClosing()
	{
		auto* s = Settings::GetSingleton();
		if (IsPlayerInPowerArmor()) return;

		std::lock_guard lock(transitionMtx);
		context.store(FOVContext::Default);

		const float fromVm = s->pipBoyFOV.load();
		const float toVm   = s->viewmodelFOV.load();
		InterpolateViewmodelFOV(fromVm, toVm, s->interpFrames.load());

		// On close, the engine sometimes momentarily resets the camera
		// FOV - reassert 1st- and 3rd-person values.
		ApplyFirstPersonFOV(s->firstPersonFOV.load());
		ApplyThirdPersonFOV(s->thirdPersonFOV.load());

		// Unlock AFTER the lerp finishes so FPInertia doesn't try to
		// apply the per-weapon override mid-transition. The lerp runs
		// on a worker thread (~8 ms / step) - wait for that plus a
		// 50 ms safety margin before handing the FOV back to FPInertia.
		NotifyFPInertia();  // make sure FPInertia has up-to-date defaults
		const int lerpMs = std::max(0, s->interpFrames.load()) * 8 + 50;
		ScheduleFPInertiaUnlock(lerpMs);
	}

	void FOVManager::OnTerminalEntered()
	{
		auto* s = Settings::GetSingleton();
		std::lock_guard lock(transitionMtx);
		context.store(FOVContext::Terminal);
		NotifyFPInertiaLock(true);

		const float fromVm = s->viewmodelFOV.load();
		const float toVm   = s->terminalFOV.load();
		InterpolateViewmodelFOV(fromVm, toVm, s->interpFrames.load());
	}

	void FOVManager::OnTerminalExited()
	{
		auto* s = Settings::GetSingleton();
		std::lock_guard lock(transitionMtx);
		context.store(FOVContext::Default);

		const float fromVm = s->terminalFOV.load();
		const float toVm   = s->viewmodelFOV.load();
		InterpolateViewmodelFOV(fromVm, toVm, s->interpFrames.load());

		// Re-assert camera FOVs - GetUp animations sometimes nudge them.
		ApplyFirstPersonFOV(s->firstPersonFOV.load());
		ApplyThirdPersonFOV(s->thirdPersonFOV.load());

		NotifyFPInertia();
		const int lerpMs = std::max(0, s->interpFrames.load()) * 8 + 50;
		ScheduleFPInertiaUnlock(lerpMs);
	}

	void FOVManager::OnSightedStateEnter()
	{
		auto* s = Settings::GetSingleton();
		if (!s->enableFirstPersonAimFOV.load()) return;

		std::lock_guard lock(transitionMtx);
		// Don't override an already-active overlay (PipBoy/Terminal).
		if (context.load() != FOVContext::Default) return;
		context.store(FOVContext::Aiming);
		// We change the camera FOV during ADS, not the viewmodel FOV,
		// so we DON'T need to lock FPInertia (it doesn't touch camera
		// FOV). Leaving FPInertia free during ADS preserves per-weapon
		// viewmodel FOV overrides while the user is aiming.

		// Ironsight FOV is a camera-FOV change, not a viewmodel-FOV
		// change. Smooth it ourselves so we get a nicer ADS-snap than the
		// engine's instant jump.
		const float from = s->firstPersonFOV.load();
		const float to   = s->firstPersonAimFOV.load();
		const int   steps = std::max(2, s->interpFramesFast.load());

		const std::uint64_t myGen = ++interpGeneration;
		std::thread([from, to, steps, myGen, this]() {
			const float delta = to - from;
			for (int i = 0; i < steps; ++i) {
				if (interpGeneration.load() != myGen) return;
				const float t   = static_cast<float>(i + 1) / static_cast<float>(steps);
				const float cur = from + delta * t;
				SetEngineFloatSetting("fDefault1stPersonFOV:Display", cur);
				std::this_thread::sleep_for(std::chrono::milliseconds(6));
			}
			if (interpGeneration.load() == myGen) {
				SetEngineFloatSetting("fDefault1stPersonFOV:Display", to);
			}
		}).detach();
	}

	void FOVManager::OnSightedStateExit()
	{
		auto* s = Settings::GetSingleton();
		if (!s->enableFirstPersonAimFOV.load()) return;

		std::lock_guard lock(transitionMtx);
		// Only reset if we're still in Aiming context - PipBoy/Terminal
		// might have taken over via a separate transition.
		if (context.load() == FOVContext::Aiming) {
			context.store(FOVContext::Default);
		}

		const float from = s->firstPersonAimFOV.load();
		const float to   = s->firstPersonFOV.load();
		const int   steps = std::max(2, s->interpFramesFast.load());

		const std::uint64_t myGen = ++interpGeneration;
		std::thread([from, to, steps, myGen]() {
			const float delta = to - from;
			for (int i = 0; i < steps; ++i) {
				if (FOVManager::GetSingleton()->interpGeneration.load() != myGen) return;
				const float t   = static_cast<float>(i + 1) / static_cast<float>(steps);
				const float cur = from + delta * t;
				FOVManager::SetEngineFloatSetting("fDefault1stPersonFOV:Display", cur);
				std::this_thread::sleep_for(std::chrono::milliseconds(6));
			}
			if (FOVManager::GetSingleton()->interpGeneration.load() == myGen) {
				FOVManager::SetEngineFloatSetting("fDefault1stPersonFOV:Display", to);
			}
		}).detach();
	}

	// ============================================================
	// FPInertia handshake
	// ============================================================
	void FOVManager::NotifyFPInertia()
	{
		// Best-effort - if FPInertia isn't loaded the dispatch is a no-op.
		auto* msg = F4SE::GetMessagingInterface();
		if (!msg) return;
		msg->Dispatch(kFPInertia_RefreshMsg, nullptr, 0, "FPInertia");
	}

	void FOVManager::NotifyFPInertiaLock(bool locked)
	{
		auto* msg = F4SE::GetMessagingInterface();
		if (!msg) return;
		std::uint8_t payload = locked ? 1u : 0u;
		msg->Dispatch(kFPInertia_LockMsg, &payload, sizeof(payload), "FPInertia");
	}

	void FOVManager::ScheduleFPInertiaUnlock(int ms)
	{
		std::thread([ms]() {
			if (ms > 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(ms));
			}
			FOVManager::GetSingleton()->NotifyFPInertiaLock(false);
		}).detach();
	}
}
