#include "stdafx.h"

#include "MirvPovFeedback.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "SchemaSystem.h"
#include "WrpConsole.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"
#include "../deps/release/prop/cs2/sdk_src/public/igameevents.h"

#include <Windows.h>
#include <stdint.h>

namespace {

using PlayEntitySound_t = void (__fastcall *)(void * entity, void * filterEntity, const char * soundName);
using HashString_t = unsigned int (__fastcall *)(const char * string, unsigned int length, unsigned int lengthXorSeed);

PlayEntitySound_t g_PlayEntitySound = nullptr;
HashString_t g_HashString = nullptr;
uint64_t g_Events = 0;
uint64_t g_HurtEvents = 0;
uint64_t g_DeathEvents = 0;
uint64_t g_PovMatches = 0;
uint64_t g_Played = 0;
uint64_t g_NoPov = 0;
uint64_t g_MissingEntity = 0;
uint64_t g_Exceptions = 0;
const char * g_LastSound = nullptr;

SOURCESDK::CS2::GameEventKeySymbol_t MakeKey(const char * name)
{
    size_t length = strlen(name);
    unsigned int hash = g_HashString(
        name,
        static_cast<unsigned int>(length),
        static_cast<unsigned int>(length) ^ 0x31415926);
    return SOURCESDK::CS2::CKV3MemberName(static_cast<int>(hash), -1, name);
}

bool IsPovAttack(
    SOURCESDK::CS2::IGameEvent * event,
    CEntityInstance *& attackerPawn,
    CEntityInstance *& victimPawn)
{
    attackerPawn = reinterpret_cast<CEntityInstance *>(event->GetPlayerPawn(MakeKey("attacker")));
    victimPawn = reinterpret_cast<CEntityInstance *>(event->GetPlayerPawn(MakeKey("userid")));
    CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
    if(nullptr == povPawn) {
        ++g_NoPov;
        return false;
    }
    if(nullptr == attackerPawn || nullptr == victimPawn) {
        ++g_MissingEntity;
        return false;
    }
    if(attackerPawn != povPawn || attackerPawn == victimPawn) return false;
    ++g_PovMatches;
    return true;
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
    if(nullptr == g_PlayEntitySound || nullptr == victimPawn || nullptr == soundName) return;
    g_PlayEntitySound(victimPawn, nullptr, soundName);
    g_LastSound = soundName;
    ++g_Played;
}

void HandleHurt(SOURCESDK::CS2::IGameEvent * event)
{
    ++g_HurtEvents;
    CEntityInstance * attackerPawn = nullptr;
    CEntityInstance * victimPawn = nullptr;
    if(!IsPovAttack(event, attackerPawn, victimPawn)) return;

    auto healthKey = MakeKey("health");
    if(event->HasKey(healthKey) && event->GetInt(healthKey) <= 0) return;

    bool headshot = 1 == event->GetInt(MakeKey("hitgroup"));
    bool armored = headshot
        ? PawnHasHelmet(victimPawn)
        : PawnHasArmor(victimPawn) || 0 < event->GetInt(MakeKey("dmg_armor"));
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
    ++g_DeathEvents;
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
    if((nullptr != g_PlayEntitySound && nullptr != g_HashString) || nullptr == clientDll) return;
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
        advancedfx::Warning(
            "[mirv_pov_feedback] Initialization failed (sound=%d hash=%d).\n",
            nullptr != g_PlayEntitySound ? 1 : 0,
            nullptr != g_HashString ? 1 : 0);
    } else {
        advancedfx::Message("[mirv_pov_feedback] Event feedback initialized with the native entity sound system.\n");
    }
}

void MirvPovFeedback_HandleGameEvent(SOURCESDK::CS2::IGameEvent * event)
{
    if(nullptr == event || !MirvPov_IsEnabled() || nullptr == g_PlayEntitySound || nullptr == g_HashString) return;
    const char * name = event->GetName();
    if(nullptr == name) return;

    __try {
        if(0 == strcmp(name, "player_hurt")) {
            ++g_Events;
            HandleHurt(event);
        } else if(0 == strcmp(name, "player_death")) {
            ++g_Events;
            HandleDeath(event);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        ++g_Exceptions;
    }
}

CON_COMMAND(mirv_pov_feedback, "Inspect POV hit and kill feedback.")
{
    if(2 <= args->ArgC() && 0 == _stricmp(args->ArgV(1), "reset")) {
        g_Events = 0;
        g_HurtEvents = 0;
        g_DeathEvents = 0;
        g_PovMatches = 0;
        g_Played = 0;
        g_NoPov = 0;
        g_MissingEntity = 0;
        g_Exceptions = 0;
        g_LastSound = nullptr;
        advancedfx::Message("mirv_pov_feedback counters reset.\n");
        return;
    }

    advancedfx::Message(
        "mirv_pov_feedback initialized=%d events=%llu hurt=%llu death=%llu matches=%llu played=%llu noPov=%llu missingEntity=%llu exceptions=%llu\n"
        "  lastSound=%s\n",
        nullptr != g_PlayEntitySound && nullptr != g_HashString ? 1 : 0,
        (unsigned long long)g_Events,
        (unsigned long long)g_HurtEvents,
        (unsigned long long)g_DeathEvents,
        (unsigned long long)g_PovMatches,
        (unsigned long long)g_Played,
        (unsigned long long)g_NoPov,
        (unsigned long long)g_MissingEntity,
        (unsigned long long)g_Exceptions,
        nullptr != g_LastSound ? g_LastSound : "none");
}
