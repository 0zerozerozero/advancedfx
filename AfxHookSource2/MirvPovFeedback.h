#pragma once

#include <Windows.h>
#include <stdint.h>

namespace SOURCESDK { namespace CS2 { class IGameEvent; } }

void MirvPovFeedback_Initialize(HMODULE clientDll);
void MirvPovFeedback_Reset();
void MirvPovFeedback_Update(int demoTick);
void MirvPovFeedback_HandleGameEvent(SOURCESDK::CS2::IGameEvent * event);

// The event path replays the local Ring after applying the native SoundSystem
// deafen control, so the native Ring is not emitted twice.
bool MirvPovFeedback_ShouldSuppressNativeFlashRing(const char * soundName);
bool MirvPovFeedback_HandleNativeDeafen(uint32_t controlHash);
