#pragma once

#include "RE/Fallout.h"

namespace FOVSlider
{
	// Sink for MenuOpenCloseEvent (PipBoy / Terminal).
	class MenuSink :
		public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static MenuSink* GetSingleton()
		{
			static MenuSink s;
			return &s;
		}

		// Hook UI event source. Safe to call multiple times - re-registers
		// the sink (UI clears registrations on game load).
		void Register();

		RE::BSEventNotifyControl ProcessEvent(
			const RE::MenuOpenCloseEvent&                a_event,
			RE::BSTEventSource<RE::MenuOpenCloseEvent>*  a_source) override;
	};

	// Handles player animation graph events received by the portable
	// PlayerCharacter receiver hook.
	class AnimSink
	{
	public:
		static AnimSink* GetSingleton()
		{
			static AnimSink s;
			return &s;
		}

		void ProcessEvent(const RE::BSAnimationGraphEvent& a_event);

	private:
		// Track previous "on terminal" state so CameraOverrideEnd can
		// reliably trigger the exit transition.
		std::atomic<bool> wasOnTerminal{ false };
	};

	// One-time installation of all event feeds (called on kGameDataReady).
	void RegisterEventSinks();

	// Refresh event sources that can be cleared across a save load.
	void OnGameLoaded();
}
