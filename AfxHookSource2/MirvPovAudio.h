#pragma once

#include <Windows.h>
#include <stdint.h>

void MirvPovAudio_Initialize(HMODULE clientDll);
bool MirvPovAudio_ApplyDeafenControl(uint32_t controlHash);
bool MirvPovAudio_PlayLocalFlashbangRing(const char * soundName);
