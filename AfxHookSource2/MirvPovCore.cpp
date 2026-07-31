#include "stdafx.h"

#include "MirvPovCore.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "MirvPovFeedback.h"
#include "MirvPovHud.h"
#include "MirvPovRadar.h"
#include "MirvPovScoreboard.h"
#include "MirvPovSoundCircle.h"
#include "MirvPovTeamHealth.h"
#include "MirvPovTeamID.h"
#include "MirvPovVoice.h"
#include "SchemaSystem.h"

#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

extern SOURCESDK::CS2::ISource2EngineToClient * g_pEngineToClient;

namespace {

int g_FakePovRadarControllerIndex = 0;
bool g_MirvPovAutoSync = false;
bool g_MirvPovEnabled = false;
thread_local void * g_MirvPovHookReturnAddress = nullptr;

CEntityInstance * GetPawnFromController(CEntityInstance * controller)
{
    if(nullptr == controller || !controller->IsPlayerController()) return nullptr;
    auto pawnHandle = controller->GetPlayerPawnHandle();
    if(!pawnHandle.IsValid()) return nullptr;
    CEntityInstance * pawn = GetEntityFromIndex(pawnHandle.GetEntryIndex());
    return nullptr != pawn && pawn->IsPlayerPawn() ? pawn : nullptr;
}

CEntityInstance * ResolveConfiguredPovPlayerController()
{
    if(g_MirvPovAutoSync) return GetObservedPlayerController();
    if(g_FakePovRadarControllerIndex <= 0) return nullptr;

    CEntityInstance * controller = GetEntityFromIndex(g_FakePovRadarControllerIndex);
    return nullptr != controller && controller->IsPlayerController() ? controller : nullptr;
}

} // namespace

bool MirvPov_IsEnabled()
{
    return g_MirvPovEnabled;
}

CEntityInstance * GetCurrentPovPlayerController()
{
    return MirvPov_IsEnabled() ? ResolveConfiguredPovPlayerController() : nullptr;
}

CEntityInstance * GetCurrentPovPlayerPawn()
{
    if(!MirvPov_IsEnabled()) return nullptr;
    if(g_MirvPovAutoSync) return GetObservedPlayerPawn();
    return GetPawnFromController(ResolveConfiguredPovPlayerController());
}

CEntityInstance * GetFakePovRadarController()
{
    return ResolveConfiguredPovPlayerController();
}

CEntityInstance * GetEffectiveSplitScreenPlayer(int slot)
{
    if(0 == slot) {
        if(CEntityInstance * povController = GetCurrentPovPlayerController()) return povController;
    }
    return GetRealSplitScreenPlayer(slot);
}

void SetFakePovRadarControllerIndex(int index)
{
    g_FakePovRadarControllerIndex = 0 < index ? index : 0;
    g_MirvPovAutoSync = false;
}

void SetFakePovRadarAutoSync(bool enabled)
{
    g_MirvPovAutoSync = enabled;
    if(enabled) g_FakePovRadarControllerIndex = -1;
}

bool GetFakePovRadarAutoSync()
{
    return g_MirvPovAutoSync;
}

int GetFakePovRadarControllerIndex()
{
    return g_FakePovRadarControllerIndex;
}

void * MirvPov_PushHookReturnAddress(void * returnAddress)
{
    void * previous = g_MirvPovHookReturnAddress;
    if(nullptr == previous) g_MirvPovHookReturnAddress = returnAddress;
    return previous;
}

void * MirvPov_GetHookReturnAddress()
{
    return g_MirvPovHookReturnAddress;
}

void MirvPov_PopHookReturnAddress(void * previous)
{
    g_MirvPovHookReturnAddress = previous;
}

void MirvPov_UpdateSeekDetection()
{
    if(!MirvPov_IsEnabled() || !g_pEngineToClient) return;
    SOURCESDK::CS2::IDemoFile * demoFile = g_pEngineToClient->GetDemoFile();
    if(!demoFile) return;
    MirvPovHud_UpdateSeekDetection(demoFile->GetDemoTick());
}

void MirvPov_Enable(HMODULE clientDll)
{
    if(g_MirvPovEnabled) return;

    g_MirvPovAutoSync = true;
    MirvPovScoreboard_Reset();
    MirvPovSoundCircle_Initialize(clientDll);
    MirvPovHud_ApplyPatches(clientDll);
    MirvPovTeamHealth_Initialize(clientDll);
    MirvPov_ApplyRadarPatches(clientDll);
    MirvPov_HookVoiceHud(clientDll);
    MirvPovScoreboard_Initialize(clientDll);
    MirvPov_ResetVoiceHud();

    g_MirvPovEnabled = true;
    MirvPovFeedback_Initialize(clientDll);
    MirvPov_UpdateVoiceTeam();
}

void MirvPov_Disable()
{
    if(!g_MirvPovEnabled) return;

    MirvPovScoreboard_Reset();
    g_MirvPovAutoSync = false;
    MirvPovHud_RemovePatches();
    MirvPov_RemoveRadarPatches();
    MirvPovTeamID_RemovePatches();
    MirvPov_ResetVoiceHud();
    g_MirvPovEnabled = false;
}
