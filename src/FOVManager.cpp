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

	// Helper for the diagnostics log. Free function so both
	// FOVManager::LogEngineSnapshot and the drift watcher can use it.
	static const char* ContextToString(FOVContext c)
	{
		switch (c) {
			case FOVContext::Default:  return "Default";
			case FOVContext::PipBoy:   return "PipBoy";
			case FOVContext::Terminal: return "Terminal";
			case FOVContext::Aiming:   return "Aiming";
			default:                   return "?";
		}
	}

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

	bool FOVManager::SetEngineFloatSetting(const char* a_key, float a_value, const char* a_caller)
	{
		// Capture the prior value so the diagnostics log shows
		// before/after - lets us see if a "redundant" write was
		// actually correcting drift introduced by another agent.
		float prior = std::numeric_limits<float>::quiet_NaN();
		(void)TryReadEngineFloatSetting(a_key, prior);

		const auto* settings = Settings::GetSingleton();
		const bool  shouldLog = settings->logEveryEngineWrite.load();

		// Try INIPrefSettingCollection first (the Papyrus Utility::SetINIFloat
		// path also resolves here for "*:Display" keys).
		bool ok = false;
		const char* hitCollection = "?";

		if (auto* prefs = RE::INIPrefSettingCollection::GetSingleton()) {
			if (auto* s = prefs->GetSetting(a_key); s && s->GetType() == RE::Setting::SETTING_TYPE::kFloat) {
				s->SetFloat(a_value);
				ok = true;
				hitCollection = "Pref";
			}
		}
		if (!ok) {
			if (auto* ini = RE::INISettingCollection::GetSingleton()) {
				if (auto* s = ini->GetSetting(a_key); s && s->GetType() == RE::Setting::SETTING_TYPE::kFloat) {
					s->SetFloat(a_value);
					ok = true;
					hitCollection = "INI";
				}
			}
		}
		if (!ok) {
			if (auto* gs = RE::GameSettingCollection::GetSingleton()) {
				for (auto& kv : gs->settings) {
					if (kv.second && kv.second->GetKey() == a_key &&
					    kv.second->GetType() == RE::Setting::SETTING_TYPE::kFloat) {
						kv.second->SetFloat(a_value);
						ok = true;
						hitCollection = "GameSetting";
						break;
					}
				}
			}
		}

		if (!ok) {
			logger::warn("[FOVSlider] [{}] Engine setting '{}' not found - couldn't apply {:.2f}",
				a_caller, a_key, a_value);
			return false;
		}

		if (shouldLog) {
			logger::info("[FOVSlider] [{}] WRITE {} {:.2f} -> {:.2f} ({})",
				a_caller, a_key, prior, a_value, hitCollection);
		}
		return true;
	}

	// ============================================================
	// PlayerCamera runtime FOV (the actually-rendered values)
	// ============================================================
	//
	// PlayerCamera carries its own `worldFOV` (3rd-person) and
	// `firstPersonFOV` floats at offsets 0x168 / 0x16C. The renderer
	// reads THESE every frame; the INI keys we usually write
	// (`fDefault1stPersonFOV:Display`, etc.) are merely *sources* the
	// engine copies into these runtime fields on certain transitions
	// (camera mode change, save load, scripted FOV adjusts, etc.).
	//
	// That decoupling is why a "smooth lerp on the INI value" looks
	// like a snap to the user: the lerp's per-step INI writes don't
	// reach the renderer until something issues `fov X Y`, at which
	// point the runtime jumps from its last-set value straight to
	// the lerp's final value.
	//
	// Writing the runtime fields directly is therefore not the same
	// kind of "engine hook" you'd expect (no detour, no Address
	// Library lookup) - it's just a direct memory write to a public
	// member CommonLibF4 already exposes. Cost: one branch + two
	// 4-byte stores. Visible effect: the renderer picks up the new
	// value on its next frame.
	//
	// Caveat: writing `firstPersonFOV` also drags the viewmodel
	// projection along until something issues `fov X Y` to
	// re-decouple them. We accept this for short lerps because the
	// alternative (snap the camera 30 deg) is worse than briefly
	// having the viewmodel match the camera. The lerp's final
	// ApplyViewmodelFOV() reasserts the user's preferred viewmodel
	// FOV.
	bool FOVManager::WriteRuntimeCameraFOV(float a_firstPersonFOV,
	                                       float a_worldFOV,
	                                       const char* a_caller)
	{
		// FPInertia ownership gate. When FPInertia is loaded it issues
		// `fov X Y` on a ~1.5 s timedReapply for WBFOV, which clobbers
		// PlayerCamera::firstPersonFOV / worldFOV to the X arg (engine
		// quirk - the Y arg only updates the INI). FPInertia is responsible
		// for undoing that runtime clobber after each `fov X Y` to keep
		// the world camera at the user's preferred value.
		//
		// If WE also write these fields, two things go wrong:
		//   1. Our drift watcher catches FPInertia's clobber, lerps
		//      runtime back to our saved camera FOV, and triggers
		//      another `fov X Y` cycle - the visible "every 1.5 s
		//      the camera ramps 30 -> 105 again" feedback loop.
		//   2. The runtime camera write drags the viewmodel projection
		//      with it (the engine couples viewmodel rendering to
		//      PlayerCamera::firstPersonFOV until `fov X Y` re-decouples
		//      them), which the user perceives as WBFOV being stomped.
		//
		// So when FPInertia is loaded, all our runtime writes are
		// no-ops. The drift watcher still flags drift to the log
		// (visibility) but auto-correct is also a no-op.
		if (FOVManager::GetSingleton()->fpInertiaPresent.load()) {
			if (Settings::GetSingleton()->logEveryEngineWrite.load()) {
				logger::trace("[FOVSlider] [{}] WriteRuntimeCameraFOV skipped (FPInertia owns runtime)", a_caller);
			}
			return false;
		}

		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera) return false;

		const bool wantWriteFirst = !std::isnan(a_firstPersonFOV);
		const bool wantWriteThird = !std::isnan(a_worldFOV);
		if (!wantWriteFirst && !wantWriteThird) return true;

		const bool shouldLog = Settings::GetSingleton()->logEveryEngineWrite.load();

		const float priorFirst = camera->firstPersonFOV;
		const float priorThird = camera->worldFOV;

		if (wantWriteFirst) camera->firstPersonFOV = a_firstPersonFOV;
		if (wantWriteThird) camera->worldFOV       = a_worldFOV;

		if (shouldLog) {
			if (wantWriteFirst && wantWriteThird) {
				logger::info("[FOVSlider] [{}] WRITE PlayerCamera 1stP {:.2f}->{:.2f} 3rdP {:.2f}->{:.2f}",
					a_caller, priorFirst, a_firstPersonFOV, priorThird, a_worldFOV);
			} else if (wantWriteFirst) {
				logger::info("[FOVSlider] [{}] WRITE PlayerCamera::firstPersonFOV {:.2f}->{:.2f}",
					a_caller, priorFirst, a_firstPersonFOV);
			} else {
				logger::info("[FOVSlider] [{}] WRITE PlayerCamera::worldFOV {:.2f}->{:.2f}",
					a_caller, priorThird, a_worldFOV);
			}
		}
		return true;
	}

	bool FOVManager::ReadRuntimeCameraFOV(float& a_firstPersonFOV, float& a_worldFOV)
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (!camera) return false;
		a_firstPersonFOV = camera->firstPersonFOV;
		a_worldFOV       = camera->worldFOV;
		return true;
	}

	// ============================================================
	// Runtime-only camera FOV lerp
	// ============================================================
	//
	// Used by the drift watcher when something else clobbered
	// PlayerCamera::firstPersonFOV / worldFOV (typically FPInertia's
	// `fov X Y` for per-weapon WBFOV - the engine quirk sets BOTH
	// camera fields to X regardless of Y, and FPInertia has no
	// runtime undo). We need to pull the camera back to the saved
	// target smoothly, but going through ApplyFirstPersonFOV /
	// ApplyThirdPersonFOV (which write INI too) and especially
	// LerpAllSettings (which finishes with ApplyViewmodelFOV +
	// NotifyFPInertia) creates a feedback loop:
	//   our `fov X Y` clobbers runtime -> our undo fixes -> FSRF ->
	//   FPInertia re-applies WBFOV -> runtime clobbered again ->
	//   watcher detects -> repeat forever.
	//
	// This function ONLY writes PlayerCamera runtime fields. It does
	// not touch INI (FPInertia already fixed those after its
	// `fov X Y`), it does not touch viewmodel (FPInertia owns that
	// per weapon), and it does not dispatch FSRF (which would
	// re-trigger the loop).
	void FOVManager::SmoothCorrectRuntimeFOV(int durationMs, int stepMs)
	{
		const std::uint64_t myGen = ++interpGeneration;

		std::thread([this, myGen, durationMs, stepMs]() {
			auto* s = Settings::GetSingleton();

			activeLerps.fetch_add(1);
			LogEngineSnapshot("RuntimeCorrect/start");

			float startFirst = std::numeric_limits<float>::quiet_NaN();
			float startThird = std::numeric_limits<float>::quiet_NaN();
			(void)ReadRuntimeCameraFOV(startFirst, startThird);

			// If the runtime is unreadable, fall back to instant write
			// (we still want to push the saved value into the camera).
			if (std::isnan(startFirst) || std::isnan(startThird)) {
				WriteRuntimeCameraFOV(s->firstPersonFOV.load(),
					s->thirdPersonFOV.load(),
					"RuntimeCorrect/instant");
				LogEngineSnapshot("RuntimeCorrect/end");
				activeLerps.fetch_sub(1);
				return;
			}

			const float targetFirst = s->firstPersonFOV.load();
			const float targetThird = s->thirdPersonFOV.load();

			const auto t0       = std::chrono::steady_clock::now();
			const auto duration = std::chrono::milliseconds(std::max(8, durationMs));
			const auto step     = std::chrono::milliseconds(std::max(1, stepMs));
			const float durMsF  = static_cast<float>(duration.count());

			while (true) {
				if (interpGeneration.load() != myGen) {
					activeLerps.fetch_sub(1);
					return;
				}

				const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - t0);
				if (elapsed >= duration) break;

				const float t = std::clamp(static_cast<float>(elapsed.count()) / durMsF, 0.0f, 1.0f);
				const float fp = startFirst + (targetFirst - startFirst) * t;
				const float wp = startThird + (targetThird - startThird) * t;
				// Pure runtime write - no INI, no fov X Y, no FSRF.
				WriteRuntimeCameraFOV(fp, wp, "RuntimeCorrect/step");
				std::this_thread::sleep_for(step);
			}

			// Final exact write so float drift can't leave us short.
			WriteRuntimeCameraFOV(targetFirst, targetThird, "RuntimeCorrect/final");

			LogEngineSnapshot("RuntimeCorrect/end");
			activeLerps.fetch_sub(1);
		}).detach();
	}

	bool FOVManager::ExecuteConsoleCommand(std::string_view a_command, const char* a_caller)
	{
		// Same recipe as FPInertia's WeaponFOV.cpp:
		//   1. ConcreteFormFactory<RE::Script>::Create()
		//   2. SetText(command)
		//   3. CompileAndRun with a real ScriptCompiler instance and
		//      kSystemWindow compiler-name. nullptr / kDefault silently no-op.
		//   4. Suppress the console-history line so we don't spam.
		auto* factory = RE::ConcreteFormFactory<RE::Script>::GetFormFactory();
		if (!factory) {
			logger::warn("[FOVSlider] [{}] Script form factory unavailable - cannot run '{}'", a_caller, a_command);
			return false;
		}
		auto* script = factory->Create();
		if (!script) {
			logger::warn("[FOVSlider] [{}] Script create failed - cannot run '{}'", a_caller, a_command);
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
			logger::warn("[FOVSlider] [{}] Failed to compile '{}'", a_caller, a_command);
			return false;
		}

		if (Settings::GetSingleton()->logEveryConsoleCommand.load()) {
			logger::info("[FOVSlider] [{}] EXEC `{}`", a_caller, a_command);
		}
		return true;
	}

	// ============================================================
	// Snapshot diagnostics - the single most useful tool for
	// reconstructing what happened to the engine FOV settings during
	// a failed transition. Reads all four FOV-related INI values
	// and dumps them next to our saved values, prefixed with `phase`.
	// ============================================================
	void FOVManager::LogEngineSnapshot(const char* a_phase) const
	{
		const auto* s = Settings::GetSingleton();

		float e1stFOV = std::numeric_limits<float>::quiet_NaN();
		float eWorldFOV = std::numeric_limits<float>::quiet_NaN();
		float eAim3rdFOV = std::numeric_limits<float>::quiet_NaN();
		float eNearDist  = std::numeric_limits<float>::quiet_NaN();

		const bool h1   = TryReadEngineFloatSetting("fDefault1stPersonFOV:Display", e1stFOV);
		const bool h3   = TryReadEngineFloatSetting("fDefaultWorldFOV:Display",     eWorldFOV);
		const bool ha3  = TryReadEngineFloatSetting("f3rdPersonAimFOV:Camera",      eAim3rdFOV);
		const bool hnd  = TryReadEngineFloatSetting("fNearDistance:Display",        eNearDist);

		const float saved1   = s->firstPersonFOV.load();
		const float saved3   = s->thirdPersonFOV.load();
		const float savedAim = s->thirdPersonAimFOV.load();
		const float savedND  = s->cameraDistance.load();

		// Also read PlayerCamera runtime fields (the actually-rendered
		// values). When INI != runtime the user sees the runtime value;
		// they're the canonical "what's on screen" answer.
		float runFirst = std::numeric_limits<float>::quiet_NaN();
		float runThird = std::numeric_limits<float>::quiet_NaN();
		const bool hr = ReadRuntimeCameraFOV(runFirst, runThird);

		// Log on a single line per setting so the user can trivially
		// grep e.g. "1stPersonFOV" and see the timeline.
		logger::info("[FOVSlider] SNAPSHOT [{}] 1stPersonFOV: saved={:.2f} ini={} runtime={} ctx={} ",
			a_phase, saved1,
			h1 ? std::format("{:.2f}", e1stFOV) : "<missing>",
			hr ? std::format("{:.2f}", runFirst) : "<missing>",
			ContextToString(context.load()));
		logger::info("[FOVSlider] SNAPSHOT [{}] WorldFOV(3rd):  saved={:.2f} ini={} runtime={}",
			a_phase, saved3,
			h3 ? std::format("{:.2f}", eWorldFOV) : "<missing>",
			hr ? std::format("{:.2f}", runThird) : "<missing>");
		logger::info("[FOVSlider] SNAPSHOT [{}] 3rdPersonAim:   saved={:.2f} ini={}",
			a_phase, savedAim,
			ha3 ? std::format("{:.2f}", eAim3rdFOV) : "<missing>");
		logger::info("[FOVSlider] SNAPSHOT [{}] NearDist:       saved={:.2f} ini={}",
			a_phase, savedND,
			hnd ? std::format("{:.2f}", eNearDist) : "<missing>");
		logger::info("[FOVSlider] SNAPSHOT [{}] LastAppliedFov: vmX={:.2f} camY={:.2f}",
			a_phase, lastAppliedViewmodel.load(), lastAppliedCamera.load());
	}

	// ============================================================
	// Initialization
	// ============================================================
	void FOVManager::Init()
	{
		context.store(FOVContext::Default);
		lastAppliedViewmodel.store(-1.0f);
		lastAppliedCamera.store(-1.0f);

		// Initial snapshot - shows what state the engine is in BEFORE
		// we apply anything on first kGameDataReady. If the engine
		// reports 90 here even though our saved is 105, we know the
		// engine pre-init defaults are at play.
		LogEngineSnapshot("Init");

		// Spawn the drift watcher (controlled by INI; 0 = disabled).
		const auto* settings = Settings::GetSingleton();
		const int   interval = settings->driftWatchIntervalMs.load();
		if (interval > 0 && !driftWatcherStarted.exchange(true)) {
			std::thread([this]() { RunDriftWatcher(); }).detach();
			logger::info("[FOVSlider] Drift watcher started (cold={} ms, hot={} ms, hotDuration={} ms)",
				interval,
				settings->driftWatchHotIntervalMs.load(),
				settings->driftWatchHotDurationMs.load());
		}
	}

	// ============================================================
	// Drift watcher hot-mode trigger
	// ============================================================
	//
	// Engages the hot-poll mode for `durationMs` ms. Called by event
	// handlers immediately after menu-close events that the diagnostic
	// log identified as triggers for engine FOV restores:
	//   - LoadingMenu close: engine writes 75 to all FOVs ~700 ms later
	//   - FaderMenu close:   engine writes 90/70 ~470 ms later
	//   - ExamineMenu close: engine writes 90/70 ~1.7 s later
	//
	// The deadline is set to MAX(existing, now + durationMs) so
	// back-to-back triggers extend the window without ever shortening
	// it. Calling this with durationMs <= 0 is a no-op (and lets a
	// caller "disable" the next trigger by passing 0 if they later
	// decide they don't want it).
	void FOVManager::TriggerDriftHotMode(int durationMs)
	{
		if (durationMs <= 0) return;
		// Clamp to at least 100 ms so a typo doesn't render the API
		// useless. Upper bound is open - the cost is one extra atomic
		// load + compare per poll.
		durationMs = std::max(100, durationMs);

		const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		const auto wantDeadline = nowMs + static_cast<std::int64_t>(durationMs);

		// CAS-loop to set deadline = max(existing, wantDeadline).
		std::int64_t cur = driftHotDeadlineMs.load();
		while (cur < wantDeadline &&
		       !driftHotDeadlineMs.compare_exchange_weak(cur, wantDeadline)) {
			// `cur` was reloaded by compare_exchange_weak on failure;
			// loop again with the fresh value.
		}

		logger::info("[FOVSlider] Drift watcher: hot mode requested for {} ms (deadline={} ms)",
			durationMs, wantDeadline);
	}

	// ============================================================
	// Drift watcher
	//
	// Polls `fDefault1stPersonFOV:Display` every iDriftWatchIntervalMs
	// (default 250 ms). When the engine value differs from our saved
	// firstPersonFOV by > 0.5 deg, we:
	//   1. WARN-log the drift with a one-line summary AND a full
	//      snapshot (only on transition into drift, not every tick).
	//   2. If bDriftAutoCorrect is true (default), kick off a smooth
	//      LerpAllSettings(iDriftCorrectDurationMs) to ease the
	//      engine values back to saved. This is what defeats the
	//      engine's post-LoadingMenu/FaderMenu FOV restore - which
	//      fires 2-4 seconds AFTER our scheduled retries end (proven
	//      via diagnostic logs) - and any external mod that writes
	//      the FOV behind our back. The lerp shape matches Pip-Boy /
	//      iron sights / game-load so the user sees a brief FOV ease
	//      back to target instead of a snap.
	//
	// Suppressions:
	//  - We DON'T flag drift while context == Aiming, because Aiming
	//    intentionally drives a different camera FOV.
	//  - We DO flag/correct drift while in PipBoy or Terminal (the
	//    camera FOV should still match the user's preference, only
	//    the viewmodel FOV is overlaid in those contexts).
	//  - We skip drift detection entirely while activeLerps > 0 so
	//    the lerp's own mid-transition writes (e.g. 92 deg while
	//    lerping 90 -> 105) don't look like drift.
	//  - We dedupe consecutive identical drift values so a stable-
	//    but-wrong engine value logs ONCE, not every poll.
	//  - After auto-correcting, we suppress polls for ceil(durMs/
	//    interval)+1 cycles so the lerp finishes AND FPInertia has
	//    one full poll to read back the corrected value.
	// ============================================================
	void FOVManager::RunDriftWatcher()
	{
		float    lastDriftEngineValue = std::numeric_limits<float>::quiet_NaN();
		bool     wasDrifting          = false;
		int      suppressCycles       = 0;
		bool     wasHotLastTick       = false;

		const auto nowMs = []() {
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
		};

		while (!driftWatcherStop.load()) {
			auto* settings = Settings::GetSingleton();

			const int  coldInterval = settings->driftWatchIntervalMs.load();
			const int  hotInterval  = std::max(1, settings->driftWatchHotIntervalMs.load());
			const bool inHot        = nowMs() < driftHotDeadlineMs.load();

			if (coldInterval <= 0 && !inHot) {
				// Watcher disabled and not in a hot window - sleep
				// briefly and re-check (so toggling the setting at
				// runtime doesn't require a restart).
				std::this_thread::sleep_for(std::chrono::milliseconds(500));
				continue;
			}

			const int interval = inHot ? hotInterval : std::max(1, coldInterval);
			std::this_thread::sleep_for(std::chrono::milliseconds(interval));

			// Log transitions in/out of hot mode so the user can
			// correlate detection latency with menu lifecycle.
			if (inHot != wasHotLastTick) {
				if (inHot) {
					logger::info("[FOVSlider] Drift watcher: HOT mode engaged (poll {} ms)", hotInterval);
				} else {
					logger::info("[FOVSlider] Drift watcher: HOT mode ended, back to cold poll ({} ms)", coldInterval);
				}
				wasHotLastTick = inHot;
			}

			if (suppressCycles > 0) {
				--suppressCycles;
				continue;
			}

			// Skip while a lerp (load burst, drift auto-correct,
			// safety retry) is in flight - the lerp's mid-transition
			// writes are not "drift", they're our own intentional
			// smooth values. Without this check, polling at 250 ms
			// during a 250 ms lerp would see e.g. engine=98 vs
			// saved=105 and trigger a recursive auto-correct.
			if (activeLerps.load() > 0) {
				continue;
			}

			// Aiming intentionally drives a different camera FOV;
			// PipBoy / Terminal don't (only the viewmodel is overlaid
			// in those contexts), so we DO check for camera-FOV drift
			// there.
			if (context.load() == FOVContext::Aiming) {
				wasDrifting          = false;
				lastDriftEngineValue = std::numeric_limits<float>::quiet_NaN();
				continue;
			}

			// Read BOTH the INI setting (engine's source-of-truth) and
			// the PlayerCamera runtime field (what the renderer actually
			// uses). Either drifting from saved is "real" drift the user
			// will eventually see, but the runtime field is the more
			// directly visible one.
			//
			// NOTE: when FPInertia is loaded, runtime drift is EXPECTED
			// every ~1.5 s (FPInertia's timedReapply issues `fov X Y`
			// for WBFOV, which clobbers PlayerCamera::firstPersonFOV
			// to the X arg). FPInertia owns the runtime undo in that
			// case. We still want INI drift detection (engine restore
			// on save load can still hit the INI path), so we read
			// INI either way - we just skip the runtime read when
			// FPInertia is present, so the watcher only flags REAL
			// problems (INI drift, which only the engine causes).
			float eFov     = 0.0f;
			float runFov   = 0.0f;
			float runThird = 0.0f;
			const bool fpiPresent = fpInertiaPresent.load();
			const bool haveIni = TryReadEngineFloatSetting("fDefault1stPersonFOV:Display", eFov);
			const bool haveRun = !fpiPresent && ReadRuntimeCameraFOV(runFov, runThird);
			if (!haveIni && !haveRun) continue;

			const float saved = Settings::GetSingleton()->firstPersonFOV.load();
			const float deltaIni = haveIni ? std::fabs(eFov - saved)   : 0.0f;
			const float deltaRun = haveRun ? std::fabs(runFov - saved) : 0.0f;

			// Whichever is further off-target drives the report (so the
			// log shows the worse of the two sources).
			const float delta = std::max(deltaIni, deltaRun);
			const float reportedEngine = (deltaRun >= deltaIni && haveRun) ? runFov : eFov;
			const char* source         = (deltaRun >= deltaIni && haveRun) ? "runtime" : "INI";

			if (delta > 0.5f) {
				const bool sameAsLastReport = std::fabs(reportedEngine - lastDriftEngineValue) < 0.05f;
				if (!sameAsLastReport) {
					logger::warn("[FOVSlider] DRIFT detected ({}): 1stPersonFOV saved={:.2f} engine={:.2f} (delta={:.2f}, ini={:.2f} runtime={:.2f})",
						source, saved, reportedEngine, delta,
						haveIni ? eFov : 0.0f, haveRun ? runFov : 0.0f);
					LogEngineSnapshot("DRIFT");
					lastDriftEngineValue = reportedEngine;
				}
				wasDrifting = true;

				// Auto-correct if enabled. We use the SAME smooth lerp
				// shape as Pip-Boy / iron sights / game-load so the
				// user sees a brief FOV ease back to the saved value
				// instead of a snap. The lerp covers 1st- and
				// 3rd-person FOV; the other settings (3rd-person aim
				// FOV, near distance) get an instant re-apply at the
				// start of the lerp because they don't visibly
				// transition.
				if (settings->driftAutoCorrect.load()) {
					const int durMs = std::max(50,
						settings->driftCorrectDurationMs.load());
					if (fpiPresent) {
						// FPInertia present: only the INI source-of-truth
						// is ours to fix. Runtime PlayerCamera fields are
						// FPInertia's responsibility (post-`fov X Y`
						// undo). A snap INI write is enough - the engine
						// re-reads INI on the next camera-mode transition,
						// and FPInertia's next `fov X Y` will pick up the
						// corrected Y arg from our INI.
						logger::warn("[FOVSlider] DRIFT auto-correct (INI-only, FPInertia owns runtime): {:.2f} -> {:.2f}",
							reportedEngine, saved);
						SetEngineFloatSetting("fDefault1stPersonFOV:Display", saved, "DriftAutoCorrect");
						SetEngineFloatSetting("fDefaultWorldFOV:Display",
							Settings::GetSingleton()->thirdPersonFOV.load(),
							"DriftAutoCorrect");
						// Short suppress window so we don't double-fire on
						// the same drift before the engine settles.
						suppressCycles = 2;
					} else {
						logger::warn("[FOVSlider] DRIFT auto-correct: runtime-lerp {:.2f} -> {:.2f} over {} ms",
							reportedEngine, saved, durMs);
						// Use SmoothCorrectRuntimeFOV (NOT LerpAllSettings):
						// runtime-only lerp that does NOT issue `fov X Y`,
						// does NOT touch INI, and does NOT dispatch FSRF.
						// Going through the full apply chain here would
						// create a feedback loop with FPInertia (loop is
						// also avoided structurally by the fpiPresent
						// branch above; this path only runs when FPInertia
						// is absent and we're the sole runtime owner).
						SmoothCorrectRuntimeFOV(durMs, 8);

						// Suppress polls until the lerp definitely
						// finishes AND FPInertia has had a tick to read
						// back the corrected value. ceil(dur / interval)
						// + 1 = at least one full poll past the lerp end.
						// Use the CURRENT interval (could be hot or cold)
						// so the suppress window matches the actual poll
						// rate.
						suppressCycles = (durMs + interval - 1) / interval + 1;
					}
				}
			} else if (wasDrifting) {
				logger::info("[FOVSlider] DRIFT cleared: 1stPersonFOV now ini={:.2f} runtime={:.2f} (saved={:.2f})",
					haveIni ? eFov : 0.0f, haveRun ? runFov : 0.0f, saved);
				wasDrifting          = false;
				lastDriftEngineValue = std::numeric_limits<float>::quiet_NaN();
			}
		}
	}

	// ============================================================
	// Apply primitives
	// ============================================================
	void FOVManager::ApplyFirstPersonFOV(float fov)
	{
		// 1) INI source-of-truth (read by engine on camera-mode
		//    transitions / save load).
		SetEngineFloatSetting("fDefault1stPersonFOV:Display", fov, "ApplyFirstPersonFOV");

		// 2) Runtime PlayerCamera::firstPersonFOV (read by the renderer
		//    every frame). Without this the INI write is INVISIBLE until
		//    something forces the engine to copy INI -> runtime - which
		//    is what was making our smooth lerps look like snaps.
		//
		// Caveat: during ADS the engine drives its own zoom via
		// PlayerCamera::fovAdjustCurrent/Target/PerSec on top of
		// firstPersonFOV. The aim-FOV feature relies on the engine
		// re-reading our INI write into the runtime lerp; if we ALSO
		// write runtime ourselves, we race the engine's adjust system
		// and ADS becomes choppy. So skip the runtime write while
		// context == Aiming - the AimLerp's INI writes still drive
		// the engine's own lerp like they used to.
		if (context.load() != FOVContext::Aiming) {
			WriteRuntimeCameraFOV(fov, std::numeric_limits<float>::quiet_NaN(),
				"ApplyFirstPersonFOV");
		}
	}

	void FOVManager::ApplyThirdPersonFOV(float fov)
	{
		SetEngineFloatSetting("fDefaultWorldFOV:Display", fov, "ApplyThirdPersonFOV");
		// Same reasoning as ApplyFirstPersonFOV - skip the runtime
		// write while aiming. 3rd-person FOV isn't visible during
		// iron sights anyway, and avoiding the write keeps us out
		// of the engine's ADS path entirely.
		if (context.load() != FOVContext::Aiming) {
			WriteRuntimeCameraFOV(std::numeric_limits<float>::quiet_NaN(), fov,
				"ApplyThirdPersonFOV");
		}
	}

	void FOVManager::ApplyThirdPersonAimFOV(float fov)
	{
		// Lives in GameSettingCollection (esm-defined default key).
		SetEngineFloatSetting("f3rdPersonAimFOV:Camera", fov, "ApplyThirdPersonAimFOV");
	}

	void FOVManager::ApplyCameraDistance(float distance)
	{
		SetEngineFloatSetting("fNearDistance:Display", distance, "ApplyCameraDistance");
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
		if (ExecuteConsoleCommand(cmd, "ApplyViewmodelFOV")) {
			lastAppliedViewmodel.store(vmFov);
			lastAppliedCamera.store(camFov);

			// Engine quirk (documented in the Player Height mod's
			// FOVSliderScript.psc): `fov X Y` sets the runtime
			// VIEWMODEL and 3rd-person camera FOV to X, and the
			// 1st-person camera FOV to Y. So after the command runs,
			// `PlayerCamera::worldFOV` has been clobbered to X
			// (= viewmodel value, not the saved 3rd-person FOV).
			//
			// Re-assert the saved 3rd-person FOV here so a 1st->3rd
			// camera transition doesn't briefly show the viewmodel
			// value. The 1st-person re-assert is gated on Aiming
			// because the engine's fovAdjust system owns runtime
			// firstPersonFOV during ADS and our write would race it.
			// 3rd-person FOV isn't ADS-related, so we always restore
			// it (worst case: harmless write while in iron-sights).
			const float savedThird = Settings::GetSingleton()->thirdPersonFOV.load();
			const bool writeFirst = context.load() != FOVContext::Aiming;
			WriteRuntimeCameraFOV(
				writeFirst ? camFov : std::numeric_limits<float>::quiet_NaN(),
				savedThird,
				writeFirst ? "ApplyViewmodelFOV/post-fov" : "ApplyViewmodelFOV/post-fov(aim)");
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

			// Force the runtime camera FOV to refresh immediately via
			// `fov X Y`. Without this, there's a visible 1-frame FOV
			// pop on every slider tick.
			//
			// Why: `ApplyFirstPersonFOV` above writes
			// `fDefault1stPersonFOV:Display` in INIPrefSettingCollection,
			// but the engine doesn't re-sync the runtime camera FOV
			// from INI per frame - only on certain events (camera mode
			// transitions). Until something issues `fov X Y`, the
			// renderer keeps using whatever runtime value was last
			// set, which equals the OLD slider value.
			//
			// FPInertia would normally paper over this for us via its
			// FSRF -> RefreshDefaults -> ApplyViewmodelFOV chain, but
			// that's one frame too late (the message dispatch crosses
			// a frame boundary), and it fires even when the user has
			// WBFOV disabled - which is the "even with WBFOV disabled
			// I still see the pop" symptom users hit.
			//
			// We issue `fov vmCurrent newCam` here, same-frame,
			// directly. Runtime camera = newCam immediately. No pop.
			//
			// Caveat: `fov X Y` resets 3rd-person FOV to X (viewmodel)
			// as a documented engine quirk. We gate this on the active
			// camera state being 1st-person/iron-sights so we don't
			// clobber a visible 3rd-person view; the engine re-reads
			// `fDefaultWorldFOV:Display` on the next 1st->3rd
			// transition so 3rd-person FOV recovers naturally either
			// way.
			if (auto* camera = RE::PlayerCamera::GetSingleton(); camera && camera->currentState) {
				const auto state = camera->currentState->id.get();
				if (state == RE::CameraState::kFirstPerson ||
				    state == RE::CameraState::kIronSights) {
					// Reset the dedup cache for camera so the apply
					// can't be skipped (lastAppliedCamera == old
					// camFov is exactly the case we need to defeat).
					lastAppliedCamera.store(-1.0f);
					ApplyViewmodelFOV(GetTargetViewmodelFOV());
				}
			}
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

		LogEngineSnapshot("ApplyAllSettings/before");

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

		LogEngineSnapshot("ApplyAllSettings/after");
	}

	// ============================================================
	// Smooth lerp re-apply
	// ============================================================
	//
	// Drives a single ramp from the engine's CURRENT 1st-person /
	// 3rd-person FOV values to our saved targets, stepping every
	// `stepMs` ms (default 8 = ~1 frame at 120 fps) for `durationMs`
	// total ms.
	//
	// Per step we write `start + (target - start) * t` for both the
	// camera-collection 1st-person FOV and the world-collection
	// 3rd-person FOV, where t = elapsed / total. This achieves two
	// things at once:
	//   (a) the user sees a smooth FOV ease, matching the Pip-Boy /
	//       Terminal / iron-sights transitions everywhere else in the
	//       plugin;
	//   (b) the high-frequency writes ALSO defeat the engine's
	//       post-load FOV restore (the original "load burst" purpose)
	//       because any stray engine write is overwritten on the next
	//       step ~8 ms later.
	//
	// Settings that don't visibly transition (3rd-person aim FOV,
	// near distance) are applied instantly at the start - reading
	// e.g. fNearDistance and lerping it across 250 ms would just be
	// wasted writes since it doesn't move the camera.
	//
	// The viewmodel `fov X Y` command is heavy (compiles a Papyrus
	// script every call), so we issue it ONCE at the end with the
	// final Y = saved camera FOV. FPInertia then re-reads the engine
	// value on its next tick.
	void FOVManager::LerpAllSettings(int durationMs, int stepMs, bool a_includeViewmodel)
	{
		// Cancel any in-flight interpolation. Anyone holding the old
		// `myGen` will see interpGeneration change and bail.
		const std::uint64_t myGen = ++interpGeneration;

		std::thread([this, myGen, durationMs, stepMs, a_includeViewmodel]() {
			auto* s = Settings::GetSingleton();

			activeLerps.fetch_add(1);
			LogEngineSnapshot("LerpAll/start");

			// Read the engine's CURRENT values so we can lerp from
			// wherever we are. If they're already at target the
			// lerp is a smooth no-op; if the engine reverted us to
			// 90 deg the lerp is a smooth ramp 90 -> target.
			//
			// Prefer the PlayerCamera runtime value over the INI key:
			// the runtime is what the user actually sees on screen,
			// and the engine sometimes diverges the two (e.g. INI=105
			// but runtime=75 right after LoadingMenu close). Lerping
			// from "what the user sees right now" produces a smooth
			// transition; lerping from the INI source-of-truth would
			// look like a snap because the runtime hasn't been updated.
			float startFirst = s->firstPersonFOV.load();
			float startThird = s->thirdPersonFOV.load();
			float runFirst   = std::numeric_limits<float>::quiet_NaN();
			float runThird   = std::numeric_limits<float>::quiet_NaN();
			(void)TryReadEngineFloatSetting("fDefault1stPersonFOV:Display", startFirst);
			(void)TryReadEngineFloatSetting("fDefaultWorldFOV:Display",     startThird);
			if (ReadRuntimeCameraFOV(runFirst, runThird)) {
				if (!std::isnan(runFirst) && runFirst >= 30.0f && runFirst <= 160.0f) {
					startFirst = runFirst;
				}
				if (!std::isnan(runThird) && runThird >= 30.0f && runThird <= 160.0f) {
					startThird = runThird;
				}
			}

			const float targetFirst = s->firstPersonFOV.load();
			const float targetThird = s->thirdPersonFOV.load();

			// Apply non-visible settings instantly at the start so
			// they're correct for the entire lerp window (and any
			// engine re-init that fires mid-lerp can't leave them
			// stale).
			ApplyThirdPersonAimFOV(s->thirdPersonAimFOV.load());
			ApplyCameraDistance(s->cameraDistance.load());

			const auto t0       = std::chrono::steady_clock::now();
			const auto duration = std::chrono::milliseconds(std::max(8, durationMs));
			const auto step     = std::chrono::milliseconds(std::max(1, stepMs));
			const float durMsF  = static_cast<float>(duration.count());

			while (true) {
				if (interpGeneration.load() != myGen) {
					// Superseded - another transition kicked off.
					// Bail out without forcing the final value, since
					// the new transition is already heading somewhere
					// else.
					activeLerps.fetch_sub(1);
					return;
				}

				const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - t0);
				if (elapsed >= duration) break;

				const float t = std::clamp(static_cast<float>(elapsed.count()) / durMsF, 0.0f, 1.0f);
				ApplyFirstPersonFOV(startFirst + (targetFirst - startFirst) * t);
				ApplyThirdPersonFOV(startThird + (targetThird - startThird) * t);
				std::this_thread::sleep_for(step);
			}

			// Final exact apply so floating-point drift can't leave
			// us 0.1 deg short of the target.
			ApplyFirstPersonFOV(targetFirst);
			ApplyThirdPersonFOV(targetThird);

			// Optional finalizer: viewmodel apply + FPInertia refresh.
			// See header docs - Phase 2 load retries pass false to
			// avoid retriggering FPInertia's WBFOV apply (which would
			// clobber the runtime camera fields via the engine's
			// `fov X Y` quirk on every retry).
			if (a_includeViewmodel) {
				lastAppliedCamera.store(-1.0f);
				lastAppliedViewmodel.store(-1.0f);
				ApplyViewmodelFOV(GetTargetViewmodelFOV());
				NotifyFPInertia();
			}

			LogEngineSnapshot(a_includeViewmodel ? "LerpAll/end" : "LerpAll/end(camera-only)");
			activeLerps.fetch_sub(1);
		}).detach();
	}

	// ============================================================
	// Game-load retry loop
	// ============================================================
	//
	// On game load the engine re-initializes the first-person camera
	// FOV some time AFTER kPostLoadGame fires (it reads camera state
	// from the save, then re-applies it during the first ~100-300 ms
	// post-load). The plain "apply once" approach leaves a visible
	// FOV pop in that gap.
	//
	// We hand the load-time apply off to LerpAllSettings, which:
	//   - reads the engine's current camera FOV (whatever the engine
	//     just initialized it to),
	//   - lerps from there to our saved target over `iLoadBurstDurationMs`
	//     (default 500 ms),
	//   - hammers the engine setting every 8 ms in the process,
	//     which is what defeats any concurrent engine writes.
	//
	// The user gets a smooth ease from the engine's restored value to
	// their preferred FOV - same lerp shape as Pip-Boy / iron sights /
	// Terminal transitions - instead of a snap.
	//
	// After the lerp, slower phase-2 retries fire `iRetryCount` times
	// `fRetryIntervalSec` apart. Each retry is also a smooth lerp,
	// which catches delayed engine re-init passes (cell load triggers,
	// scripted intro sequences) without snapping.
	void FOVManager::ScheduleLoadRetry()
	{
		LogEngineSnapshot("ScheduleLoadRetry/queued");

		std::thread([this]() {
			auto* s = Settings::GetSingleton();

			// ---- PHASE 1: smooth load-lerp ----
			LerpAllSettings(
				std::max(50, s->loadBurstDurationMs.load()),
				std::max(1, s->loadBurstStepMs.load()));

			// Wait for the lerp worker to finish so the snapshot
			// below shows post-lerp values, not mid-lerp.
			const auto lerpWait = std::chrono::milliseconds(
				std::max(50, s->loadBurstDurationMs.load()) + 50);
			std::this_thread::sleep_for(lerpWait);

			logger::info("[FOVSlider] Load lerp complete ({} ms)",
				static_cast<long long>(s->loadBurstDurationMs.load()));

			// ---- PHASE 2: slow safety-net retries (each smooth) ----
			// Catches delayed engine re-init passes that happen well
			// after the initial save-load. Each retry is a quick
			// 150 ms lerp instead of a snap so the user never sees a
			// visible FOV pop even if the engine clobbered us in
			// between retries.
			const int   count        = std::max(1, s->loadRetryCount.load());
			const float interval     = std::max(0.05f, s->loadRetryInterval.load());
			constexpr int kRetryLerp = 150;

			for (int i = 0; i < count; ++i) {
				std::this_thread::sleep_for(std::chrono::duration<float>(interval));
				logger::info("[FOVSlider] Game-load safety retry {}/{} (smooth camera-only, {} ms)",
					i + 1, count, kRetryLerp);
				// Camera-only: skip ApplyViewmodelFOV + FSRF on retries.
				// The first phase already set viewmodel + refreshed
				// FPInertia; retries are only here to catch DELAYED
				// engine writes to fDefault1stPersonFOV:Display from
				// cell-load triggers / scripted intros. Re-issuing
				// `fov X Y` on each retry would clobber runtime back
				// to our default vm (engine quirk) and re-trigger
				// FPInertia's WBFOV apply, causing visible camera
				// pops every retry interval.
				LerpAllSettings(kRetryLerp, 8, /*a_includeViewmodel=*/false);
			}
			LogEngineSnapshot("LoadRetry/done");
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
		if (IsPlayerInPowerArmor()) {
			logger::info("[FOVSlider] OnPipBoyOpening (skipped - in PA)");
			return;
		}
		// If the user hasn't configured a distinct Pip-Boy FOV, there's nothing
		// to change. Skip the lock/lerp/unlock entirely so FPInertia remains
		// free to react to weapon swaps done via the Pip-Boy favorites screen.
		const float fromVm = s->viewmodelFOV.load();
		const float toVm   = s->pipBoyFOV.load();
		if (std::fabs(fromVm - toVm) < 0.05f) {
			logger::trace("[FOVSlider] OnPipBoyOpening - skipped (pipBoyFOV == viewmodelFOV)");
			return;
		}

		logger::info("[FOVSlider] OnPipBoyOpening - context Default -> PipBoy");
		LogEngineSnapshot("PipBoyOpening/before");

		std::lock_guard lock(transitionMtx);
		context.store(FOVContext::PipBoy);
		// Lock FIRST so FPInertia stops applying before our lerp begins.
		NotifyFPInertiaLock(true);

		InterpolateViewmodelFOV(fromVm, toVm, s->interpFramesFast.load());

		// Re-assert 3rd-person FOV in case anything reset it.
		ApplyThirdPersonFOV(s->thirdPersonFOV.load());
	}

	void FOVManager::OnPipBoyClosing()
	{
		auto* s = Settings::GetSingleton();
		if (IsPlayerInPowerArmor()) {
			logger::info("[FOVSlider] OnPipBoyClosing (skipped - in PA)");
			return;
		}
		const float fromVm = s->pipBoyFOV.load();
		const float toVm   = s->viewmodelFOV.load();
		if (std::fabs(fromVm - toVm) < 0.05f) {
			// We never entered PipBoy context (open was skipped), so there's
			// nothing to restore. The drift watcher covers any transient engine
			// camera reset that can happen on Pip-Boy close.
			logger::trace("[FOVSlider] OnPipBoyClosing - skipped (pipBoyFOV == viewmodelFOV)");
			return;
		}

		logger::info("[FOVSlider] OnPipBoyClosing - context PipBoy -> Default");
		LogEngineSnapshot("PipBoyClosing/before");

		std::lock_guard lock(transitionMtx);
		context.store(FOVContext::Default);

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
		logger::info("[FOVSlider] OnTerminalEntered - context -> Terminal");
		LogEngineSnapshot("TerminalEntered/before");

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
		logger::info("[FOVSlider] OnTerminalExited - context Terminal -> Default");
		LogEngineSnapshot("TerminalExited/before");

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
		if (!s->enableFirstPersonAimFOV.load()) {
			logger::trace("[FOVSlider] OnSightedStateEnter - skipped (aim FOV disabled)");
			return;
		}

		std::lock_guard lock(transitionMtx);
		// Don't override an already-active overlay (PipBoy/Terminal).
		if (context.load() != FOVContext::Default) {
			logger::info("[FOVSlider] OnSightedStateEnter - skipped (context={})", ContextToString(context.load()));
			return;
		}
		logger::info("[FOVSlider] OnSightedStateEnter - context Default -> Aiming");
		LogEngineSnapshot("SightedEnter/before");
		context.store(FOVContext::Aiming);

		// Lock FPInertia for the duration of ADS. Although ADS only changes
		// the 1st-person camera FOV (INI fDefault1stPersonFOV:Display) and
		// not the viewmodel projection, FPInertia's `fov X Y` reapply also
		// rewrites that same INI key (Y arg + an explicit
		// SetEngineFloatSetting fixup), which would clobber our aim INI
		// value back to the default camera FOV ~1.5 s into ADS. Locking
		// FPInertia keeps it from re-issuing `fov X Y` during ADS so the
		// aim FOV holds for the whole zoom.
		NotifyFPInertiaLock(true);

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
				SetEngineFloatSetting("fDefault1stPersonFOV:Display", cur, "AimLerp(in)");
				std::this_thread::sleep_for(std::chrono::milliseconds(6));
			}
			if (interpGeneration.load() == myGen) {
				SetEngineFloatSetting("fDefault1stPersonFOV:Display", to, "AimLerp(in)Final");
			}
		}).detach();
	}

	void FOVManager::OnSightedStateExit()
	{
		auto* s = Settings::GetSingleton();
		if (!s->enableFirstPersonAimFOV.load()) {
			logger::trace("[FOVSlider] OnSightedStateExit - skipped (aim FOV disabled)");
			return;
		}

		std::lock_guard lock(transitionMtx);
		// Only reset if we're still in Aiming context - PipBoy/Terminal
		// might have taken over via a separate transition.
		const bool wasAiming = (context.load() == FOVContext::Aiming);
		if (wasAiming) {
			logger::info("[FOVSlider] OnSightedStateExit - context Aiming -> Default");
			LogEngineSnapshot("SightedExit/before");
			context.store(FOVContext::Default);
		} else {
			logger::info("[FOVSlider] OnSightedStateExit - context unchanged ({})",
				ContextToString(context.load()));
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
				FOVManager::SetEngineFloatSetting("fDefault1stPersonFOV:Display", cur, "AimLerp(out)");
				std::this_thread::sleep_for(std::chrono::milliseconds(6));
			}
			if (FOVManager::GetSingleton()->interpGeneration.load() == myGen) {
				FOVManager::SetEngineFloatSetting("fDefault1stPersonFOV:Display", to, "AimLerp(out)Final");
			}
		}).detach();

		// Release the FPInertia lock once our exit lerp has finished
		// settling the camera FOV back to the default 1st-person value.
		// We delay the unlock so FPInertia's first post-ADS `fov X Y`
		// observes the final INI value (Y arg = camera FOV) rather than
		// an in-flight intermediate. Only release if we actually held the
		// lock (transitioned out of Aiming) - if PipBoy/Terminal took
		// over mid-aim, that overlay owns the lock now and will release
		// it itself.
		if (wasAiming) {
			const int unlockMs = std::max(0, steps) * 6 + 50;
			ScheduleFPInertiaUnlock(unlockMs);
		}
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
		if (!msg) {
			logger::warn("[FOVSlider] FSLK dispatch failed: MessagingInterface unavailable (locked={})", locked);
			return;
		}
		std::uint8_t payload = locked ? 1u : 0u;
		const bool ok = msg->Dispatch(kFPInertia_LockMsg, &payload, sizeof(payload), "FPInertia");
		logger::info("[FOVSlider] FSLK dispatch -> FPInertia: locked={} ok={}", locked, ok);
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
