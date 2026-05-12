#include "stdafx.h"

#include "ClientEntitySystem.h"
#include "DeathMsg.h"
#include "WrpConsole.h"
#include "Globals.h"

#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
extern SOURCESDK::CS2::ISource2EngineToClient * g_pEngineToClient;

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"
#include "../shared/AfxDetours.h"
#include "../shared/FFITools.h"
#include "../shared/StringTools.h"

#include "AfxHookSource2Rs.h"
#include "SchemaSystem.h"

#define WIN32_LEAN_AND_MEAN
#include "../deps/release/Detours/src/detours.h"

#include <map>
#include <algorithm>

void ** g_pEntityList = nullptr;
GetHighestEntityIndex_t  g_GetHighestEntityIndex = nullptr;
GetEntityFromIndex_t g_GetEntityFromIndex = nullptr;

typedef CEntityInstance * (__fastcall * ClientDll_GetSplitScreenPlayer_t)(int slot);
static ClientDll_GetSplitScreenPlayer_t g_ClientDll_GetSplitScreenPlayer = nullptr;
extern ClientDll_GetSplitScreenPlayer_t g_Org_ClientDll_GetSplitScreenPlayer;

static int g_FakePovRadarControllerIndex = 0;
static bool g_FakePovRadarAutoSync = false;
static unsigned int g_FakePovRadarExperimentFlags = 0;

static CEntityInstance ** g_pLocalPlayerControllerPointer = nullptr;
static bool g_bLocalPlayerControllerPointerResolved = false;

struct FakePovRadarFrameContextState {
    bool active = false;
    bool patchedControllerTeam = false;
    bool patchedControllerPawn = false;
    bool patchedControllerPlayerPawn = false;
    bool patchedControllerObserverPawn = false;
    bool patchedPawnState = false;
    bool patchedLocalPointer = false;
    bool patchedControllerFlags = false;
    bool patchedObserverMode = false;
    CEntityInstance * realController = nullptr;
    CEntityInstance * realPawn = nullptr;
    uint8_t originalControllerTeam = 0;
    unsigned int originalControllerPawnHandle = 0;
    unsigned int originalControllerPlayerPawnHandle = 0;
    unsigned int originalControllerObserverPawnHandle = 0;
    uint8_t originalPawnTeam = 0;
    uint8_t originalObserverMode = 0;
    unsigned int originalObserverTarget = 0;
    unsigned int originalViewEntity = 0;
    CEntityInstance * originalLocalPointerValue = nullptr;
    int enemySpottedPatchCount = 0;
    uint8_t originalRealIsLocalPlayerController = 0;
    uint8_t originalFakeIsLocalPlayerController = 0;
    uint8_t originalRealIsHLTV = 0;
};

struct SpottedRestoreEntry {
    int pawnEntryIndex;
    uint8_t originalSpotted;
    uint32_t originalMask[2];
};

static constexpr int kMaxSpottedRestoreEntries = 64;

static FakePovRadarFrameContextState g_FakePovRadarFrameContextState;
static SpottedRestoreEntry g_SpottedRestoreEntries[kMaxSpottedRestoreEntries];
static int g_SpottedRestoreCount = 0;
static int g_FakePovRadarLastDemoTick = 0;
static bool g_FakePovRadarIsBackwardJump = false;
static std::string g_FakePovRadarLastFrameContextDiag;
static bool g_FakePovRadarLastFrameContextAvailable = false;
static bool g_FakePovRadarFrameContextWasActive = false;

/*
cl_track_render_eye_angles 1
cl_ent_absbox 192
cl_ent_viewoffset 192
*/

// CEntityInstance: Root class for all entities
// Retrieved from script function.
const char * CEntityInstance::GetName() {
    /*
        undefined8 * FUN_1814beac0(void) {
            puVar6[2] = "CEntityInstance: Root class for all entities";
            ...
            puVar4[2] = "Get the entity name";
            ...
            *puVar4 = "GetName";
            ...
            puVar4[8] = FUN_18094f290; // <-  VSCRIPT entity.GetName function.
            ...            
        }        
    */
	const char * pszName = (const char*)*(unsigned char**)(*(unsigned char**)((unsigned char*)this + 0x10) + 0x18);
	if(pszName) return pszName;
	return "";
}

// Retrieved from script function.
// can return nullptr!
const char * CEntityInstance::GetDebugName() {
    /*
        undefined8 * FUN_1814beac0(void) {
            puVar6[2] = "CEntityInstance: Root class for all entities";
            ...
            puVar4[2] = "Get the entity name w/help if not defined (i.e. classname/etc)";
            ...
            *puVar4 = "GetDebugName";
            ...
           puVar4[8] = &LAB_1814c1b90; // <-  VSCRIPT entity.GetDebugName function.
            ...            
        }        
    */    
	const char * pszName = (const char*)*(unsigned char**)(*(unsigned char**)((unsigned char*)this + 0x10) + 0x18);
	if(pszName) return pszName;
	return **(const char***)(*(unsigned char**)(*(unsigned char**)((unsigned char*)this + 0x10) + 0x8)+0x50);
}

// Retrieved from script function.
const char * CEntityInstance::GetClassName() {
    /*
        undefined8 * FUN_1814beac0(void) {
            puVar6[2] = "CEntityInstance: Root class for all entities";
            ...
            *puVar4 = "GetClassname";
            ...
            puVar4[8] = &LAB_1814c1b60; // <-  VSCRIPT entity.GetClassName function.
            ...            
        }        
    */     
	const char * pszName = (const char*)*(unsigned char**)(*(unsigned char**)((unsigned char*)this + 0x10) + 0x20);
	if(pszName) return pszName;
	return "";
}

extern HMODULE g_H_ClientDll;

// Retrieved from script function.
const char * CEntityInstance::GetClientClassName() {
    // GetClientClass function.
    // find it by searching for 4th full-ptr ref to "C_PlantedC4" subtract sizeof(void*) (0x8) and search function that references this struct.
    // you need to search for raw bytes, GiHidra doesn't seem to find the reference.
    void * pClientClass = ((void * (__fastcall *)(void *)) (*(void***)this)[43]) (this);

    if(pClientClass) {
        return *(const char**)((unsigned char*)pClientClass + 0x10);
    }
    return nullptr;
}

// Retrieved from script function.
// GetEntityHandle ...

bool CEntityInstance::IsPlayerPawn() {
	// See cl_ent_text drawing function.
	return ((bool (__fastcall *)(void *)) (*(void***)this)[153]) (this);
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetPlayerPawnHandle() {
	// See cl_ent_text drawing function.
	if(!IsPlayerController())  return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int *)((unsigned char *)this + g_clientDllOffsets.CBasePlayerController.m_hPawn));
}

bool CEntityInstance::IsPlayerController() {
	// See cl_ent_text drawing function. Near "Pawn: (%d) Name: %s".
	return ((bool (__fastcall *)(void *)) (*(void***)this)[154]) (this);    
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetPlayerControllerHandle() {
	// See cl_ent_text drawing function.
	if(!IsPlayerPawn())  return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int *)((unsigned char *)this + g_clientDllOffsets.C_BasePlayerPawn.m_hController));
}

unsigned int CEntityInstance::GetHealth() {
	// See cl_ent_text drawing function. Near "Health: %d\n".
	return *(unsigned int *)((unsigned char *)this + g_clientDllOffsets.C_BaseEntity.m_iHealth);
}

int CEntityInstance::GetTeam() {
    return *(int*)((u_char*)(this) + g_clientDllOffsets.C_BaseEntity.m_iTeamNum);
}


/**
 * @remarks FLOAT_MAX if invalid
 */
void CEntityInstance::GetOrigin(float & x, float & y, float & z) {
    auto ptr = *(u_char**)((u_char*)this + g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode);
	// See cl_ent_text drawing function. Near "Position: %0.3f, %0.3f, %0.3f\n" or cl_ent_viewoffset related function.
	auto vector = (float*)(ptr + g_clientDllOffsets.CGameSceneNode.m_vecAbsOrigin);
	x =  vector[0];
	y =  vector[1];
	z =  vector[2];
}

void CEntityInstance::GetRenderEyeOrigin(float outOrigin[3]) {
	// GetRenderEyeAngles vtable offset minus 1
	((void (__fastcall *)(void *,float outOrigin[3])) (*(void***)this)[168]) (this,outOrigin);
}

void CEntityInstance::GetRenderEyeAngles(float outAngles[3]) {
	// See cl_track_render_eye_angles. Near "Render eye angles: %.7f, %.7f, %.7f\n".
	((void (__fastcall *)(void *,float outAngles[3])) (*(void***)this)[169]) (this,outAngles);
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetViewEntityHandle() {
	if (!IsPlayerPawn())  return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
    void * pCameraServices = *(void**)((unsigned char*)this + g_clientDllOffsets.C_BasePlayerPawn.m_pCameraServices);
    if(nullptr == pCameraServices) return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int*)((unsigned char*)pCameraServices + g_clientDllOffsets.CPlayer_CameraServices.m_hViewEntity));
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetActiveWeaponHandle() {
	if (!IsPlayerPawn())  return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
    void * pWeaponServices = *(void**)((unsigned char*)this + g_clientDllOffsets.C_BasePlayerPawn.m_pWeaponServices);
    if(nullptr == pWeaponServices) return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int*)((unsigned char*)pWeaponServices + g_clientDllOffsets.CPlayer_WeaponServices.m_hActiveWeapon));
}

const char * CEntityInstance::GetPlayerName(){
    if (!IsPlayerController()) return nullptr;
    return *(const char **)((u_char*)(this) + g_clientDllOffsets.CBasePlayerController.m_iszPlayerName);
}

uint64_t CEntityInstance::GetSteamId(){
    if (!IsPlayerController())  return 0;
    return *(uint64_t*)((u_char*)(this) + g_clientDllOffsets.CBasePlayerController.m_steamID);
}

const char * CEntityInstance::GetSanitizedPlayerName() {
   if (!IsPlayerController()) return nullptr;
    return *(const char **)((u_char*)(this) + g_clientDllOffsets.CCSPlayerController.m_sSanitizedPlayerName);

}

uint8_t CEntityInstance::GetObserverMode() {
	if (!IsPlayerPawn()) return 0;
    void * pObserverServices = *(void**)((unsigned char*)this + g_clientDllOffsets.C_BasePlayerPawn.m_pObserverServices);
    if(nullptr == pObserverServices) return 0;
	return *(uint8_t*)((unsigned char*)pObserverServices + g_clientDllOffsets.CPlayer_ObserverServices.m_iObserverMode);    
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetObserverTarget() {
	if (!IsPlayerPawn())  return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
    void * pObserverServices = *(void**)((unsigned char*)this + g_clientDllOffsets.C_BasePlayerPawn.m_pObserverServices);
    if(nullptr == pObserverServices) return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
	return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(unsigned int*)((unsigned char*)pObserverServices + g_clientDllOffsets.CPlayer_ObserverServices.m_hObserverTarget));    
}

bool CEntityInstance::GetSpottedState(bool & spotted, uint32_t & mask0, uint32_t & mask1) {
	spotted = false;
	mask0 = 0;
	mask1 = 0;

	if (!IsPlayerPawn()) return false;
	if (0 == g_clientDllOffsets.C_CSPlayerPawnBase.m_entitySpottedState) return false;

	auto spottedState = (unsigned char*)this + g_clientDllOffsets.C_CSPlayerPawnBase.m_entitySpottedState;
	spotted = 0 != *(uint8_t*)(spottedState + g_clientDllOffsets.EntitySpottedState_t.m_bSpotted);
	auto maskPtr = (uint32_t*)(spottedState + g_clientDllOffsets.EntitySpottedState_t.m_bSpottedByMask);
	mask0 = maskPtr[0];
	mask1 = maskPtr[1];
	return true;
}

SOURCESDK::CS2::CBaseHandle CEntityInstance::GetHandle() {
	if (auto pEntityIdentity = *(u_char**)((u_char*)this + g_clientDllOffsets.CEntityInstance.m_pEntity)) {
		return SOURCESDK::CS2::CEntityHandle::CEntityHandle(*(uint32_t*)(pEntityIdentity + 0x10));
	}

	return SOURCESDK::CS2::CEntityHandle::CEntityHandle();
}

typedef	void (__fastcall * org_LookupAttachment_t)(void* This, uint8_t& outIdx, const char* attachmentName);
org_LookupAttachment_t org_LookupAttachment = nullptr;

typedef	bool (__fastcall * org_GetAttachment_t)(void* This, uint8_t idx, void* out);
org_GetAttachment_t org_GetAttachment = nullptr;

uint8_t CEntityInstance::LookupAttachment(const char* attachmentName) {
	uint8_t idx = 0;
	org_LookupAttachment(this, idx, attachmentName);
	return idx;
}

bool CEntityInstance::GetAttachment(uint8_t idx, SOURCESDK::Vector &origin, SOURCESDK::Quaternion &angles) {
	alignas(16) float resData[8] = {0};

	if(org_GetAttachment(this, idx, resData)) {
		origin.x = resData[0];
		origin.y = resData[1];
		origin.z = resData[2];

		angles.x = resData[4];
		angles.y = resData[5];
		angles.z = resData[6];
		angles.w = resData[7];

		return true;
	}

	return false;
}

class CAfxEntityInstanceRef {
public:
    static CAfxEntityInstanceRef * Aquire(CEntityInstance * pInstance) {
        CAfxEntityInstanceRef * pRef;
        auto it = m_Map.find(pInstance);
        if(it != m_Map.end()) {    
            pRef = it->second;
        } else {
            pRef = new CAfxEntityInstanceRef(pInstance);
            m_Map[pInstance] = pRef;
        }
        pRef->AddRef();
        return pRef;
    }

    static void Invalidate(CEntityInstance * pInstance) {
        if(m_Map.empty()) return;
        auto it = m_Map.find(pInstance);
        if(it != m_Map.end()) {
            auto & pInstance = it->second;
            pInstance->m_pInstance = nullptr;
            m_Map.erase(it);
        }        
    }

    CEntityInstance * GetInstance() {
        return m_pInstance;
    }

    bool IsValid() {
        return nullptr != m_pInstance;
    }

    void AddRef() {
        m_RefCount++;
    }

    void Release() {
        m_RefCount--;
        if(0 == m_RefCount) {
            delete this;
        }
    }

protected:
    CAfxEntityInstanceRef(class CEntityInstance * pInstance)
    : m_pInstance(pInstance)
    {
    }

    ~CAfxEntityInstanceRef() {
        m_Map.erase(m_pInstance);
    }

private:
    int m_RefCount = 0;
    class CEntityInstance * m_pInstance;
    static std::map<CEntityInstance *,CAfxEntityInstanceRef *> m_Map;
};

std::map<CEntityInstance *,CAfxEntityInstanceRef *> CAfxEntityInstanceRef::m_Map;


typedef void* (__fastcall * OnAddEntity_t)(void* This, CEntityInstance* pInstance, SOURCESDK::uint32 handle);
OnAddEntity_t g_Org_OnAddEntity = nullptr;


void* __fastcall New_OnAddEntity(void* This, CEntityInstance* pInstance, SOURCESDK::uint32 handle) {

    void * result =  g_Org_OnAddEntity(This,pInstance,handle);

    if(g_b_on_add_entity && pInstance) {
        auto pRef = CAfxEntityInstanceRef::Aquire(pInstance);
        AfxHookSource2Rs_Engine_OnAddEntity(pRef,handle);
        pRef->Release();
    }

    return result;
}

typedef void* (__fastcall * OnRemoveEntity_t)(void* This, CEntityInstance* inst, SOURCESDK::uint32 handle);
OnRemoveEntity_t g_Org_OnRemoveEntity = nullptr;

void* __fastcall New_OnRemoveEntity(void* This, CEntityInstance* pInstance, SOURCESDK::uint32 handle) {

    if(g_b_on_remove_entity && pInstance) {
        auto pRef = CAfxEntityInstanceRef::Aquire(pInstance);
        AfxHookSource2Rs_Engine_OnRemoveEntity(pRef,handle);
        pRef->Release();
    }

    CAfxEntityInstanceRef::Invalidate(pInstance);

    void * result =  g_Org_OnRemoveEntity(This,pInstance,handle);
    return result;
}

#define STRINGIZE(x) STRINGIZE2(x)
#define STRINGIZE2(x) #x
#define MkErrStr(file,line) "Problem in " file ":" STRINGIZE(line)
extern void ErrorBox(char const * messageText);

bool Hook_ClientEntitySystem( void* pEntityList, void * pFnGetHighestEntityIterator, void * pFnGetEntityFromIndex ) {
    static bool firstResult = false;
    static bool firstRun = true;

    if(firstRun) {
        firstRun = false;
        g_pEntityList = (void**)pEntityList;
        g_GetHighestEntityIndex = (GetHighestEntityIndex_t)pFnGetHighestEntityIterator;
        g_GetEntityFromIndex = (GetEntityFromIndex_t)pFnGetEntityFromIndex;
        firstResult = true;
    }

    return firstResult;
}

bool Hook_ClientEntitySystem2() {
    static bool firstResult = false;
    static bool firstRun = true;

    if(g_pEntityList && *g_pEntityList) {
        // https://github.com/bruhmoment21/cs2-sdk
        void ** vtable = **(void****)g_pEntityList;
        g_Org_OnAddEntity = (OnAddEntity_t)vtable[15];
        g_Org_OnRemoveEntity = (OnRemoveEntity_t)vtable[16];
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_Org_OnAddEntity, New_OnAddEntity);
        DetourAttach(&(PVOID&)g_Org_OnRemoveEntity, New_OnRemoveEntity);
        firstResult = NO_ERROR == DetourTransactionCommit();
    }

    return firstResult;    
}

void Hook_ClientEntitySystem3(HMODULE clientDll) {
	// these two called one after each other
	// there is only one placed where they are called together
	//
	//                             LAB_1802066cd                                   XREF[1]:     18020660b (j)   
    // 1802066cd 8b  4f  6c       MOV        ECX ,dword ptr [RDI  + 0x6c ]
    // 1802066d0 83  e9  01       SUB        ECX ,0x1
    // 1802066d3 0f  84  3d       JZ         LAB_180206816
    //           01  00  00
    // 1802066d9 83  f9  01       CMP        ECX ,0x1
    // 1802066dc 75  4c           JNZ        LAB_18020672a
    // 1802066de 4c  8b  47  30    MOV        R8,qword ptr [RDI  + 0x30 ]
    // 1802066e2 48  8d  95       LEA        RDX =>Stack [0x28 ],[RBP  + 0xc0 ]
    //           c0  00  00  00
    // 1802066e9 48  8b  ce       MOV        RCX ,RSI
    // 1802066ec e8  9f  bf       CALL       FUN_1808c2690                                    undefined FUN_1808c2690()
    //           6b  00
    // 1802066f1 0f  b6  95       MOVZX      EDX ,byte ptr [RBP  + Stack [0x28 ]]
    //           c0  00  00  00
    // 1802066f8 84  d2           TEST       DL,DL
    // 1802066fa 74  29           JZ         LAB_180206725
    // 1802066fc 4c  8d  45  b0    LEA        R8=>local_e8 ,[RBP  + -0x50 ]
    // 180206700 48  8b  ce       MOV        RCX ,RSI
    // 180206703 e8  78  ca       CALL       FUN_1808b3180                                    undefined FUN_1808b3180()
	//
	// to find this place find function with 5 arguments near "Unable to create non-precached breakable%s\n"
	// then in that function find place like this
	//
	//   else if (((*(int *)((longlong)param_4 + 0x6c) == 2) &&
    //          (FUN_1808c2690(param_3,&param_5,param_4[6]), (char)param_5 != '\0')) &&
    //         (FUN_1808b3180(param_3,(char)param_5,&local_e8), cVar2 != '\0')) {
    //   FUN_1815f22b0(param_2,(char)param_5,local_158,&local_108);
    // }
	//
	// first function can be found near "attachment_point" or called with "muzzle_flash" as last arg
	// second function in some places can be found with offset to m_nAttachmentIndex of CEffectData as 2nd arg

	if (auto startAddr = getAddress(clientDll, "E8 ?? ?? ?? ?? 0F B6 95 ?? ?? ?? ?? 84 D2 74 29 4C 8D 45 B0 48 8B CE E8 ?? ?? ?? ??")) {
		org_LookupAttachment = (org_LookupAttachment_t)(startAddr + 5 + *(int32_t*)(startAddr + 1));
		org_GetAttachment = (org_GetAttachment_t)(startAddr + 23 + 5 + *(int32_t*)(startAddr + 23 + 1));
	} else ErrorBox(MkErrStr(__FILE__, __LINE__));
}

int GetHighestEntityIndex() {
    return 2048; // Hardcoded for now, because the function we have is the count, not the index and we need to change mirv-script API to support that better.
    //return g_pEntityList && g_GetHighestEntityIndex ? g_GetHighestEntityIndex(*g_pEntityList, false) : -1;
}

static uint8_t * GetTeamFieldPtr(CEntityInstance * entity) {
    if(nullptr == entity) return nullptr;
    return (uint8_t *)((unsigned char *)entity + g_clientDllOffsets.C_BaseEntity.m_iTeamNum);
}

static unsigned int * GetControllerPawnHandleFieldPtr(CEntityInstance * controller) {
    if(nullptr == controller || !controller->IsPlayerController()) return nullptr;
    return (unsigned int *)((unsigned char *)controller + g_clientDllOffsets.CBasePlayerController.m_hPawn);
}

static unsigned int * GetControllerPlayerPawnHandleFieldPtr(CEntityInstance * controller) {
    if(nullptr == controller || !controller->IsPlayerController()) return nullptr;
    if(0 == g_clientDllOffsets.CCSPlayerController.m_hPlayerPawn) return nullptr;
    return (unsigned int *)((unsigned char *)controller + g_clientDllOffsets.CCSPlayerController.m_hPlayerPawn);
}

static unsigned int * GetControllerObserverPawnHandleFieldPtr(CEntityInstance * controller) {
    if(nullptr == controller || !controller->IsPlayerController()) return nullptr;
    if(0 == g_clientDllOffsets.CCSPlayerController.m_hObserverPawn) return nullptr;
    return (unsigned int *)((unsigned char *)controller + g_clientDllOffsets.CCSPlayerController.m_hObserverPawn);
}

static void * GetObserverServicesPtr(CEntityInstance * pawn) {
    if(nullptr == pawn || !pawn->IsPlayerPawn()) return nullptr;
    return *(void **)((unsigned char *)pawn + g_clientDllOffsets.C_BasePlayerPawn.m_pObserverServices);
}

static void * GetObserverServicesPtrUnchecked(CEntityInstance * pawn) {
    if(nullptr == pawn || 0 == g_clientDllOffsets.C_BasePlayerPawn.m_pObserverServices) return nullptr;
    return *(void **)((unsigned char *)pawn + g_clientDllOffsets.C_BasePlayerPawn.m_pObserverServices);
}

static void * GetCameraServicesPtr(CEntityInstance * pawn) {
    if(nullptr == pawn || !pawn->IsPlayerPawn()) return nullptr;
    return *(void **)((unsigned char *)pawn + g_clientDllOffsets.C_BasePlayerPawn.m_pCameraServices);
}

static uint8_t * GetObserverModeFieldPtr(CEntityInstance * pawn) {
    if(void * pObserverServices = GetObserverServicesPtr(pawn)) {
        return (uint8_t *)((unsigned char *)pObserverServices + g_clientDllOffsets.CPlayer_ObserverServices.m_iObserverMode);
    }
    return nullptr;
}

static unsigned int * GetObserverTargetFieldPtr(CEntityInstance * pawn) {
    if(void * pObserverServices = GetObserverServicesPtr(pawn)) {
        return (unsigned int *)((unsigned char *)pObserverServices + g_clientDllOffsets.CPlayer_ObserverServices.m_hObserverTarget);
    }
    return nullptr;
}

static uint8_t * GetObserverModeFieldPtrUnchecked(CEntityInstance * pawn) {
    if(void * pObserverServices = GetObserverServicesPtrUnchecked(pawn)) {
        return (uint8_t *)((unsigned char *)pObserverServices + g_clientDllOffsets.CPlayer_ObserverServices.m_iObserverMode);
    }
    return nullptr;
}

static unsigned int * GetObserverTargetFieldPtrUnchecked(CEntityInstance * pawn) {
    if(void * pObserverServices = GetObserverServicesPtrUnchecked(pawn)) {
        return (unsigned int *)((unsigned char *)pObserverServices + g_clientDllOffsets.CPlayer_ObserverServices.m_hObserverTarget);
    }
    return nullptr;
}

static unsigned int * GetViewEntityFieldPtr(CEntityInstance * pawn) {
    if(void * pCameraServices = GetCameraServicesPtr(pawn)) {
        return (unsigned int *)((unsigned char *)pCameraServices + g_clientDllOffsets.CPlayer_CameraServices.m_hViewEntity);
    }
    return nullptr;
}

CEntityInstance * GetEntityFromIndex(int index) {
    if(index < 0 || nullptr == g_pEntityList || nullptr == *g_pEntityList || nullptr == g_GetEntityFromIndex) return nullptr;
    return (CEntityInstance *)g_GetEntityFromIndex(*g_pEntityList, index);
}

CEntityInstance * GetRealSplitScreenPlayer(int slot) {
    if(nullptr != g_Org_ClientDll_GetSplitScreenPlayer) {
        return g_Org_ClientDll_GetSplitScreenPlayer(slot);
    }

    if(nullptr == g_ClientDll_GetSplitScreenPlayer) return nullptr;
    return g_ClientDll_GetSplitScreenPlayer(slot);
}

CEntityInstance * GetFakePovRadarController() {
    if(g_FakePovRadarAutoSync) {
        CEntityInstance * realController = GetRealSplitScreenPlayer(0);
        if(nullptr == realController) return nullptr;
        auto pawnHandle = realController->GetPlayerPawnHandle();
        if(!pawnHandle.IsValid()) return nullptr;
        CEntityInstance * realPawn = GetEntityFromIndex(pawnHandle.GetEntryIndex());
        if(nullptr == realPawn) return nullptr;

        uint8_t * pObsMode = GetObserverModeFieldPtrUnchecked(realPawn);
        if(nullptr == pObsMode || 0 == *pObsMode) return nullptr;

        unsigned int * pObsTarget = GetObserverTargetFieldPtrUnchecked(realPawn);
        if(nullptr == pObsTarget) return nullptr;
        SOURCESDK::CS2::CBaseHandle targetHandle(*pObsTarget);
        if(!targetHandle.IsValid()) return nullptr;

        CEntityInstance * targetPawn = GetEntityFromIndex(targetHandle.GetEntryIndex());
        if(nullptr == targetPawn) return nullptr;

        auto controllerHandle = targetPawn->GetPlayerControllerHandle();
        if(!controllerHandle.IsValid()) return nullptr;
        CEntityInstance * targetController = GetEntityFromIndex(controllerHandle.GetEntryIndex());
        if(nullptr == targetController || !targetController->IsPlayerController()) return nullptr;
        return targetController;
    }

    if(g_FakePovRadarControllerIndex <= 0) return nullptr;
    return GetEntityFromIndex(g_FakePovRadarControllerIndex);
}

CEntityInstance * GetEffectiveSplitScreenPlayer(int slot) {
    if(0 == slot) {
        if(CEntityInstance * fake = GetFakePovRadarController()) {
            return fake;
        }
    }
    return GetRealSplitScreenPlayer(slot);
}

bool IsFakePovRadarFrameContextActive() {
    return g_FakePovRadarFrameContextState.active;
}

bool ConsumeFakePovRadarFrameContextWasActive() {
    bool result = g_FakePovRadarFrameContextWasActive;
    g_FakePovRadarFrameContextWasActive = false;
    return result;
}

void SetFakePovRadarControllerIndex(int index) {
    g_FakePovRadarControllerIndex = 0 < index ? index : 0;
    g_FakePovRadarAutoSync = false;
}

void SetFakePovRadarAutoSync(bool enabled) {
    g_FakePovRadarAutoSync = enabled;
    if(enabled) g_FakePovRadarControllerIndex = -1;
}

bool GetFakePovRadarAutoSync() {
    return g_FakePovRadarAutoSync;
}

int GetFakePovRadarControllerIndex() {
    return g_FakePovRadarControllerIndex;
}

bool IsFakePovRadarEnabled() {
    return g_FakePovRadarAutoSync || 0 < g_FakePovRadarControllerIndex;
}

void SetFakePovRadarExperimentFlags(unsigned int flags) {
    g_FakePovRadarExperimentFlags = flags;
}

unsigned int GetFakePovRadarExperimentFlags() {
    return g_FakePovRadarExperimentFlags;
}

int CEntityInstance_GetCompTeammateColor(CEntityInstance * controller) {
    if(nullptr == controller || !controller->IsPlayerController()) return -1;
    if(0 == g_clientDllOffsets.CCSPlayerController.m_iCompTeammateColor) return -1;
    return *(int *)((unsigned char *)controller + g_clientDllOffsets.CCSPlayerController.m_iCompTeammateColor);
}

bool FakePovRadar_InitLocalPlayerControllerPointer(HMODULE clientDll) {
    if(g_bLocalPlayerControllerPointerResolved) return g_pLocalPlayerControllerPointer != nullptr;

    g_bLocalPlayerControllerPointerResolved = true;
    g_pLocalPlayerControllerPointer = nullptr;

    size_t matchAddr = getAddress(clientDll, "48 83 3D ?? ?? ?? ?? 00 0F 95");
    if(0 == matchAddr) {
        advancedfx::Message("[mirv_pov_radar] LocalPlayerControllerPointer pattern not found\n");
        return false;
    }

    int32_t ripOffset = *(int32_t *)(matchAddr + 3);
    size_t pointerAddr = matchAddr + 8 + ripOffset;

    CEntityInstance ** candidate = (CEntityInstance **)pointerAddr;

    CEntityInstance * realLocal = GetRealSplitScreenPlayer(0);
    CEntityInstance * pointee = *candidate;

    if(nullptr != realLocal && nullptr != pointee && pointee == realLocal) {
        g_pLocalPlayerControllerPointer = candidate;
        advancedfx::Message("[mirv_pov_radar] LocalPlayerControllerPointer resolved at %p (pointee matches real local)\n", (void*)candidate);
        return true;
    }

    if(nullptr != pointee && pointee != realLocal) {
        advancedfx::Message("[mirv_pov_radar] LocalPlayerControllerPointer candidate at %p pointee mismatch (pointee=%p realLocal=%p), accepting tentatively\n", (void*)candidate, (void*)pointee, (void*)realLocal);
        g_pLocalPlayerControllerPointer = candidate;
        return true;
    }

    advancedfx::Message("[mirv_pov_radar] LocalPlayerControllerPointer candidate at %p has null pointee, deferring\n", (void*)candidate);
    g_pLocalPlayerControllerPointer = candidate;
    return true;
}

bool FakePovRadar_HasLocalPlayerControllerPointer() {
    return g_pLocalPlayerControllerPointer != nullptr;
}

void * FakePovRadar_GetLocalPlayerControllerPointerAddress() {
    return (void *)g_pLocalPlayerControllerPointer;
}

static void FakePovRadar_LogFrameContextDiag(const char * reason, CEntityInstance * realController, CEntityInstance * fakeController, CEntityInstance * realPawn, CEntityInstance * fakePawn) {
    std::string diag;
    diag.append(reason ? reason : "<null>");
    diag.append(" | realController=");
    diag.append(realController ? (realController->GetSanitizedPlayerName() ? realController->GetSanitizedPlayerName() : realController->GetDebugName()) : "<null>");
    diag.append(" fakeController=");
    diag.append(fakeController ? (fakeController->GetSanitizedPlayerName() ? fakeController->GetSanitizedPlayerName() : fakeController->GetDebugName()) : "<null>");
    diag.append(" realPawn=");
    diag.append(realPawn ? "ok" : "null");
    diag.append(" fakePawn=");
    diag.append(fakePawn ? "ok" : "null");

    if(diag == g_FakePovRadarLastFrameContextDiag) return;
    g_FakePovRadarLastFrameContextDiag = diag;

    advancedfx::Message("[mirv_pov_radar_frame] %s\n", diag.c_str());
}

static void FakePovRadar_LogFrameContextAvailability(
    CEntityInstance * realPawn,
    CEntityInstance * fakePawn,
    uint8_t * realPawnTeam,
    uint8_t * fakePawnTeam,
    uint8_t * realObserverMode,
    uint8_t * fakeObserverMode,
    unsigned int * realObserverTarget,
    unsigned int * fakeObserverTarget,
    unsigned int * realViewEntity,
    unsigned int * fakeViewEntity
) {
    std::string diag = "availability:";
    diag.append(" realPawnTeam="); diag.append(realPawnTeam ? "ok" : "null");
    diag.append(" fakePawnTeam="); diag.append(fakePawnTeam ? "ok" : "null");
    diag.append(" realObserverMode="); diag.append(realObserverMode ? "ok" : "null");
    diag.append(" fakeObserverMode="); diag.append(fakeObserverMode ? "ok" : "null");
    diag.append(" realObserverTarget="); diag.append(realObserverTarget ? "ok" : "null");
    diag.append(" fakeObserverTarget="); diag.append(fakeObserverTarget ? "ok" : "null");
    diag.append(" realViewEntity="); diag.append(realViewEntity ? "ok" : "null");
    diag.append(" fakeViewEntity="); diag.append(fakeViewEntity ? "ok" : "null");
    diag.append(" realPawnClass="); diag.append(realPawn ? realPawn->GetClientClassName() ? realPawn->GetClientClassName() : realPawn->GetClassName() : "<null>");
    diag.append(" fakePawnClass="); diag.append(fakePawn ? fakePawn->GetClientClassName() ? fakePawn->GetClientClassName() : fakePawn->GetClassName() : "<null>");

    if(diag == g_FakePovRadarLastFrameContextDiag) return;
    g_FakePovRadarLastFrameContextDiag = diag;

    advancedfx::Message("[mirv_pov_radar_frame] %s\n", diag.c_str());
}

void FakePovRadar_BeginClientFrameContext() {
    if(g_FakePovRadarFrameContextState.active) return;

    CEntityInstance * fakeController = GetFakePovRadarController();
    CEntityInstance * realController = GetRealSplitScreenPlayer(0);
    if(nullptr == fakeController || nullptr == realController || fakeController == realController) {
        FakePovRadar_LogFrameContextDiag("skip: no distinct real/fake controller", realController, fakeController, nullptr, nullptr);
        return;
    }

    CEntityInstance * realPawn = GetEntityFromIndex(realController->GetPlayerPawnHandle().GetEntryIndex());
    CEntityInstance * fakePawn = GetEntityFromIndex(fakeController->GetPlayerPawnHandle().GetEntryIndex());

    uint8_t * realControllerTeam = GetTeamFieldPtr(realController);
    uint8_t * fakeControllerTeam = GetTeamFieldPtr(fakeController);
    unsigned int * realControllerPawnHandle = GetControllerPawnHandleFieldPtr(realController);
    unsigned int * fakeControllerPawnHandle = GetControllerPawnHandleFieldPtr(fakeController);
    unsigned int * realControllerPlayerPawnHandle = GetControllerPlayerPawnHandleFieldPtr(realController);
    unsigned int * fakeControllerPlayerPawnHandle = GetControllerPlayerPawnHandleFieldPtr(fakeController);
    unsigned int * realControllerObserverPawnHandle = GetControllerObserverPawnHandleFieldPtr(realController);
    unsigned int * fakeControllerObserverPawnHandle = GetControllerObserverPawnHandleFieldPtr(fakeController);
    uint8_t * realPawnTeam = realPawn ? GetTeamFieldPtr(realPawn) : nullptr;
    uint8_t * fakePawnTeam = fakePawn ? GetTeamFieldPtr(fakePawn) : nullptr;

    if(nullptr == realControllerTeam || nullptr == fakeControllerTeam) {
        FakePovRadar_LogFrameContextDiag("skip: controller team pointers unavailable", realController, fakeController, realPawn, fakePawn);
        return;
    }

    g_FakePovRadarFrameContextState.active = true;
    g_FakePovRadarFrameContextWasActive = true;
    g_FakePovRadarFrameContextState.realController = realController;
    g_FakePovRadarFrameContextState.realPawn = realPawn;

    const unsigned int identityExperiments = kFakePovRadarExp_LocalPointer | kFakePovRadarExp_ControllerFlags | kFakePovRadarExp_ObserverMode;
    bool needIdentitySwap = (g_FakePovRadarExperimentFlags & identityExperiments) != 0;

    if(needIdentitySwap) {
        g_FakePovRadarFrameContextState.originalControllerTeam = *realControllerTeam;
        g_FakePovRadarFrameContextState.patchedControllerTeam = true;
        *realControllerTeam = *fakeControllerTeam;

        if(nullptr != realControllerPawnHandle && nullptr != fakeControllerPawnHandle) {
            g_FakePovRadarFrameContextState.originalControllerPawnHandle = *realControllerPawnHandle;
            g_FakePovRadarFrameContextState.patchedControllerPawn = true;
            *realControllerPawnHandle = *fakeControllerPawnHandle;
        }

        if(nullptr != realControllerPlayerPawnHandle && nullptr != fakeControllerPlayerPawnHandle) {
            g_FakePovRadarFrameContextState.originalControllerPlayerPawnHandle = *realControllerPlayerPawnHandle;
            g_FakePovRadarFrameContextState.patchedControllerPlayerPawn = true;
            *realControllerPlayerPawnHandle = *fakeControllerPlayerPawnHandle;
        }

        if(nullptr != realControllerObserverPawnHandle && nullptr != fakeControllerObserverPawnHandle) {
            g_FakePovRadarFrameContextState.originalControllerObserverPawnHandle = *realControllerObserverPawnHandle;
            g_FakePovRadarFrameContextState.patchedControllerObserverPawn = true;
            *realControllerObserverPawnHandle = *fakeControllerObserverPawnHandle;
        }

        bool contextAvailable = nullptr != realPawnTeam && nullptr != fakePawnTeam;

        if(contextAvailable) {
            g_FakePovRadarFrameContextState.originalPawnTeam = *realPawnTeam;
            g_FakePovRadarFrameContextState.patchedPawnState = true;
            *realPawnTeam = *fakePawnTeam;
        }

        if(g_FakePovRadarLastFrameContextAvailable != contextAvailable) {
            FakePovRadar_LogFrameContextAvailability(
                realPawn,
                fakePawn,
                realPawnTeam,
                fakePawnTeam,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr
            );

            FakePovRadar_LogFrameContextDiag(
                contextAvailable
                    ? "active: controller/local team patched without camera override"
                    : "partial: controller team patched, local pawn team unavailable",
                realController,
                fakeController,
                realPawn,
                fakePawn
        );

        g_FakePovRadarLastFrameContextAvailable = contextAvailable;
    }
    } // needIdentitySwap

    if(g_FakePovRadarExperimentFlags & kFakePovRadarExp_LocalPointer) {
        if(nullptr != g_pLocalPlayerControllerPointer) {
            g_FakePovRadarFrameContextState.originalLocalPointerValue = *g_pLocalPlayerControllerPointer;
            *g_pLocalPlayerControllerPointer = fakeController;
            g_FakePovRadarFrameContextState.patchedLocalPointer = true;
        }
    }

    if(g_FakePovRadarExperimentFlags & kFakePovRadarExp_ForceSpotted) {
        g_SpottedRestoreCount = 0;
        int fakeTeam = fakeController->GetTeam();
        if(fakeTeam == 2 || fakeTeam == 3) {
            int highestIndex = GetHighestEntityIndex();
            for(int i = 0; i < highestIndex + 1; ++i) {
                CEntityInstance * controller = GetEntityFromIndex(i);
                if(nullptr == controller || !controller->IsPlayerController()) continue;
                int team = controller->GetTeam();
                if(team != fakeTeam) continue;
                if(controller == fakeController) continue;

                auto pawnHandle = controller->GetPlayerPawnHandle();
                if(!pawnHandle.IsValid()) continue;
                CEntityInstance * pawn = GetEntityFromIndex(pawnHandle.GetEntryIndex());
                if(nullptr == pawn || !pawn->IsPlayerPawn()) continue;
                if(0 == g_clientDllOffsets.C_CSPlayerPawnBase.m_entitySpottedState) continue;

                auto spottedState = (unsigned char*)pawn + g_clientDllOffsets.C_CSPlayerPawnBase.m_entitySpottedState;
                uint8_t * pSpotted = (uint8_t*)(spottedState + g_clientDllOffsets.EntitySpottedState_t.m_bSpotted);
                uint32_t * pMask = (uint32_t*)(spottedState + g_clientDllOffsets.EntitySpottedState_t.m_bSpottedByMask);

                if(g_SpottedRestoreCount < kMaxSpottedRestoreEntries) {
                    g_SpottedRestoreEntries[g_SpottedRestoreCount].pawnEntryIndex = pawnHandle.GetEntryIndex();
                    g_SpottedRestoreEntries[g_SpottedRestoreCount].originalSpotted = *pSpotted;
                    g_SpottedRestoreEntries[g_SpottedRestoreCount].originalMask[0] = pMask[0];
                    g_SpottedRestoreEntries[g_SpottedRestoreCount].originalMask[1] = pMask[1];
                    g_SpottedRestoreCount++;
                }

                *pSpotted = 1;
                pMask[0] = 0xFFFFFFFF;
                pMask[1] = 0xFFFFFFFF;
            }
        }
    }

    if(g_FakePovRadarExperimentFlags & kFakePovRadarExp_ControllerFlags) {
        ptrdiff_t offIsLocal = g_clientDllOffsets.CBasePlayerController.m_bIsLocalPlayerController;
        ptrdiff_t offIsHLTV = g_clientDllOffsets.CBasePlayerController.m_bIsHLTV;
        if(offIsLocal != 0 && offIsHLTV != 0) {
            uint8_t * pRealIsLocal = (uint8_t *)((unsigned char *)realController + offIsLocal);
            uint8_t * pFakeIsLocal = (uint8_t *)((unsigned char *)fakeController + offIsLocal);
            uint8_t * pRealIsHLTV = (uint8_t *)((unsigned char *)realController + offIsHLTV);

            g_FakePovRadarFrameContextState.originalRealIsLocalPlayerController = *pRealIsLocal;
            g_FakePovRadarFrameContextState.originalFakeIsLocalPlayerController = *pFakeIsLocal;
            g_FakePovRadarFrameContextState.originalRealIsHLTV = *pRealIsHLTV;

            *pRealIsLocal = 0;
            *pFakeIsLocal = 1;
            *pRealIsHLTV = 0;

            g_FakePovRadarFrameContextState.patchedControllerFlags = true;
        }
    }

    if(g_FakePovRadarExperimentFlags & kFakePovRadarExp_ObserverMode) {
        if(nullptr != realPawn) {
            uint8_t * pRealObserverMode = GetObserverModeFieldPtrUnchecked(realPawn);
            unsigned int * pRealObserverTarget = GetObserverTargetFieldPtrUnchecked(realPawn);
            if(nullptr != pRealObserverMode) {
                g_FakePovRadarFrameContextState.originalObserverMode = *pRealObserverMode;
                *pRealObserverMode = 0; // OBS_MODE_NONE

                if(nullptr != pRealObserverTarget) {
                    g_FakePovRadarFrameContextState.originalObserverTarget = *pRealObserverTarget;
                    if(nullptr != fakePawn) {
                        auto handle = fakePawn->GetHandle();
                        *pRealObserverTarget = handle.IsValid() ? (unsigned int)handle.ToInt() : 0xFFFFFFFF;
                    }
                }

                g_FakePovRadarFrameContextState.patchedObserverMode = true;
            }
        }
    }
}

void FakePovRadar_EndClientFrameContext() {
    if(!g_FakePovRadarFrameContextState.active) return;

    CEntityInstance * realController = g_FakePovRadarFrameContextState.realController;
    CEntityInstance * realPawn = g_FakePovRadarFrameContextState.realPawn;

    if(g_FakePovRadarFrameContextState.patchedObserverMode) {
        if(nullptr != realPawn) {
            uint8_t * pRealObserverMode = GetObserverModeFieldPtrUnchecked(realPawn);
            unsigned int * pRealObserverTarget = GetObserverTargetFieldPtrUnchecked(realPawn);
            if(nullptr != pRealObserverMode) {
                *pRealObserverMode = g_FakePovRadarFrameContextState.originalObserverMode;
            }
            if(nullptr != pRealObserverTarget) {
                *pRealObserverTarget = g_FakePovRadarFrameContextState.originalObserverTarget;
            }
        }
    }

    if(g_FakePovRadarFrameContextState.patchedControllerFlags) {
        CEntityInstance * fakeController = GetFakePovRadarController();
        ptrdiff_t offIsLocal = g_clientDllOffsets.CBasePlayerController.m_bIsLocalPlayerController;
        ptrdiff_t offIsHLTV = g_clientDllOffsets.CBasePlayerController.m_bIsHLTV;
        if(offIsLocal != 0 && offIsHLTV != 0) {
            uint8_t * pRealIsLocal = (uint8_t *)((unsigned char *)realController + offIsLocal);
            uint8_t * pFakeIsLocal = fakeController ? (uint8_t *)((unsigned char *)fakeController + offIsLocal) : nullptr;
            uint8_t * pRealIsHLTV = (uint8_t *)((unsigned char *)realController + offIsHLTV);

            *pRealIsLocal = g_FakePovRadarFrameContextState.originalRealIsLocalPlayerController;
            if(pFakeIsLocal) *pFakeIsLocal = g_FakePovRadarFrameContextState.originalFakeIsLocalPlayerController;
            *pRealIsHLTV = g_FakePovRadarFrameContextState.originalRealIsHLTV;
        }
    }

    if(g_FakePovRadarFrameContextState.patchedLocalPointer) {
        if(nullptr != g_pLocalPlayerControllerPointer) {
            *g_pLocalPlayerControllerPointer = g_FakePovRadarFrameContextState.originalLocalPointerValue;
        }
    }

    if(g_FakePovRadarFrameContextState.patchedControllerTeam) if(uint8_t * realControllerTeam = GetTeamFieldPtr(realController)) {
        *realControllerTeam = g_FakePovRadarFrameContextState.originalControllerTeam;
    }

    if(g_FakePovRadarFrameContextState.patchedControllerPawn) if(unsigned int * realControllerPawnHandle = GetControllerPawnHandleFieldPtr(realController)) {
        *realControllerPawnHandle = g_FakePovRadarFrameContextState.originalControllerPawnHandle;
    }

    if(g_FakePovRadarFrameContextState.patchedControllerPlayerPawn) if(unsigned int * realControllerPlayerPawnHandle = GetControllerPlayerPawnHandleFieldPtr(realController)) {
        *realControllerPlayerPawnHandle = g_FakePovRadarFrameContextState.originalControllerPlayerPawnHandle;
    }

    if(g_FakePovRadarFrameContextState.patchedControllerObserverPawn) if(unsigned int * realControllerObserverPawnHandle = GetControllerObserverPawnHandleFieldPtr(realController)) {
        *realControllerObserverPawnHandle = g_FakePovRadarFrameContextState.originalControllerObserverPawnHandle;
    }

    if(g_FakePovRadarFrameContextState.patchedPawnState) if(uint8_t * realPawnTeam = GetTeamFieldPtr(realPawn)) {
        *realPawnTeam = g_FakePovRadarFrameContextState.originalPawnTeam;
    }

    g_FakePovRadarFrameContextState = FakePovRadarFrameContextState {};
}

void FakePovRadar_RestoreSpottedState() {
    if(g_SpottedRestoreCount == 0) return;
    for(int i = 0; i < g_SpottedRestoreCount; ++i) {
        auto & entry = g_SpottedRestoreEntries[i];
        CEntityInstance * pawn = GetEntityFromIndex(entry.pawnEntryIndex);
        if(nullptr == pawn || !pawn->IsPlayerPawn()) continue;
        if(0 == g_clientDllOffsets.C_CSPlayerPawnBase.m_entitySpottedState) continue;
        auto spottedState = (unsigned char*)pawn + g_clientDllOffsets.C_CSPlayerPawnBase.m_entitySpottedState;
        uint8_t * pSpotted = (uint8_t*)(spottedState + g_clientDllOffsets.EntitySpottedState_t.m_bSpotted);
        uint32_t * pMask = (uint32_t*)(spottedState + g_clientDllOffsets.EntitySpottedState_t.m_bSpottedByMask);
        *pSpotted = entry.originalSpotted;
        pMask[0] = entry.originalMask[0];
        pMask[1] = entry.originalMask[1];
    }
    g_SpottedRestoreCount = 0;
}

void FakePovRadar_ReWriteSpotted() {
    if(g_SpottedRestoreCount == 0) return;
    for(int i = 0; i < g_SpottedRestoreCount; ++i) {
        auto & entry = g_SpottedRestoreEntries[i];
        CEntityInstance * pawn = GetEntityFromIndex(entry.pawnEntryIndex);
        if(nullptr == pawn || !pawn->IsPlayerPawn()) continue;
        if(0 == g_clientDllOffsets.C_CSPlayerPawnBase.m_entitySpottedState) continue;
        auto spottedState = (unsigned char*)pawn + g_clientDllOffsets.C_CSPlayerPawnBase.m_entitySpottedState;
        uint8_t * pSpotted = (uint8_t*)(spottedState + g_clientDllOffsets.EntitySpottedState_t.m_bSpotted);
        uint32_t * pMask = (uint32_t*)(spottedState + g_clientDllOffsets.EntitySpottedState_t.m_bSpottedByMask);
        *pSpotted = 1;
        pMask[0] = 0xFFFFFFFF;
        pMask[1] = 0xFFFFFFFF;
    }
}

struct MirvEntityEntry {
	int entryIndex;
	int handle;
	std::string debugName;
	std::string className;
	std::string clientClassName;
	SOURCESDK::Vector origin;
	SOURCESDK::QAngle angles;
};

CON_COMMAND(mirv_listentities, "List entities.")
{
	auto argC = args->ArgC();
	auto arg0 = args->ArgV(0);

	bool filterPlayers = false;
	bool sortByDistance = false;
	int printCount = -1;

	if (2 <= argC && 0 == _stricmp(args->ArgV(1), "help")) {
		advancedfx::Message(
			"%s help - Print this help.\n"
			"%s <option1> <option2> ... - Customize printed output with options.\n"
			"Where <option> is (you don't have to use all):\n"
			"\t\"isPlayer=1\" - Show only player related entities. Unless you need handles, the \"mirv_deathmsg help players\" might be more useful.\n"
			"\t\"sort=distance\" - Sort entities by distance relative to current position, from closest to most distant.\n"
			"\t\"limit=<i>\" - Limit number of printed entries.\n"
			"Example:\n"
			"%s sort=distance limit=10\n" 
			, arg0, arg0, arg0
		);
		return;
	} else {
		for (int i = 1; i < argC; i++) {
			const char * argI = args->ArgV(i);
			if (StringIBeginsWith(argI, "limit=")) {
				printCount = atoi(argI + strlen("limit="));
			} 
			else if (StringIBeginsWith(argI, "sort=")) {
				if (0 == _stricmp(argI + strlen("sort="), "distance")) sortByDistance = true;
			}
			else if (0 == _stricmp(argI, "isPlayer=1")) {
				filterPlayers = true;
			}
		}
	}

	std::vector<MirvEntityEntry> entries;

    int highestIndex = GetHighestEntityIndex();
    for(int i = 0; i < highestIndex + 1; i++) {
        if(auto ent = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList,i)) {
			if (filterPlayers && !ent->IsPlayerController() && !ent->IsPlayerPawn()) continue;
			
            float render_origin[3];
            float render_angles[3];
            ent->GetRenderEyeOrigin(render_origin);
            ent->GetRenderEyeAngles(render_angles);

			auto debugName = ent->GetDebugName();
			auto className = ent->GetClassName();
			auto clientClassName = ent->GetClientClassName();

			entries.emplace_back(
				MirvEntityEntry {
					i, ent->GetHandle().ToInt(), 
					debugName ? debugName : "", className ? className : "", clientClassName ? clientClassName : "",
					SOURCESDK::Vector {render_origin[0], render_origin[1], render_origin[2]},
					SOURCESDK::QAngle {render_angles[0], render_angles[1], render_angles[2]} 
				}
			);

        }
    }

	if (sortByDistance) {
		SOURCESDK::Vector curPos = {(float)g_CurrentGameCamera.origin[0], (float)g_CurrentGameCamera.origin[1], (float)g_CurrentGameCamera.origin[2]};

		std::sort(entries.begin(), entries.end(), [&](MirvEntityEntry & a, MirvEntityEntry & b) {
			auto distA = (curPos - a.origin).LengthSqr();
			auto distB = (curPos - b.origin).LengthSqr();
			return distA < distB;
		});
	}

	advancedfx::Message("entryIndex / handle / debugName / className / clientClassName / [ x , y , z , rX , rY , rZ ]\n");
	if (printCount == -1) printCount = entries.size();
	for (int i = 0; i < printCount; i++) {
		auto e = entries[i];
		advancedfx::Message("%i / %i / %s / %s / %s / [ %f , %f , %f , %f , %f , %f ]\n"
			, e.entryIndex, e.handle
			, e.debugName.c_str(), e.className.c_str(), e.clientClassName.c_str()
			, e.origin.x, e.origin.y, e.origin.z 
			, e.angles.x, e.angles.y, e.angles.z
		);
	}
}

extern "C" int afx_hook_source2_get_highest_entity_index() {
    int highestIndex = GetHighestEntityIndex();
    return highestIndex;
}

extern "C" void * afx_hook_source2_get_entity_ref_from_index(int index) {
    if(CEntityInstance * result = (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList,index)) {
        return CAfxEntityInstanceRef::Aquire(result);
    }
    return nullptr;
}

extern "C" void afx_hook_source2_add_ref_entity_ref(void * pRef) {
    ((CAfxEntityInstanceRef *)pRef)->AddRef();
}

extern "C" void afx_hook_source2_release_entity_ref(void * pRef) {
    ((CAfxEntityInstanceRef *)pRef)->Release();
}

extern "C" FFIBool afx_hook_source2_get_entity_ref_is_valid(void * pRef) {
    return BOOL_TO_FFIBOOL(((CAfxEntityInstanceRef *)pRef)->IsValid());
}

extern "C" const char * afx_hook_source2_get_entity_ref_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetName();
    }
    return "";
}

extern "C" const char * afx_hook_source2_get_entity_ref_debug_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetDebugName();
    }
    return nullptr;
}

extern "C" const char * afx_hook_source2_get_entity_ref_class_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetClassName();
    }
    return "";
}

extern "C" const char * afx_hook_source2_get_entity_ref_client_class_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetClientClassName();
    }
    return "";
}

extern "C" FFIBool afx_hook_source2_get_entity_ref_is_player_pawn(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return BOOL_TO_FFIBOOL(pInstance->IsPlayerPawn());
    }
    return FFIBOOL_FALSE;
}

extern "C" int afx_hook_source2_get_entity_ref_player_pawn_handle(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetPlayerPawnHandle().ToInt();
    }
    return SOURCESDK_CS2_INVALID_EHANDLE_INDEX;    
}

extern "C" FFIBool afx_hook_source2_get_entity_ref_is_player_controller(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return BOOL_TO_FFIBOOL(pInstance->IsPlayerController());
    }
    return FFIBOOL_FALSE;
}

// ============================================================================
// Approach B: Hook CCSGO_HudRadar update method
// ============================================================================

static void ** g_pCCSGO_HudRadar_vtable = nullptr;
static int g_HudRadarHookSlot = -1;

typedef void (__fastcall * HudRadarUpdate_t)(void * This);
static HudRadarUpdate_t g_Org_HudRadarUpdate = nullptr;

// Spotted patch/restore scoped to radar update
static void RadarUpdate_ForceTeammateSpotted() {
    if(!(g_FakePovRadarExperimentFlags & kFakePovRadarExp_ForceSpotted)) return;

    CEntityInstance * fakeController = GetFakePovRadarController();
    if(nullptr == fakeController) return;

    int fakeTeam = fakeController->GetTeam();
    int fakeSlot = fakeController->GetHandle().IsValid() ? fakeController->GetHandle().GetEntryIndex() : -1;
    int realSlot = -1;
    {
        CEntityInstance * realController = GetRealSplitScreenPlayer(0);
        if(realController && realController->GetHandle().IsValid())
            realSlot = realController->GetHandle().GetEntryIndex();
    }

    if((fakeTeam != 2 && fakeTeam != 3) || (fakeSlot < 0 && realSlot < 0)) return;

    g_SpottedRestoreCount = 0;
    int highestIndex = GetHighestEntityIndex();
    for(int i = 0; i < highestIndex + 1 && g_SpottedRestoreCount < kMaxSpottedRestoreEntries; ++i) {
        CEntityInstance * controller = GetEntityFromIndex(i);
        if(nullptr == controller || !controller->IsPlayerController()) continue;
        int team = controller->GetTeam();
        if(team != fakeTeam) continue;
        if(controller == fakeController) continue;

        auto pawnHandle = controller->GetPlayerPawnHandle();
        if(!pawnHandle.IsValid()) continue;
        CEntityInstance * pawn = GetEntityFromIndex(pawnHandle.GetEntryIndex());
        if(nullptr == pawn || !pawn->IsPlayerPawn()) continue;
        if(0 == g_clientDllOffsets.C_CSPlayerPawnBase.m_entitySpottedState) continue;

        auto spottedState = (unsigned char*)pawn + g_clientDllOffsets.C_CSPlayerPawnBase.m_entitySpottedState;
        uint8_t * pSpotted = (uint8_t*)(spottedState + g_clientDllOffsets.EntitySpottedState_t.m_bSpotted);
        uint32_t * pMask = (uint32_t*)(spottedState + g_clientDllOffsets.EntitySpottedState_t.m_bSpottedByMask);

        g_SpottedRestoreEntries[g_SpottedRestoreCount].pawnEntryIndex = pawnHandle.GetEntryIndex();
        g_SpottedRestoreEntries[g_SpottedRestoreCount].originalSpotted = *pSpotted;
        g_SpottedRestoreEntries[g_SpottedRestoreCount].originalMask[0] = pMask[0];
        g_SpottedRestoreEntries[g_SpottedRestoreCount].originalMask[1] = pMask[1];

        *pSpotted = 1;
        if(fakeSlot >= 0) {
            int maskWord = fakeSlot / 32;
            if(maskWord < 2) pMask[maskWord] |= (1u << (fakeSlot % 32));
        }
        if(realSlot >= 0 && realSlot != fakeSlot) {
            int maskWord = realSlot / 32;
            if(maskWord < 2) pMask[maskWord] |= (1u << (realSlot % 32));
        }

        g_SpottedRestoreCount++;
    }
}

static void RadarUpdate_RestoreTeammateSpotted() {
    for(int i = 0; i < g_SpottedRestoreCount; ++i) {
        CEntityInstance * pawn = GetEntityFromIndex(g_SpottedRestoreEntries[i].pawnEntryIndex);
        if(nullptr == pawn || !pawn->IsPlayerPawn()) continue;
        if(0 == g_clientDllOffsets.C_CSPlayerPawnBase.m_entitySpottedState) continue;
        auto spottedState = (unsigned char*)pawn + g_clientDllOffsets.C_CSPlayerPawnBase.m_entitySpottedState;
        uint8_t * pSpotted = (uint8_t*)(spottedState + g_clientDllOffsets.EntitySpottedState_t.m_bSpotted);
        uint32_t * pMask = (uint32_t*)(spottedState + g_clientDllOffsets.EntitySpottedState_t.m_bSpottedByMask);
        *pSpotted = g_SpottedRestoreEntries[i].originalSpotted;
        pMask[0] = g_SpottedRestoreEntries[i].originalMask[0];
        pMask[1] = g_SpottedRestoreEntries[i].originalMask[1];
    }
    g_SpottedRestoreCount = 0;
}

static volatile long g_HudRadarHookCallCount = 0;

void __fastcall New_HudRadarUpdate(void * This) {
    InterlockedIncrement(&g_HudRadarHookCallCount);
    if(IsFakePovRadarEnabled() && (g_FakePovRadarExperimentFlags & kFakePovRadarExp_ForceSpotted)) {
        RadarUpdate_ForceTeammateSpotted();
        g_Org_HudRadarUpdate(This);
        RadarUpdate_RestoreTeammateSpotted();
    } else {
        g_Org_HudRadarUpdate(This);
    }
}

long GetHudRadarHookCallCount() {
    return InterlockedExchange(&g_HudRadarHookCallCount, 0); // read and reset
}

bool Hook_HudRadarUpdate(HMODULE clientDll, int vtableSlot) {
    if(vtableSlot < 0) {
        if(g_HudRadarHookSlot >= 0 && g_Org_HudRadarUpdate != nullptr && g_pCCSGO_HudRadar_vtable != nullptr) {
            AfxDetourPtr((PVOID*)&(g_pCCSGO_HudRadar_vtable[g_HudRadarHookSlot]), g_Org_HudRadarUpdate, nullptr);
            g_Org_HudRadarUpdate = nullptr;
            g_HudRadarHookSlot = -1;
        }
        advancedfx::Message("[mirv_pov_radar] Radar hook disabled\n");
        return true;
    }

    if(nullptr == g_pCCSGO_HudRadar_vtable) {
        if(nullptr == clientDll) {
            advancedfx::Message("[mirv_pov_radar] No client.dll handle\n");
            return false;
        }
        g_pCCSGO_HudRadar_vtable = (void**)Afx::BinUtils::FindClassVtable(clientDll, ".?AVCCSGO_HudRadar@@", 0, 0);
        if(nullptr == g_pCCSGO_HudRadar_vtable) {
            advancedfx::Message("[mirv_pov_radar] CCSGO_HudRadar vtable not found\n");
            return false;
        }
        advancedfx::Message("[mirv_pov_radar] CCSGO_HudRadar vtable found at %p\n", (void*)g_pCCSGO_HudRadar_vtable);
    }

    if(g_HudRadarHookSlot >= 0 && g_HudRadarHookSlot != vtableSlot && g_Org_HudRadarUpdate != nullptr) {
        AfxDetourPtr((PVOID*)&(g_pCCSGO_HudRadar_vtable[g_HudRadarHookSlot]), g_Org_HudRadarUpdate, nullptr);
        g_Org_HudRadarUpdate = nullptr;
        g_HudRadarHookSlot = -1;
    }

    g_HudRadarHookSlot = vtableSlot;
    AfxDetourPtr((PVOID*)&(g_pCCSGO_HudRadar_vtable[vtableSlot]), New_HudRadarUpdate, (PVOID*)&g_Org_HudRadarUpdate);
    advancedfx::Message("[mirv_pov_radar] Hooked CCSGO_HudRadar vtable[%d] at %p (original=%p)\n", vtableSlot, (void*)New_HudRadarUpdate, (void*)g_Org_HudRadarUpdate);
    return true;
}

void * GetHudRadarVtable() {
    return g_pCCSGO_HudRadar_vtable;
}

int GetHudRadarHookSlot() {
    return g_HudRadarHookSlot;
}

extern "C" int afx_hook_source2_get_entity_ref_player_controller_handle(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetPlayerControllerHandle().ToInt();
    }
    return SOURCESDK_CS2_INVALID_EHANDLE_INDEX;  
}

extern "C" int afx_hook_source2_get_entity_ref_health(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetHealth();
    }
    return 0;    
}

extern "C" int afx_hook_source2_get_entity_ref_team(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        return pInstance->GetTeam();
    }
    return 0;    
}


extern "C" void afx_hook_source2_get_entity_ref_origin(void * pRef, float & x, float & y, float & z) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       pInstance->GetOrigin(x,y,z);
    }    
}

extern "C" void afx_hook_source2_get_entity_ref_render_eye_origin(void * pRef, float & x, float & y, float & z) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        float tmp[3];
       pInstance->GetRenderEyeOrigin(tmp);
       x = tmp[0];
       y = tmp[1];
       z = tmp[2];
    }    
}

extern "C" void afx_hook_source2_get_entity_ref_render_eye_angles(void * pRef, float & x, float & y, float & z) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
        float tmp[3];
       pInstance->GetRenderEyeAngles(tmp);
       x = tmp[0];
       y = tmp[1];
       z = tmp[2];
    }    
}

extern "C" int afx_hook_source2_get_entity_ref_view_entity_handle(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetViewEntityHandle().ToInt();
    }
    return SOURCESDK_CS2_INVALID_EHANDLE_INDEX;
}

extern "C" int afx_hook_source2_get_entity_ref_active_weapon_handle(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetActiveWeaponHandle().ToInt();
    }
    return SOURCESDK_CS2_INVALID_EHANDLE_INDEX;
}

extern "C" const char* afx_hook_source2_get_entity_ref_player_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetPlayerName();
    }
    return nullptr;
}

extern "C" uint64_t afx_hook_source2_get_entity_ref_steam_id(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetSteamId();
    }
    return 0;
}

extern "C" const char* afx_hook_source2_get_entity_ref_sanitized_player_name(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetSanitizedPlayerName();
    }
    return nullptr;
}

ClientDll_GetSplitScreenPlayer_t g_Org_ClientDll_GetSplitScreenPlayer = nullptr;

static CEntityInstance * __fastcall New_ClientDll_GetSplitScreenPlayer(int slot) {
    if(nullptr == g_Org_ClientDll_GetSplitScreenPlayer) return nullptr;
    if(0 == slot) {
        if(CEntityInstance * fakeController = GetFakePovRadarController()) {
            return fakeController;
        }
    }
    return g_Org_ClientDll_GetSplitScreenPlayer(slot);
}

bool Hook_GetSplitScreenPlayer( void* pAddr) {
    g_Org_ClientDll_GetSplitScreenPlayer = (ClientDll_GetSplitScreenPlayer_t)pAddr;
    g_ClientDll_GetSplitScreenPlayer = g_Org_ClientDll_GetSplitScreenPlayer;

    static bool s_Detoured = false;
    if(s_Detoured) return true;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_Org_ClientDll_GetSplitScreenPlayer, New_ClientDll_GetSplitScreenPlayer);

    if(NO_ERROR != DetourTransactionCommit()) {
        advancedfx::Message("[mirv_pov_radar_hook] GetSplitScreenPlayer detour failed\n");
        g_ClientDll_GetSplitScreenPlayer = (ClientDll_GetSplitScreenPlayer_t)pAddr;
        g_Org_ClientDll_GetSplitScreenPlayer = (ClientDll_GetSplitScreenPlayer_t)pAddr;
        return false;
    }

    s_Detoured = true;
    advancedfx::Message("[mirv_pov_radar_hook] GetSplitScreenPlayer detour installed\n");
    return true;
}

extern "C" void * afx_hook_source2_get_entity_ref_from_split_screen_player(int index) {
    if(0 == index) {
        if(CEntityInstance * result = GetRealSplitScreenPlayer(index)) {
            return CAfxEntityInstanceRef::Aquire(result);
        }
    }
    return nullptr;
}

extern "C" void * afx_hook_source2_get_entity_ref_from_effective_split_screen_player(int index) {
    if(0 == index) {
        if(CEntityInstance * result = GetEffectiveSplitScreenPlayer(index)) {
            return CAfxEntityInstanceRef::Aquire(result);
        }
    }
    return nullptr;
}

extern "C" FFIBool afx_hook_source2_is_fake_pov_radar_frame_context_active() {
    return BOOL_TO_FFIBOOL(IsFakePovRadarFrameContextActive());
}

extern "C" FFIBool afx_hook_source2_consume_fake_pov_radar_frame_context_was_active() {
    return BOOL_TO_FFIBOOL(ConsumeFakePovRadarFrameContextWasActive());
}

extern "C" uint8_t afx_hook_source2_get_entity_ref_observer_mode(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetObserverMode();
    }
    return 0;
}

extern "C" int afx_hook_source2_get_entity_ref_observer_target_handle(void * pRef) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
       return pInstance->GetObserverTarget().ToInt();
    }
    return SOURCESDK_CS2_INVALID_EHANDLE_INDEX;
}

extern "C" FFIBool afx_hook_source2_get_entity_ref_attachment(void * pRef, const char* attachmentName, double outPosition[3], double outAngles[4]) {
    if(auto pInstance = ((CAfxEntityInstanceRef *)pRef)->GetInstance()) {
		auto idx = pInstance->LookupAttachment(attachmentName);
		if (0 == idx) return FFIBOOL_FALSE;
		
		SOURCESDK::Vector origin;
		SOURCESDK::Quaternion angles;

		if (pInstance->GetAttachment(idx, origin, angles)) {
			outPosition[0] = origin.x;
			outPosition[1] = origin.y;
			outPosition[2] = origin.z;

			outAngles[0] = angles.w;
			outAngles[1] = angles.x;
			outAngles[2] = angles.y;
			outAngles[3] = angles.z;

			return FFIBOOL_TRUE;
		}
    }

    return FFIBOOL_FALSE;
}
