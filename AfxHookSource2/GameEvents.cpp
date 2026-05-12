#include "stdafx.h"

#include "GameEvents.h"

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../deps/release/prop/cs2/sdk_src/public/igameevents.h"
#include "../deps/release/prop/cs2/sdk_src/public/tier1/utlstring.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"

#include "AfxHookSource2Rs.h"
#include "ClientEntitySystem.h"

#include <Windows.h>
#include "../deps/release/Detours/src/detours.h"
#include <cstring>

namespace SOURCESDK {
namespace CS2 {

static inline uint32 MurmurHash2(const void * key, int len, uint32 seed)
{
	const uint32 m = 0x5bd1e995;
	const int r = 24;
	uint32 h = seed ^ len;

	const unsigned char * data = static_cast<const unsigned char *>(key);

	while (len >= 4)
	{
		uint32 k = data[0] | (uint32(data[1]) << 8) | (uint32(data[2]) << 16) | (uint32(data[3]) << 24);
		k *= m;
		k ^= k >> r;
		k *= m;

		h *= m;
		h ^= k;

		data += 4;
		len -= 4;
	}

	switch (len)
	{
	case 3:
		h ^= uint32(data[2]) << 16;
		[[fallthrough]];
	case 2:
		h ^= uint32(data[1]) << 8;
		[[fallthrough]];
	case 1:
		h ^= uint32(data[0]);
		h *= m;
		break;
	default:
		break;
	}

	h ^= h >> 13;
	h *= m;
	h ^= h >> 15;

	return h;
}

uint32 MurmurHash2LowerCase(char const * pString, uint32 nSeed)
{
	return MurmurHash2LowerCase(pString, pString ? (int)std::strlen(pString) : 0, nSeed);
}

uint32 MurmurHash2LowerCase(char const * pString, int nLength, uint32 nSeed)
{
	if (!pString || nLength <= 0) return MurmurHash2("", 0, nSeed);

	std::string lower;
	lower.reserve((size_t)nLength);
	for (int i = 0; i < nLength; ++i)
	{
		unsigned char c = static_cast<unsigned char>(pString[i]);
		lower.push_back((char)std::tolower(c));
	}

	return MurmurHash2(lower.data(), (int)lower.size(), nSeed);
}

} // namespace CS2 {
} // namespace SOURCESDK {

enum CS2GameEventKeyType
{
	CS2GameEventKeyType_Local = 0,
	CS2GameEventKeyType_CString = 1,
	CS2GameEventKeyType_Float = 2,
	CS2GameEventKeyType_Long = 3,
	CS2GameEventKeyType_Short = 4,
	CS2GameEventKeyType_Byte = 5,
	CS2GameEventKeyType_Bool = 6,
	CS2GameEventKeyType_Uint64 = 7
};


typedef SOURCESDK::CS2::CGameEvent * (*CGameEventManager_CreateEvent_t)( void * This, const char *name, bool bForce /*= false*/, int *pCookie /*= NULL*/ ); //:006
typedef bool (*CGameEventManager_FireEvent_t)( void * This, SOURCESDK::CS2::CGameEvent *event, bool bDontBroadcast /*= false*/ ); //:007
typedef bool (*CGameEventManager_FireEventClientSide_t)( void * This, SOURCESDK::CS2::CGameEvent *event ); //:008
typedef void (*CGameEventManager_FreeEvent_t)( void * This, SOURCESDK::CS2::CGameEvent *event ); //:010

void * g_pGameEventManager = nullptr;

CGameEventManager_CreateEvent_t g_CGameEventManager_CreateEvent = nullptr;
CGameEventManager_FreeEvent_t g_CGameEventManager_FreeEvent = nullptr;

CGameEventManager_FireEvent_t g_Old_CGameEventManager_FireEvent = nullptr;
CGameEventManager_FireEventClientSide_t g_Old_CGameEventManager_FireEventClientSide = nullptr;

//extern const char * GetStringForSymbol(int value);

//typedef void ( * DebugPrintKV3_t)(const struct KeyValues3 *);
typedef bool ( * SaveKV3AsJSON_t)( const struct SOURCESDK::CS2::KeyValues3* kv, SOURCESDK::CS2::CUtlString* error, SOURCESDK::CS2::CUtlString* output );

//DebugPrintKV3_t g_DebugPrintKV3 = nullptr;
SaveKV3AsJSON_t g_SaveKV3AsJSON = nullptr;

static bool IsRadarRelevantClientEvent(const char * name) {
	if(nullptr == name) return false;

	return 0 == std::strcmp(name, "player_team")
		|| 0 == std::strcmp(name, "local_player_team")
		|| 0 == std::strcmp(name, "local_player_controller_team")
		|| 0 == std::strcmp(name, "local_player_pawn_changed")
		|| 0 == std::strcmp(name, "entity_visible")
		|| 0 == std::strcmp(name, "spec_target_updated")
		|| 0 == std::strcmp(name, "spec_mode_updated");
}

static SOURCESDK::CS2::CKV3MemberName kGameEventKeyUserid("userid");
static SOURCESDK::CS2::CKV3MemberName kGameEventKeyTarget("target");
static SOURCESDK::CS2::CKV3MemberName kGameEventKeySubject("subject");
static SOURCESDK::CS2::CKV3MemberName kGameEventKeyClassname("classname");
static SOURCESDK::CS2::CKV3MemberName kGameEventKeyEntityname("entityname");

static const char * GetControllerName(CEntityInstance * controller) {
	if(nullptr == controller) return "<null>";

	const char * name = controller->GetSanitizedPlayerName();
	if(nullptr == name) name = controller->GetPlayerName();
	if(nullptr == name) name = controller->GetDebugName();
	if(nullptr == name) name = controller->GetClassName();
	return name ? name : "";
}

static const char * GetEntityName(CEntityInstance * entity) {
	if(nullptr == entity) return "<null>";

	const char * name = entity->GetSanitizedPlayerName();
	if(nullptr == name) name = entity->GetPlayerName();
	if(nullptr == name) name = entity->GetDebugName();
	if(nullptr == name) name = entity->GetName();
	if(nullptr == name) name = entity->GetClientClassName();
	if(nullptr == name) name = entity->GetClassName();
	return name ? name : "";
}

static const char * GetEntityClassName(CEntityInstance * entity) {
	if(nullptr == entity) return "<null>";

	const char * clientClassName = entity->GetClientClassName();
	if(nullptr != clientClassName && 0 != clientClassName[0]) return clientClassName;

	const char * className = entity->GetClassName();
	return className ? className : "";
}

static int GetEntityTeamSafe(CEntityInstance * entity) {
	if(nullptr == entity) return -1;
	return entity->GetTeam();
}

static int GetEntityHandleSafe(CEntityInstance * entity) {
	if(nullptr == entity) return SOURCESDK_CS2_INVALID_EHANDLE_INDEX;
	return entity->GetHandle().ToInt();
}

static CEntityInstance * GetPawnFromController(CEntityInstance * controller) {
	if(nullptr == controller || !controller->IsPlayerController()) return nullptr;
	return GetEntityFromIndex(controller->GetPlayerPawnHandle().GetEntryIndex());
}

static void PrintRadarRelevantClientEventKeys(SOURCESDK::CS2::CGameEvent * event) {
	if(nullptr == event) return;

	bool hasUserid = event->HasKey(kGameEventKeyUserid);
	bool hasTarget = event->HasKey(kGameEventKeyTarget);
	bool hasSubject = event->HasKey(kGameEventKeySubject);
	bool hasClassname = event->HasKey(kGameEventKeyClassname);
	bool hasEntityname = event->HasKey(kGameEventKeyEntityname);

	CEntityInstance * useridController = hasUserid ? reinterpret_cast<CEntityInstance *>(event->GetPlayerController(kGameEventKeyUserid)) : nullptr;
	CEntityInstance * targetEntity = hasTarget ? reinterpret_cast<CEntityInstance *>(event->GetEntity(kGameEventKeyTarget)) : nullptr;
	CEntityInstance * subjectEntity = hasSubject ? reinterpret_cast<CEntityInstance *>(event->GetEntity(kGameEventKeySubject)) : nullptr;
	CEntityInstance * useridPawn = GetPawnFromController(useridController);
	CEntityInstance * targetController = targetEntity && targetEntity->IsPlayerPawn() ? GetEntityFromIndex(targetEntity->GetPlayerControllerHandle().GetEntryIndex()) : (targetEntity && targetEntity->IsPlayerController() ? targetEntity : nullptr);
	CEntityInstance * targetPawn = targetEntity && targetEntity->IsPlayerController() ? GetPawnFromController(targetEntity) : targetEntity;
	CEntityInstance * subjectController = subjectEntity && subjectEntity->IsPlayerPawn() ? GetEntityFromIndex(subjectEntity->GetPlayerControllerHandle().GetEntryIndex()) : (subjectEntity && subjectEntity->IsPlayerController() ? subjectEntity : nullptr);
	CEntityInstance * subjectPawn = subjectEntity && subjectEntity->IsPlayerController() ? GetPawnFromController(subjectEntity) : subjectEntity;

	advancedfx::Message(
		"  event keys: userid=%i useridController=%s useridTeam=%i useridPawn=%i target=%i targetEntity=%s targetClass=%s targetTeam=%i targetController=%s targetPawn=%i subject=%i subjectEntity=%s subjectClass=%s subjectTeam=%i subjectController=%s subjectPawn=%i classname=%s entityname=%s\n"
		, hasUserid ? event->GetInt(kGameEventKeyUserid) : -1
		, GetControllerName(useridController)
		, GetEntityTeamSafe(useridController)
		, GetEntityHandleSafe(useridPawn)
		, hasTarget ? event->GetEHandle(kGameEventKeyTarget).ToInt() : SOURCESDK_CS2_INVALID_EHANDLE_INDEX
		, GetEntityName(targetEntity)
		, GetEntityClassName(targetEntity)
		, GetEntityTeamSafe(targetEntity)
		, GetControllerName(targetController)
		, GetEntityHandleSafe(targetPawn)
		, hasSubject ? event->GetEHandle(kGameEventKeySubject).ToInt() : SOURCESDK_CS2_INVALID_EHANDLE_INDEX
		, GetEntityName(subjectEntity)
		, GetEntityClassName(subjectEntity)
		, GetEntityTeamSafe(subjectEntity)
		, GetControllerName(subjectController)
		, GetEntityHandleSafe(subjectPawn)
		, hasClassname ? event->GetString(kGameEventKeyClassname) : "<none>"
		, hasEntityname ? event->GetString(kGameEventKeyEntityname) : "<none>"
	);
}

static void PrintRadarRelevantClientEventContext(const char * eventName) {
	CEntityInstance * realLocal = GetRealSplitScreenPlayer(0);
	CEntityInstance * effectiveLocal = GetEffectiveSplitScreenPlayer(0);
	CEntityInstance * fakeController = GetFakePovRadarController();
	CEntityInstance * realLocalPawn = realLocal ? GetEntityFromIndex(realLocal->GetPlayerPawnHandle().GetEntryIndex()) : nullptr;
	CEntityInstance * effectiveLocalPawn = effectiveLocal ? GetEntityFromIndex(effectiveLocal->GetPlayerPawnHandle().GetEntryIndex()) : nullptr;
	const bool framePatchActive = IsFakePovRadarFrameContextActive();
	const bool framePatchWasActive = ConsumeFakePovRadarFrameContextWasActive();

	advancedfx::Message(
		"[mirv_pov_radar_event] %s\n"
		"  frame patch: active=%s latchedSinceLastQuery=%s\n"
		"  real local controller: team=%i name=%s\n"
		"  effective local controller: team=%i name=%s\n"
		"  fake controller: team=%i name=%s\n"
		"  real local pawn: observerMode=%u observerTarget=%i viewEntity=%i\n"
		"  effective local pawn: observerMode=%u observerTarget=%i viewEntity=%i\n"
		, eventName ? eventName : "<null>"
		, framePatchActive ? "true" : "false"
		, framePatchWasActive ? "true" : "false"
		, realLocal ? realLocal->GetTeam() : -1
		, GetControllerName(realLocal)
		, effectiveLocal ? effectiveLocal->GetTeam() : -1
		, GetControllerName(effectiveLocal)
		, fakeController ? fakeController->GetTeam() : -1
		, GetControllerName(fakeController)
		, realLocalPawn ? realLocalPawn->GetObserverMode() : 0
		, realLocalPawn ? realLocalPawn->GetObserverTarget().ToInt() : SOURCESDK_CS2_INVALID_EHANDLE_INDEX
		, realLocalPawn ? realLocalPawn->GetViewEntityHandle().ToInt() : SOURCESDK_CS2_INVALID_EHANDLE_INDEX
		, effectiveLocalPawn ? effectiveLocalPawn->GetObserverMode() : 0
		, effectiveLocalPawn ? effectiveLocalPawn->GetObserverTarget().ToInt() : SOURCESDK_CS2_INVALID_EHANDLE_INDEX
		, effectiveLocalPawn ? effectiveLocalPawn->GetViewEntityHandle().ToInt() : SOURCESDK_CS2_INVALID_EHANDLE_INDEX
	);
}

static void PrintRadarRelevantClientEvent(SOURCESDK::CS2::CGameEvent * event) {
	const char * eventName = event ? event->GetName() : nullptr;
	if(nullptr == eventName || !IsRadarRelevantClientEvent(eventName)) return;

	SOURCESDK::CS2::CUtlString error;
	SOURCESDK::CS2::CUtlString output;
	if(g_SaveKV3AsJSON && event->GetDataKeys() && g_SaveKV3AsJSON(event->GetDataKeys(), &error, &output) && nullptr != output.Get()) {
		advancedfx::Message("[mirv_pov_radar_event_json] %s %s\n", eventName, output.Get());
	} else {
		advancedfx::Message("[mirv_pov_radar_event_json] %s <json unavailable>\n", eventName ? eventName : "<null>");
	}

	PrintRadarRelevantClientEventKeys(event);
	PrintRadarRelevantClientEventContext(eventName);
}


void SendGameEvent(SOURCESDK::CS2::CGameEvent *event) {

    static bool firstRun = true;
    if(firstRun) {
        firstRun = false;
		HMODULE hModule = GetModuleHandleA("tier0.dll");
		if (hModule)
		{
            //g_DebugPrintKV3 = (DebugPrintKV3_t)GetProcAddress(hModule,"?DebugPrintKV3@@YAXPEBVKeyValues3@@@Z");
			g_SaveKV3AsJSON = (SaveKV3AsJSON_t)GetProcAddress(hModule, "?SaveKV3AsJSON@@YA_NPEBVKeyValues3@@PEAVCUtlString@@1@Z");
		}        
    }

    SOURCESDK::CS2::CUtlString error;
    SOURCESDK::CS2::CUtlString output;

    if(g_SaveKV3AsJSON(event->GetDataKeys(),&error,&output) && nullptr != output) AfxHookSourceRs_Engine_OnGameEvent(event->GetName(), event->GetID(), output.Get());
    else advancedfx::Warning("Event: \"%s\" (%i): SaveKV3AsJSON failed: \"%s\"\n", event->GetName(), event->GetID(),error.Get() ? error.Get() : "[nullptr]");
}

bool New_CGameEventManager_FireEvent( void * This, SOURCESDK::CS2::CGameEvent *event, bool bDontBroadcast /*= false*/ ) {
    g_pGameEventManager = This;

    //advancedfx::Message("Server Event: %s\n", event->GetName());

    return g_Old_CGameEventManager_FireEvent(This, event, bDontBroadcast);
}

extern bool g_b_on_game_event;

bool New_CGameEventManager_FireEventClientSide( void * This, SOURCESDK::CS2::CGameEvent *event ) {
    g_pGameEventManager = This;

    PrintRadarRelevantClientEvent(event);

    if(g_b_on_game_event) SendGameEvent(event);

    return g_Old_CGameEventManager_FireEventClientSide(This, event);
}

bool Hook_CGameEventManager(void* addrClientDll) {
    static bool firstResult = false;
    static bool firstRun = true;

    if(firstRun) {
        firstRun = false;

        if(size_t arddrVtable = Afx::BinUtils::FindClassVtable((HMODULE)addrClientDll,".?AVCGameEventManager@@",0,0)) {
            void ** vtable = (void**)arddrVtable;
            g_CGameEventManager_CreateEvent = (CGameEventManager_CreateEvent_t)vtable[6];
            g_Old_CGameEventManager_FireEvent = (CGameEventManager_FireEvent_t)vtable[7];
            g_Old_CGameEventManager_FireEventClientSide = (CGameEventManager_FireEventClientSide_t)vtable[8];
            g_CGameEventManager_FreeEvent = (CGameEventManager_FreeEvent_t)vtable[10];
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)g_Old_CGameEventManager_FireEvent, New_CGameEventManager_FireEvent);
            DetourAttach(&(PVOID&)g_Old_CGameEventManager_FireEventClientSide, New_CGameEventManager_FireEventClientSide);
            firstResult = NO_ERROR == DetourTransactionCommit();
        }
    }

    return firstResult;
}
