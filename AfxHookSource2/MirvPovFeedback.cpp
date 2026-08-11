#include "stdafx.h"

#include "MirvPovFeedback.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "MirvPovAudio.h"
#include "SchemaSystem.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"
#include "../deps/release/prop/cs2/sdk_src/public/igameevents.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <stdint.h>

namespace {

using PlayEntitySound_t = void (__fastcall *)(
    void * entity,
    void * filterEntity,
    const char * soundName);
using HashString_t = unsigned int (__fastcall *)(
    const char * string,
    unsigned int length,
    unsigned int lengthXorSeed);

PlayEntitySound_t g_PlayEntitySound = nullptr;
HashString_t g_HashString = nullptr;
std::atomic<int> g_CurrentDemoTick {-1};
int g_LastDemoTick = -1;
uint64_t g_FlashEventSequence = 0;

constexpr int kMaximumContinuousDemoTickDelta = 64;
constexpr int kMaximumFlashDetonationAgeTicks = 16;
constexpr size_t kRecentFlashDetonationCount = 16;
// server.dll player_hurt -> client.dll event hook -> SoundSystem001 +0x1B8,
// control.deafenHE (the native HE deafening control).
constexpr uint32_t kHeGrenadeDeafenControlHash = 0xb60b5483;

struct FlashSoundProfile {
    const char * ringSound;
    uint32_t deafenControlHash;
};

struct FlashDetonation {
    int entityId;
    float x;
    float y;
    float z;
    int demoTick;
    uint64_t sequence;
    bool valid;
};

const FlashSoundProfile kShortFlashSoundProfile = {
    "Flashbang.Ring.Short", 0x523f9894
};
const FlashSoundProfile kMediumFlashSoundProfile = {
    "Flashbang.Ring.Medium", 0x67bdb4cb
};
const FlashSoundProfile kLongFlashSoundProfile = {
    "Flashbang.Ring.Long", 0xf339fbbb
};

FlashDetonation g_RecentFlashDetonations[kRecentFlashDetonationCount] = {};
size_t g_NextFlashDetonation = 0;

int GetQueueTick()
{
    int tick = g_CurrentDemoTick.load();
    return 0 <= tick ? tick : 0;
}

SOURCESDK::CS2::GameEventKeySymbol_t MakeKey(const char * name)
{
    size_t length = strlen(name);
    unsigned int hash = g_HashString(
        name,
        static_cast<unsigned int>(length),
        static_cast<unsigned int>(length) ^ 0x31415926);
    return SOURCESDK::CS2::CKV3MemberName(static_cast<int>(hash), -1, name);
}

void ResetFlashDetonations()
{
    for(FlashDetonation & detonation : g_RecentFlashDetonations) {
        detonation.valid = false;
    }
    g_NextFlashDetonation = 0;
}

void RecordFlashDetonation(SOURCESDK::CS2::IGameEvent * event)
{
    if(nullptr == event) return;

    auto entityIdKey = MakeKey("entityid");
    auto xKey = MakeKey("x");
    auto yKey = MakeKey("y");
    auto zKey = MakeKey("z");
    if(!event->HasKey(entityIdKey)
        || !event->HasKey(xKey)
        || !event->HasKey(yKey)
        || !event->HasKey(zKey)) return;

    FlashDetonation & detonation = g_RecentFlashDetonations[g_NextFlashDetonation];
    detonation.entityId = event->GetInt(entityIdKey);
    detonation.x = event->GetFloat(xKey);
    detonation.y = event->GetFloat(yKey);
    detonation.z = event->GetFloat(zKey);
    detonation.demoTick = GetQueueTick();
    detonation.sequence = ++g_FlashEventSequence;
    detonation.valid = std::isfinite(detonation.x)
        && std::isfinite(detonation.y)
        && std::isfinite(detonation.z);
    g_NextFlashDetonation = (g_NextFlashDetonation + 1)
        % kRecentFlashDetonationCount;
}

const FlashSoundProfile * GetFlashSoundProfileForPawn(
    CEntityInstance * pawn,
    int entityId,
    int demoTick)
{
    const FlashDetonation * matchedDetonation = nullptr;
    for(const FlashDetonation & detonation : g_RecentFlashDetonations) {
        int age = demoTick - detonation.demoTick;
        if(!detonation.valid
            || detonation.entityId != entityId
            || age < 0
            || kMaximumFlashDetonationAgeTicks < age) continue;
        if(nullptr == matchedDetonation
            || matchedDetonation->sequence < detonation.sequence) {
            matchedDetonation = &detonation;
        }
    }

    if(nullptr == matchedDetonation) return nullptr;

    float eyeOrigin[3] = {};
    __try {
        pawn->GetRenderEyeOrigin(eyeOrigin);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }

    float dx = eyeOrigin[0] - matchedDetonation->x;
    float dy = eyeOrigin[1] - matchedDetonation->y;
    float dz = eyeOrigin[2] - (matchedDetonation->z + 1.0f);
    float distanceSquared = dx * dx + dy * dy + dz * dz;
    if(!std::isfinite(distanceSquared)) return nullptr;
    if(distanceSquared < 100.0f * 100.0f) return &kLongFlashSoundProfile;
    if(distanceSquared < 500.0f * 500.0f) return &kMediumFlashSoundProfile;
    if(distanceSquared < 1000.0f * 1000.0f) return &kShortFlashSoundProfile;
    return nullptr;
}

const FlashSoundProfile * GetFlashSoundProfileForRingSound(
    const char * soundName)
{
    if(nullptr == soundName) return nullptr;
    if(0 == strcmp(soundName, kShortFlashSoundProfile.ringSound)) {
        return &kShortFlashSoundProfile;
    }
    if(0 == strcmp(soundName, kMediumFlashSoundProfile.ringSound)) {
        return &kMediumFlashSoundProfile;
    }
    if(0 == strcmp(soundName, kLongFlashSoundProfile.ringSound)) {
        return &kLongFlashSoundProfile;
    }
    return nullptr;
}

bool IsPovAttack(
    SOURCESDK::CS2::IGameEvent * event,
    CEntityInstance *& attackerPawn,
    CEntityInstance *& victimPawn)
{
    attackerPawn = reinterpret_cast<CEntityInstance *>(
        event->GetPlayerPawn(MakeKey("attacker")));
    victimPawn = reinterpret_cast<CEntityInstance *>(
        event->GetPlayerPawn(MakeKey("userid")));
    CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
    if(nullptr == povPawn || nullptr == attackerPawn || nullptr == victimPawn) {
        return false;
    }
    return attackerPawn == povPawn && attackerPawn != victimPawn;
}

bool PawnHasHelmet(CEntityInstance * pawn)
{
    return nullptr != pawn
        && 0 != g_clientDllOffsets.C_CSPlayerPawn.m_bPrevHelmet
        && *reinterpret_cast<bool *>(
            reinterpret_cast<unsigned char *>(pawn)
            + g_clientDllOffsets.C_CSPlayerPawn.m_bPrevHelmet);
}

bool PawnHasArmor(CEntityInstance * pawn)
{
    return nullptr != pawn
        && 0 != g_clientDllOffsets.C_CSPlayerPawn.m_ArmorValue
        && 0 < *reinterpret_cast<int *>(
            reinterpret_cast<unsigned char *>(pawn)
            + g_clientDllOffsets.C_CSPlayerPawn.m_ArmorValue);
}

void Play(CEntityInstance * victimPawn, const char * soundName)
{
    if(nullptr == g_PlayEntitySound
        || nullptr == victimPawn
        || nullptr == soundName) return;
    g_PlayEntitySound(victimPawn, nullptr, soundName);
}

void PlayFlashRing(CEntityInstance * povPawn, const char * soundName)
{
    if(nullptr == soundName) return;

    bool playedLocally = false;
    __try {
        playedLocally = MirvPovAudio_PlayLocalFlashbangRing(soundName);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    if(playedLocally) return;

    __try {
        Play(povPawn, soundName);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
}

void HandlePlayerBlind(SOURCESDK::CS2::IGameEvent * event)
{
    CEntityInstance * victimPawn = reinterpret_cast<CEntityInstance *>(
        event->GetPlayerPawn(MakeKey("userid")));
    CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
    if(nullptr == victimPawn || victimPawn != povPawn) return;

    auto entityIdKey = MakeKey("entityid");
    if(!event->HasKey(entityIdKey)) return;

    const FlashSoundProfile * profile = GetFlashSoundProfileForPawn(
        victimPawn,
        event->GetInt(entityIdKey),
        GetQueueTick());
    if(nullptr == profile) return;

    MirvPovAudio_ApplyDeafenControl(profile->deafenControlHash);
    PlayFlashRing(povPawn, profile->ringSound);
}

void HandleHeGrenadeHurt(SOURCESDK::CS2::IGameEvent * event)
{
    CEntityInstance * victimPawn = reinterpret_cast<CEntityInstance *>(
        event->GetPlayerPawn(MakeKey("userid")));
    CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
    if(nullptr == victimPawn || victimPawn != povPawn) return;

    auto weaponKey = MakeKey("weapon");
    if(!event->HasKey(weaponKey)) return;
    const char * weapon = event->GetString(weaponKey);
    if(nullptr == weapon
        || (0 != _stricmp(weapon, "hegrenade")
            && 0 != _strnicmp(weapon, "hegrenade_", 10))) return;

    auto damageKey = MakeKey("dmg_health");
    if(!event->HasKey(damageKey) || event->GetInt(damageKey) <= 0) return;

    MirvPovAudio_ApplyDeafenControl(kHeGrenadeDeafenControlHash);
}

void HandleHurt(SOURCESDK::CS2::IGameEvent * event)
{
    CEntityInstance * attackerPawn = nullptr;
    CEntityInstance * victimPawn = nullptr;
    if(!IsPovAttack(event, attackerPawn, victimPawn)) return;

    auto healthKey = MakeKey("health");
    if(event->HasKey(healthKey) && event->GetInt(healthKey) <= 0) return;

    bool headshot = 1 == event->GetInt(MakeKey("hitgroup"));
    bool armored = headshot
        ? PawnHasHelmet(victimPawn)
        : PawnHasArmor(victimPawn)
            || 0 < event->GetInt(MakeKey("dmg_armor"));
    Play(victimPawn,
        headshot
            ? (armored
                ? "Player.DamageHeadShotArmor.AttackerFeedback"
                : "Player.DamageHeadShot.AttackerFeedback")
            : (armored
                ? "Player.DamageBodyArmor.AttackerFeedback"
                : "Player.DamageBody.AttackerFeedback"));
}

void HandleDeath(SOURCESDK::CS2::IGameEvent * event)
{
    CEntityInstance * attackerPawn = nullptr;
    CEntityInstance * victimPawn = nullptr;
    if(!IsPovAttack(event, attackerPawn, victimPawn)) return;

    bool headshot = event->GetBool(MakeKey("headshot"));
    bool armored = headshot ? PawnHasHelmet(victimPawn) : PawnHasArmor(victimPawn);
    Play(victimPawn,
        headshot
            ? (armored
                ? "Player.DeathHeadShotArmor.AttackerFeedback"
                : "Player.DeathHeadShot.AttackerFeedback")
            : (armored
                ? "Player.DeathBodyArmor.AttackerFeedback"
                : "Player.DeathBody.AttackerFeedback"));
}

} // namespace

void MirvPovFeedback_Initialize(HMODULE clientDll)
{
    if(nullptr == clientDll
        || (nullptr != g_PlayEntitySound && nullptr != g_HashString)) return;

    if(nullptr == g_PlayEntitySound) {
        g_PlayEntitySound = reinterpret_cast<PlayEntitySound_t>(getAddress(
            clientDll,
            "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 49 8B E8"));
    }
    if(nullptr == g_HashString) {
        g_HashString = reinterpret_cast<HashString_t>(getAddress(
            clientDll,
            "48 83 EC 28 45 8B D0 4C 8B C9 48 83 FA 04 0F 82 ?? ?? ?? ?? 0F B6 09 48 89 5C 24 20 8D 41 BF 3C 19 77 03 80 C1 20"));
    }
    if(nullptr == g_PlayEntitySound || nullptr == g_HashString) {
        advancedfx::Warning("[mirv_pov_feedback] Native sound path was not found.\n");
    }
}

void MirvPovFeedback_Reset()
{
    ResetFlashDetonations();
    g_FlashEventSequence = 0;
    g_LastDemoTick = -1;
    g_CurrentDemoTick = -1;
}

void MirvPovFeedback_Update(int demoTick)
{
    if(!MirvPov_IsEnabled() || demoTick < 0) {
        MirvPovFeedback_Reset();
        return;
    }

    if(0 <= g_LastDemoTick) {
        int tickDelta = demoTick - g_LastDemoTick;
        if(tickDelta < 0 || kMaximumContinuousDemoTickDelta < tickDelta) {
            ResetFlashDetonations();
        }
    }

    g_LastDemoTick = demoTick;
    g_CurrentDemoTick = demoTick;
}

void MirvPovFeedback_HandleGameEvent(SOURCESDK::CS2::IGameEvent * event)
{
    if(nullptr == event || !MirvPov_IsEnabled() || nullptr == g_HashString) return;
    const char * name = event->GetName();
    if(nullptr == name) return;

    __try {
        if(0 == strcmp(name, "flashbang_detonate")) {
            RecordFlashDetonation(event);
        } else if(0 == strcmp(name, "player_blind")) {
            HandlePlayerBlind(event);
        } else if(0 == strcmp(name, "player_hurt")) {
            HandleHeGrenadeHurt(event);
            HandleHurt(event);
        } else if(0 == strcmp(name, "player_death")) {
            HandleDeath(event);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool MirvPovFeedback_ShouldSuppressNativeFlashRing(const char * soundName)
{
    return MirvPov_IsEnabled()
        && nullptr != GetFlashSoundProfileForRingSound(soundName);
}

bool MirvPovFeedback_HandleNativeDeafen(uint32_t controlHash)
{
    if(!MirvPov_IsEnabled()) return false;
    return controlHash == kShortFlashSoundProfile.deafenControlHash
        || controlHash == kMediumFlashSoundProfile.deafenControlHash
        || controlHash == kLongFlashSoundProfile.deafenControlHash
        || controlHash == kHeGrenadeDeafenControlHash;
}
