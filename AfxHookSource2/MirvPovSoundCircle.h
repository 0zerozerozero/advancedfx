#pragma once

#include <Windows.h>
#include <stdint.h>

void MirvPovSoundCircle_Initialize(HMODULE clientDll);
void MirvPovSoundCircle_SetSoundEventInterfaceSlot(void ** slot);
const char * MirvPovSoundCircle_GetNativeSoundEventName(uint32_t eventHash);
void MirvPovSoundCircle_ProcessNativeSoundEvent(void * netMessage);
