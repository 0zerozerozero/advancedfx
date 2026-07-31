#pragma once

#include <Windows.h>

class CEntityInstance;

bool MirvPov_IsEnabled();
void MirvPov_Enable(HMODULE clientDll);
void MirvPov_Disable();

CEntityInstance * GetCurrentPovPlayerController();
CEntityInstance * GetCurrentPovPlayerPawn();
CEntityInstance * GetEffectiveSplitScreenPlayer(int slot);

CEntityInstance * GetFakePovRadarController();
int GetFakePovRadarControllerIndex();
void SetFakePovRadarControllerIndex(int index);
void SetFakePovRadarAutoSync(bool enabled);
bool GetFakePovRadarAutoSync();

void * MirvPov_PushHookReturnAddress(void * returnAddress);
void * MirvPov_GetHookReturnAddress();
void MirvPov_PopHookReturnAddress(void * previous);

void MirvPov_UpdateSeekDetection();
