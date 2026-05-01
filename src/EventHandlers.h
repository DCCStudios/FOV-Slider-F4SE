#pragma once

#include "RE/Bethesda/Events.h"
#include "RE/Bethesda/UI.h"

namespace RE
{
	// CommonLibF4 only forward-declares this struct; we re-declare it
	// here with the verified engine layout (refr, animEvent, argument)
	// so we can RegisterSink against `BSTEventSource<BSAnimationGraphEvent>`
	// returned by `BGSAnimationSystemUtils::GetEventSourcePointersFromGraph`.
	// Same approach FPInertia uses.
	struct BSAnimationGraphEvent
	{
		TESObjectREFR* refr;
		BSFixedString  animEvent;
		BSFixedString  argument;
	};

	namespace BGSAnimationSystemUtils
	{
		inline bool GetEventSourcePointersFromGraph(
			const TESObjectREFR* a_refr,
			BSScrapArray<BSTEventSource<BSAnimationGraphEvent>*>& a_sourcesOut)
		{
			using func_t = decltype(&GetEventSourcePointersFromGraph);
			REL::Relocation<func_t> func{ REL::ID(897074) };
			return func(a_refr, a_sourcesOut);
		}
	}
}

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

	// Sink for BSAnimationGraphEvent (CameraOverrideStart, sightedState*).
	class AnimSink :
		public RE::BSTEventSink<RE::BSAnimationGraphEvent>
	{
	public:
		static AnimSink* GetSingleton()
		{
			static AnimSink s;
			return &s;
		}

		// Re-registers on every game load (the player's animation graph
		// is rebuilt). Returns true if registration succeeded - false if
		// the player isn't ready yet (callers should retry).
		bool Register();
		void Unregister();

		bool IsRegistered() const { return registered.load(); }

		RE::BSEventNotifyControl ProcessEvent(
			const RE::BSAnimationGraphEvent&                a_event,
			RE::BSTEventSource<RE::BSAnimationGraphEvent>*  a_source) override;

	private:
		std::atomic<bool> registered{ false };

		// Track previous "on terminal" state so CameraOverrideEnd can
		// reliably trigger the exit transition.
		std::atomic<bool> wasOnTerminal{ false };
	};

	// One-time registration of all event sinks (called on kGameDataReady).
	// Animation sink registration is retried until the player graph is
	// ready, since on cold-boot the graph isn't populated immediately.
	void RegisterEventSinks();

	// Re-register sinks after a save load (animation graph is rebuilt).
	void OnGameLoaded();
}
