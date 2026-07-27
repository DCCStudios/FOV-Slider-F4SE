#pragma once

#include "RE/Fallout.h"

namespace FOVSlider
{
	// Power Armor detection - biped slot 40 ("PA frame") has a parent object
	// when the player is wearing PA. Same heuristic FPInertia uses; no
	// dependency on Papyrus or HasKeyword().
	inline bool IsPlayerInPowerArmor()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) return false;
		if (!player->biped) return false;
		constexpr std::uint32_t kPAFrameSlot = 40;
		return player->biped->object[kPAFrameSlot].parent.object != nullptr;
	}

	// Iron-sight detection. F4 sometimes stays in kFirstPerson during ADS so
	// we also check gunState - kSighted (6) and kFireSighted (8) are the
	// authoritative ADS states (verified across vanilla + custom anim graphs).
	inline bool IsPlayerInIronSights()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) return false;

		// gunState is a 4-bit bitfield on ActorState. Values 6 (kSighted)
		// and 8 (kFireSighted) are the verified ADS states across vanilla
		// and custom animation graphs.
		const auto gs = static_cast<std::uint32_t>(player->gunState);
		if (gs == 6 || gs == 8) return true;

		auto* camera = RE::PlayerCamera::GetSingleton();
		if (camera && camera->currentState &&
		    camera->currentState->id.get() == RE::CameraState::kIronSights) {
			return true;
		}
		return false;
	}

	// True while the VATS camera state owns the view (engine-driven FOV).
	inline bool IsPlayerCameraVATS()
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		return camera && camera->currentState &&
		       camera->currentState->id.get() == RE::CameraState::kVATS;
	}

	// Resolve the player's currently-occupied furniture (the chair/terminal
	// they're sitting at). Returns nullptr if not interacting with any.
	inline RE::TESObjectREFR* GetPlayerCurrentFurniture()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->currentProcess || !player->currentProcess->middleHigh) {
			return nullptr;
		}
		auto refPtr = player->currentProcess->middleHigh->currentFurniture.get();
		return refPtr.get();
	}
}
