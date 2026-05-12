#pragma once

#include <cstdint>

#include <Windows.h>
#include "../deps/release/prop/cs2/sdk_src/public/entityhandle.h"

bool Hook_ClientEntitySystem( void* pEntityList, void * pFnGetHighestEntityIterator, void * pFnGetEntityFromIndex );

bool Hook_ClientEntitySystem2();

void Hook_ClientEntitySystem3(HMODULE clientDll);

bool Hook_GetSplitScreenPlayer( void* pAddr);

class CEntityInstance * GetEntityFromIndex(int index);

class CEntityInstance * GetRealSplitScreenPlayer(int slot);

class CEntityInstance * GetFakePovRadarController();

class CEntityInstance * GetEffectiveSplitScreenPlayer(int slot);

void SetFakePovRadarControllerIndex(int index);

int GetFakePovRadarControllerIndex();

void SetFakePovRadarAutoSync(bool enabled);

bool GetFakePovRadarAutoSync();

bool IsFakePovRadarEnabled();

enum FakePovRadarExperimentFlags : unsigned int {
    kFakePovRadarExp_None = 0,
    kFakePovRadarExp_LocalPointer = 1 << 0,
    kFakePovRadarExp_ForceSpotted = 1 << 1,
    kFakePovRadarExp_ControllerFlags = 1 << 2,
    kFakePovRadarExp_ObserverMode = 1 << 3,
};

void SetFakePovRadarExperimentFlags(unsigned int flags);
unsigned int GetFakePovRadarExperimentFlags();

bool FakePovRadar_InitLocalPlayerControllerPointer(HMODULE clientDll);
bool FakePovRadar_HasLocalPlayerControllerPointer();
void * FakePovRadar_GetLocalPlayerControllerPointerAddress();

bool Hook_HudRadarUpdate(HMODULE clientDll, int vtableSlot);
void * GetHudRadarVtable();
int GetHudRadarHookSlot();
long GetHudRadarHookCallCount();

int CEntityInstance_GetCompTeammateColor(class CEntityInstance * controller);

bool IsFakePovRadarFrameContextActive();

bool ConsumeFakePovRadarFrameContextWasActive();

void FakePovRadar_BeginClientFrameContext();

void FakePovRadar_EndClientFrameContext();

void FakePovRadar_RestoreSpottedState();
void FakePovRadar_ReWriteSpotted();

class CAfxEntityInstanceRef;

class CEntityInstance {
public:
    const char * GetName();

    const char * GetDebugName();

    const char * GetClassName();

    const char * GetClientClassName();

    bool IsPlayerPawn();

    SOURCESDK::CS2::CBaseHandle GetPlayerPawnHandle();

    bool IsPlayerController();

    SOURCESDK::CS2::CBaseHandle GetPlayerControllerHandle();

    unsigned int GetHealth();

    int GetTeam();
	
    /**
     * @remarks FLOAT_MAX if invalid
     */
    void GetOrigin(float & x, float & y, float & z);

    void GetRenderEyeOrigin(float outOrigin[3]);

    void GetRenderEyeAngles(float outAngles[3]);

    SOURCESDK::CS2::CBaseHandle GetViewEntityHandle();

    SOURCESDK::CS2::CBaseHandle GetActiveWeaponHandle();

    const char * GetPlayerName();

    uint64_t GetSteamId();

    const char * GetSanitizedPlayerName();

    uint8_t GetObserverMode();
    SOURCESDK::CS2::CBaseHandle GetObserverTarget();
    bool GetSpottedState(bool & spotted, uint32_t & mask0, uint32_t & mask1);

    SOURCESDK::CS2::CBaseHandle GetHandle();

    uint8_t LookupAttachment(const char* attachmentName);
	bool GetAttachment(uint8_t idx, SOURCESDK::Vector &origin, SOURCESDK::Quaternion &angles);
};

typedef int (__fastcall * GetHighestEntityIndex_t)(void * pEntityList, bool bUnknown);
typedef void * (__fastcall * GetEntityFromIndex_t)(void * pEntityList, int index);

extern GetHighestEntityIndex_t  g_GetHighestEntityIndex;
extern GetEntityFromIndex_t g_GetEntityFromIndex;

extern void ** g_pEntityList;

int GetHighestEntityIndex();
