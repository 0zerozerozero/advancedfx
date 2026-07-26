#include "stdafx.h"

#include "MirvPovSoundCircle.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "SchemaSystem.h"
#include "WrpConsole.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"
#include "../deps/release/Detours/src/detours.h"

#include <Windows.h>
#include <atomic>
#include <intrin.h>
#include <stdint.h>
#include <string.h>

#pragma intrinsic(_ReturnAddress)

namespace {

using GetLocalPawn_t = CEntityInstance * (__fastcall *)();
using DoStartSoundEvent_t = void (__fastcall *)(void *, void *);
using QueueRadarSound_t = void (__fastcall *)(CEntityInstance *, int, float, bool);
using HashSoundField_t = uint32_t (__fastcall *)(const char *, uint32_t);
using ResolveSoundEventId_t = uint32_t (__fastcall *)(void *, const char *, bool);
using GetSoundEventName_t = const char * (__fastcall *)(void *, uint32_t);
using GetSoundFieldCount_t = int (__fastcall *)(void *, uint32_t, uint32_t, bool *, bool);
using GetSoundFieldValue_t = bool (__fastcall *)(void *, uint32_t, uint32_t, const void **, int, bool);

GetLocalPawn_t g_OrgGetLocalPawn = nullptr;
DoStartSoundEvent_t g_OrgDoStartSoundEvent = nullptr;
QueueRadarSound_t g_QueueRadarSound = nullptr;
void ** g_SoundEventInterfaceSlot = nullptr;
void * g_LocalPawnAddress = nullptr;
void * g_QueueRadarSoundAddress = nullptr;
void * g_SoundGateReturnAddresses[3] = {};
uint32_t g_DistanceCurveKey = 0;
bool g_Hooked = false;
std::atomic_bool g_Enabled = true;
std::atomic_bool g_NativeProducerSeen = false;

std::atomic_uint64_t g_LocalPawnCalls = 0;
std::atomic_uint64_t g_GateMatches[3] = {};
std::atomic_uint64_t g_Overrides = 0;
std::atomic_uint64_t g_NoPov = 0;
std::atomic_uint64_t g_SosCalls = 0;
std::atomic_uint64_t g_SosPov = 0;
std::atomic_uint64_t g_SosSteps = 0;
std::atomic_uint64_t g_SosQueued = 0;
std::atomic_uint64_t g_SosNativeSkip = 0;
std::atomic_uint64_t g_Exceptions = 0;

uint32_t FinalizeSoundFieldHash(uint32_t hash)
{
    uint32_t result = hash * 0x5bd1e995;
    result ^= 0x66608f41;
    result = (result ^ (result >> 13)) * 0x5bd1e995;
    return result ^ (result >> 15);
}

CEntityInstance * ResolveSoundSourcePawn(int entityIndex)
{
    if(entityIndex < 0 || 0x7ffe < entityIndex) return nullptr;
    CEntityInstance * entity = GetEntityFromIndex(entityIndex);
    if(nullptr == entity) return nullptr;
    if(entity->IsPlayerPawn()) return entity;

    if(0 == g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode
        || 0 == g_clientDllOffsets.CGameSceneNode.m_pParent
        || 0 == g_clientDllOffsets.CGameSceneNode.m_pOwner) return nullptr;
    unsigned char * sceneNode = *reinterpret_cast<unsigned char **>(
        reinterpret_cast<unsigned char *>(entity)
        + g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode);
    if(nullptr == sceneNode) return nullptr;
    unsigned char * parentNode = *reinterpret_cast<unsigned char **>(
        sceneNode + g_clientDllOffsets.CGameSceneNode.m_pParent);
    if(nullptr == parentNode) return nullptr;
    CEntityInstance * parent = *reinterpret_cast<CEntityInstance **>(
        parentNode + g_clientDllOffsets.CGameSceneNode.m_pOwner);
    return nullptr != parent && parent->IsPlayerPawn() ? parent : nullptr;
}

bool IsCurrentPovPlayer(CEntityInstance * pawn)
{
    if(nullptr == pawn) return false;
    CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
    if(nullptr != povPawn && pawn == povPawn) return true;

    CEntityInstance * povController = GetCurrentPovPlayerController();
    if(nullptr == povController) return false;
    auto pawnController = pawn->GetPlayerControllerHandle();
    auto povControllerHandle = povController->GetHandle();
    return pawnController.IsValid()
        && povControllerHandle.IsValid()
        && pawnController.ToInt() == povControllerHandle.ToInt();
}

void * GetSoundEventInterface()
{
    void * interfaceBase = nullptr != g_SoundEventInterfaceSlot
        ? *g_SoundEventInterfaceSlot
        : nullptr;
    return nullptr != interfaceBase
        ? reinterpret_cast<unsigned char *>(interfaceBase) + 8
        : nullptr;
}

int GetNativeSoundRadius(void * soundEventInterface, uint32_t eventId)
{
    if(nullptr == soundEventInterface) return 0;
    void ** vtable = *reinterpret_cast<void ***>(soundEventInterface);
    auto getCount = reinterpret_cast<GetSoundFieldCount_t>(vtable[0x180 / sizeof(void *)]);
    auto getValue = reinterpret_cast<GetSoundFieldValue_t>(vtable[0x1a0 / sizeof(void *)]);
    if(nullptr == getCount || nullptr == getValue) return 0;

    const void * value = nullptr;
    if(getValue(soundEventInterface, eventId, 0x00442583, &value, 0, false)
        && nullptr != value
        && *reinterpret_cast<const float *>(value) <= 0.5f) return 0;

    float radius = 0.0f;
    value = nullptr;
    if(getValue(soundEventInterface, eventId, 0x7d58c040, &value, 0, false)
        && nullptr != value) {
        radius = *reinterpret_cast<const float *>(value);
    }
    if(radius <= 0.0f) {
        bool isArray = false;
        int count = getCount(
            soundEventInterface,
            eventId,
            g_DistanceCurveKey,
            &isArray,
            false);
        value = nullptr;
        if(isArray && 1 < count
            && getValue(
                soundEventInterface,
                eventId,
                g_DistanceCurveKey,
                &value,
                count - 1,
                false)
            && nullptr != value) {
            radius = *reinterpret_cast<const float *>(value);
        }
    }
    return 1.0f <= radius ? static_cast<int>(radius) : 0;
}

CEntityInstance * __fastcall New_GetLocalPawn()
{
    CEntityInstance * nativePawn = g_OrgGetLocalPawn();
    ++g_LocalPawnCalls;
    if(!g_Enabled.load() || !MirvPov_IsEnabled()) return nativePawn;

    void * returnAddress = _ReturnAddress();
    int gate = returnAddress == g_SoundGateReturnAddresses[0]
        ? 0
        : (returnAddress == g_SoundGateReturnAddresses[1]
            ? 1
            : (returnAddress == g_SoundGateReturnAddresses[2] ? 2 : -1));
    if(gate < 0) return nativePawn;

    ++g_GateMatches[gate];
    if(0 == gate) g_NativeProducerSeen = true;
    __try {
        CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
        if(nullptr == povPawn) {
            ++g_NoPov;
            return nativePawn;
        }
        ++g_Overrides;
        return povPawn;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        ++g_Exceptions;
        return nativePawn;
    }
}

void __fastcall New_DoStartSoundEvent(void * soundOpGameSystem, void * netMessage)
{
    ++g_SosCalls;
    uint32_t eventHash = 0;
    CEntityInstance * sourcePawn = nullptr;
    bool isPov = false;
    __try {
        if(nullptr != netMessage) {
            unsigned char * message = reinterpret_cast<unsigned char *>(netMessage);
            eventHash = *reinterpret_cast<uint32_t *>(message + 0x54);
            int sourceEntityIndex = *reinterpret_cast<int *>(message + 0x60);
            sourcePawn = ResolveSoundSourcePawn(sourceEntityIndex);
            isPov = IsCurrentPovPlayer(sourcePawn);
            if(isPov) ++g_SosPov;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        ++g_Exceptions;
    }

    g_OrgDoStartSoundEvent(soundOpGameSystem, netMessage);

    if(!g_Enabled.load()
        || !MirvPov_IsEnabled()
        || !isPov
        || nullptr == sourcePawn
        || 0 == eventHash) return;
    if(g_NativeProducerSeen.load()) {
        ++g_SosNativeSkip;
        return;
    }

    __try {
        void * soundEventInterface = GetSoundEventInterface();
        if(nullptr == soundEventInterface) return;
        void ** vtable = *reinterpret_cast<void ***>(soundEventInterface);
        auto getName = reinterpret_cast<GetSoundEventName_t>(vtable[0x10 / sizeof(void *)]);
        auto resolveEventId = reinterpret_cast<ResolveSoundEventId_t>(vtable[0]);
        if(nullptr == getName || nullptr == resolveEventId) return;

        const char * name = getName(soundEventInterface, eventHash);
        if(nullptr == name || nullptr == strstr(name, ".Step")) return;
        ++g_SosSteps;

        uint32_t eventId = resolveEventId(soundEventInterface, name, true);
        if(0 == eventId) return;
        int radius = GetNativeSoundRadius(soundEventInterface, eventId);
        if(radius < 1) return;

        g_QueueRadarSound(sourcePawn, radius, 0.5f, true);
        ++g_SosQueued;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        ++g_Exceptions;
    }
}

void ResetCounters()
{
    g_LocalPawnCalls = 0;
    g_GateMatches[0] = 0;
    g_GateMatches[1] = 0;
    g_GateMatches[2] = 0;
    g_Overrides = 0;
    g_NoPov = 0;
    g_SosCalls = 0;
    g_SosPov = 0;
    g_SosSteps = 0;
    g_SosQueued = 0;
    g_SosNativeSkip = 0;
    g_Exceptions = 0;
}

} // namespace

void MirvPovSoundCircle_Initialize(HMODULE clientDll)
{
    if(g_Hooked || nullptr == clientDll) return;

    size_t playerSoundProducer = getAddress(
        clientDll,
        "FF D3 8B D8 E8 ?? ?? ?? ?? 48 3B C6 0F 85 ?? ?? ?? ?? 0F 28 DE 44 88 74 24 20 44 8B C3 48 8D 0D ?? ?? ?? ?? 48 8B D6 E8 ?? ?? ?? ??");
    size_t queueRadarSound = getAddress(
        clientDll,
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 0F 29 74 24 30 41 0F B6 F9 0F 28 F2 8B F2 48 8B D9 E8 ?? ?? ?? ?? 48 3B C3 75 ??");
    size_t positionUpdater = getAddress(
        clientDll,
        "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? 00 00 48 89 6C 24 ?? 48 8D 54 24 ?? 48 89 74 24 ?? 48 8B C8");
    if(0 == playerSoundProducer || 0 == queueRadarSound || 0 == positionUpdater) {
        advancedfx::Warning(
            "[mirv_pov_sound_circle] Native radar sound path was not found (event=%d queue=%d position=%d).\n",
            0 != playerSoundProducer ? 1 : 0,
            0 != queueRadarSound ? 1 : 0,
            0 != positionUpdater ? 1 : 0);
        return;
    }

    uint8_t * callSites[3] = {
        reinterpret_cast<uint8_t *>(playerSoundProducer) + 4,
        reinterpret_cast<uint8_t *>(queueRadarSound) + 0x20,
        reinterpret_cast<uint8_t *>(positionUpdater) + 9
    };
    if(0xe8 != callSites[0][0] || 0xe8 != callSites[1][0] || 0xe8 != callSites[2][0]) {
        advancedfx::Warning("[mirv_pov_sound_circle] Native local-Pawn calls changed.\n");
        return;
    }

    uint8_t * localPawnTargets[3];
    for(int i = 0; i < 3; ++i) {
        int32_t relative = *reinterpret_cast<int32_t *>(callSites[i] + 1);
        localPawnTargets[i] = callSites[i] + 5 + relative;
    }
    const uint8_t expectedLocalPawnPrefix[] = {0x40, 0x53, 0x48, 0x83, 0xec};
    if(localPawnTargets[0] != localPawnTargets[1]
        || localPawnTargets[0] != localPawnTargets[2]
        || 0 != memcmp(
            localPawnTargets[0],
            expectedLocalPawnPrefix,
            sizeof(expectedLocalPawnPrefix))) {
        advancedfx::Warning("[mirv_pov_sound_circle] Native local-Pawn target validation failed.\n");
        return;
    }

    size_t doStartSoundEvent = getAddress(
        clientDll,
        "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 81 EC 90 00 00 00 8B 42 40");
    if(0 == doStartSoundEvent) {
        advancedfx::Warning("[mirv_pov_sound_circle] Native SOS start-sound handler was not found.\n");
        return;
    }
    uint8_t * createSoundEventCall = reinterpret_cast<uint8_t *>(doStartSoundEvent) + 0xec;
    if(0xe8 != createSoundEventCall[0]) {
        advancedfx::Warning("[mirv_pov_sound_circle] Native SOS create-sound call validation failed.\n");
        return;
    }
    int32_t createSoundEventRelative = *reinterpret_cast<int32_t *>(createSoundEventCall + 1);
    uint8_t * createSoundEvent = createSoundEventCall + 5 + createSoundEventRelative;
    uint8_t * soundEventGlobalLoad = createSoundEvent + 0xb8;
    const uint8_t expectedGlobalLoadTail[] = {0x41, 0x8b, 0xd4, 0x48, 0x83, 0xc1, 0x08};
    if(0x48 != soundEventGlobalLoad[0]
        || 0x8b != soundEventGlobalLoad[1]
        || 0x0d != soundEventGlobalLoad[2]
        || 0 != memcmp(
            soundEventGlobalLoad + 7,
            expectedGlobalLoadTail,
            sizeof(expectedGlobalLoadTail))) {
        advancedfx::Warning("[mirv_pov_sound_circle] Native sound-event interface validation failed.\n");
        return;
    }
    int32_t soundEventGlobalRelative = *reinterpret_cast<int32_t *>(soundEventGlobalLoad + 3);
    void ** soundEventInterfaceSlot = reinterpret_cast<void **>(
        soundEventGlobalLoad + 7 + soundEventGlobalRelative);

    size_t hashSoundField = getAddress(
        clientDll,
        "48 89 5C 24 08 44 0F B6 09 44 8B DA 4C 8B C1 41 8D 41 BF 3C 19 77 04 41 80 C1 20");
    if(0 == hashSoundField) {
        advancedfx::Warning("[mirv_pov_sound_circle] Native sound-field hash helper was not found.\n");
        return;
    }
    auto hashField = reinterpret_cast<HashSoundField_t>(hashSoundField);
    static const char distanceCurvePath[] = "public.distance_volume_mapping_curve";
    uint32_t curveHash = hashField(distanceCurvePath + 16, 0x26835b00);
    curveHash = hashField(distanceCurvePath + 24, curveHash);
    uint32_t distanceCurveKey = FinalizeSoundFieldHash(curveHash);
    if(0xd7da5bc8 != distanceCurveKey) {
        advancedfx::Warning(
            "[mirv_pov_sound_circle] Native distance-curve key validation failed (%08X).\n",
            distanceCurveKey);
        return;
    }

    g_LocalPawnAddress = localPawnTargets[0];
    g_QueueRadarSoundAddress = reinterpret_cast<void *>(queueRadarSound);
    g_OrgGetLocalPawn = reinterpret_cast<GetLocalPawn_t>(g_LocalPawnAddress);
    g_OrgDoStartSoundEvent = reinterpret_cast<DoStartSoundEvent_t>(doStartSoundEvent);
    g_QueueRadarSound = reinterpret_cast<QueueRadarSound_t>(g_QueueRadarSoundAddress);
    g_SoundEventInterfaceSlot = soundEventInterfaceSlot;
    g_DistanceCurveKey = distanceCurveKey;
    for(int i = 0; i < 3; ++i) g_SoundGateReturnAddresses[i] = callSites[i] + 5;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)g_OrgGetLocalPawn, New_GetLocalPawn);
    DetourAttach(&(PVOID &)g_OrgDoStartSoundEvent, New_DoStartSoundEvent);
    if(NO_ERROR != DetourTransactionCommit()) {
        g_OrgGetLocalPawn = nullptr;
        g_OrgDoStartSoundEvent = nullptr;
        g_QueueRadarSound = nullptr;
        g_SoundEventInterfaceSlot = nullptr;
        g_LocalPawnAddress = nullptr;
        g_QueueRadarSoundAddress = nullptr;
        g_DistanceCurveKey = 0;
        g_SoundGateReturnAddresses[0] = nullptr;
        g_SoundGateReturnAddresses[1] = nullptr;
        g_SoundGateReturnAddresses[2] = nullptr;
        advancedfx::Warning("[mirv_pov_sound_circle] Native sound-circle detour failed.\n");
        return;
    }

    g_Hooked = true;
    ResetCounters();
    advancedfx::Message(
        "[mirv_pov_sound_circle] Native SOS footstep circles enabled (getter=%p queue=%p handler=%p).\n",
        g_LocalPawnAddress,
        g_QueueRadarSoundAddress,
        reinterpret_cast<void *>(doStartSoundEvent));
}

CON_COMMAND(mirv_pov_sound_circle, "Control native POV radar sound circles.")
{
    if(2 <= args->ArgC()) {
        const char * value = args->ArgV(1);
        if(0 == _stricmp(value, "true") || 0 == _stricmp(value, "1") || 0 == _stricmp(value, "on")) {
            g_Enabled = true;
        } else if(0 == _stricmp(value, "false") || 0 == _stricmp(value, "0") || 0 == _stricmp(value, "off")) {
            g_Enabled = false;
        } else if(0 == _stricmp(value, "reset")) {
            ResetCounters();
        } else {
            advancedfx::Message("Usage: mirv_pov_sound_circle true|false|reset\n");
            return;
        }
    }

    advancedfx::Message(
        "mirv_pov_sound_circle enabled=%d applied=%d getter=%p queue=%p nativeSeen=%d calls=%llu gates=%llu,%llu,%llu overrides=%llu noPov=%llu sos=%llu sosPov=%llu steps=%llu queued=%llu nativeSkip=%llu exceptions=%llu\n",
        g_Enabled.load() ? 1 : 0,
        g_Hooked ? 1 : 0,
        g_LocalPawnAddress,
        g_QueueRadarSoundAddress,
        g_NativeProducerSeen.load() ? 1 : 0,
        static_cast<unsigned long long>(g_LocalPawnCalls.load()),
        static_cast<unsigned long long>(g_GateMatches[0].load()),
        static_cast<unsigned long long>(g_GateMatches[1].load()),
        static_cast<unsigned long long>(g_GateMatches[2].load()),
        static_cast<unsigned long long>(g_Overrides.load()),
        static_cast<unsigned long long>(g_NoPov.load()),
        static_cast<unsigned long long>(g_SosCalls.load()),
        static_cast<unsigned long long>(g_SosPov.load()),
        static_cast<unsigned long long>(g_SosSteps.load()),
        static_cast<unsigned long long>(g_SosQueued.load()),
        static_cast<unsigned long long>(g_SosNativeSkip.load()),
        static_cast<unsigned long long>(g_Exceptions.load()));
}
