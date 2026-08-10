#include "stdafx.h"

#include "MirvPovAudio.h"

#include "Globals.h"
#include "MirvPovFeedback.h"
#include "MirvPovSoundCircle.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"
#include "../deps/release/Detours/src/detours.h"

#include <Windows.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace {

using AudioParameterHandler_t = uintptr_t (__fastcall *)(const void *, void *, void *);
using DoStartSoundEvent_t = void (__fastcall *)(void *, void *);
using TriggerSoundControl_t = void (__fastcall *)(void *, uint32_t);
using LocalPlayerFilterCtor_t = void (__fastcall *)(void *);
using PlayLocalSound_t = void (__fastcall *)(
    void *,
    void *,
    int,
    const char *,
    float,
    const void *);

AudioParameterHandler_t g_OrgAudioParameterHandler = nullptr;
DoStartSoundEvent_t g_OrgDoStartSoundEvent = nullptr;
LocalPlayerFilterCtor_t g_LocalPlayerFilterCtor = nullptr;
PlayLocalSound_t g_PlayLocalSound = nullptr;
void ** g_SoundSystemSlot = nullptr;
void ** g_SoundEventInterfaceSlot = nullptr;
bool g_AudioParameterHooked = false;
bool g_SoundEventHooked = false;

constexpr size_t kAudioParameterDiscriminatorOffset = 0x48;
constexpr size_t kAudioParameterControlHashOffset = 0x4c;
constexpr uint32_t kAudioParameterPulseDiscriminator = 0x92e1d607;
constexpr size_t kSoundMessageEventHashOffset = 0x54;

void __fastcall New_DoStartSoundEvent(void * soundOpGameSystem, void * netMessage)
{
    uint32_t eventHash = 0;
    __try {
        if(nullptr != netMessage) {
            eventHash = *reinterpret_cast<uint32_t *>(
                reinterpret_cast<unsigned char *>(netMessage)
                + kSoundMessageEventHashOffset);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        eventHash = 0;
    }

    const char * eventName = MirvPovSoundCircle_GetNativeSoundEventName(eventHash);
    if(nullptr != eventName
        && MirvPovFeedback_ShouldSuppressNativeFlashRing(eventName)) return;

    g_OrgDoStartSoundEvent(soundOpGameSystem, netMessage);
    MirvPovSoundCircle_ProcessNativeSoundEvent(netMessage);
}

uintptr_t __fastcall New_AudioParameterHandler(
    const void * message,
    void * callbackContext,
    void * callbackData)
{
    uint32_t discriminator = 0;
    uint32_t controlHash = 0;
    __try {
        if(nullptr != message) {
            const unsigned char * bytes = reinterpret_cast<const unsigned char *>(message);
            discriminator = *reinterpret_cast<const uint32_t *>(
                bytes + kAudioParameterDiscriminatorOffset);
            controlHash = *reinterpret_cast<const uint32_t *>(
                bytes + kAudioParameterControlHashOffset);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        discriminator = 0;
        controlHash = 0;
    }

    if(kAudioParameterPulseDiscriminator == discriminator && 0 != controlHash) {
        bool suppress = false;
        __try {
            suppress = MirvPovFeedback_HandleNativeDeafen(controlHash);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            suppress = false;
        }
        if(suppress) return 0;
    }

    return g_OrgAudioParameterHandler(message, callbackContext, callbackData);
}

} // namespace

void MirvPovAudio_Initialize(HMODULE clientDll)
{
    if(nullptr == clientDll
        || (g_AudioParameterHooked
            && nullptr != g_SoundSystemSlot
            && nullptr != g_LocalPlayerFilterCtor
            && nullptr != g_PlayLocalSound
            && g_SoundEventHooked
            && nullptr != g_SoundEventInterfaceSlot)) return;

    size_t audioParameterHandler = getAddress(
        clientDll,
        "8B 41 ?? 48 8B D1 39 05");
    uint8_t * soundSystemLoad = 0 != audioParameterHandler
        ? reinterpret_cast<uint8_t *>(audioParameterHandler) + 0x52
        : nullptr;
    const uint8_t expectedLoad[] = {0x48, 0x8b, 0x0d};
    const uint8_t expectedControlTail[] = {
        0x8b, 0x52, 0x4c,
        0x48, 0x8b, 0x01,
        0x48, 0xff, 0xa0, 0xb8, 0x01, 0x00, 0x00,
        0xc3
    };
    bool audioParameterPathValid = nullptr != soundSystemLoad
        && 0 == memcmp(soundSystemLoad, expectedLoad, sizeof(expectedLoad))
        && 0 == memcmp(
            soundSystemLoad + 7,
            expectedControlTail,
            sizeof(expectedControlTail));
    if(audioParameterPathValid) {
        if(nullptr == g_SoundSystemSlot) {
            int32_t relative = *reinterpret_cast<int32_t *>(soundSystemLoad + 3);
            g_SoundSystemSlot = reinterpret_cast<void **>(
                soundSystemLoad + 7 + relative);
        }
        if(!g_AudioParameterHooked) {
            g_OrgAudioParameterHandler = reinterpret_cast<AudioParameterHandler_t>(
                audioParameterHandler);
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(
                &(PVOID &)g_OrgAudioParameterHandler,
                New_AudioParameterHandler);
            if(NO_ERROR == DetourTransactionCommit()) {
                g_AudioParameterHooked = true;
            } else {
                g_OrgAudioParameterHandler = nullptr;
                advancedfx::Warning("[mirv_pov_audio] Native AudioParameter detour failed.\n");
            }
        }
    } else if(nullptr == g_SoundSystemSlot || !g_AudioParameterHooked) {
        advancedfx::Warning("[mirv_pov_audio] Native flashbang-deafen control path was not found.\n");
    }

    if(nullptr == g_LocalPlayerFilterCtor || nullptr == g_PlayLocalSound) {
        size_t sendAudioHandler = getAddress(
            clientDll,
            "40 53 48 83 EC 60 48 8B 59 48 48 83 E3 FC 48 83 7B 18 0F 76");
        uint8_t * filterCtorCall = 0 != sendAudioHandler
            ? reinterpret_cast<uint8_t *>(sendAudioHandler) + 0x35
            : nullptr;
        uint8_t * playLocalSoundCall = 0 != sendAudioHandler
            ? reinterpret_cast<uint8_t *>(sendAudioHandler) + 0x64
            : nullptr;
        uint8_t * filterCtor = nullptr;
        uint8_t * playLocalSound = nullptr;
        if(nullptr != filterCtorCall && 0xe8 == filterCtorCall[0]) {
            int32_t relative = *reinterpret_cast<int32_t *>(filterCtorCall + 1);
            filterCtor = filterCtorCall + 5 + relative;
        }
        if(nullptr != playLocalSoundCall && 0xe8 == playLocalSoundCall[0]) {
            int32_t relative = *reinterpret_cast<int32_t *>(playLocalSoundCall + 1);
            playLocalSound = playLocalSoundCall + 5 + relative;
        }
        const uint8_t expectedFilterCtorPrefix[] = {0x48, 0x89, 0x5c, 0x24};
        const uint8_t expectedPlayLocalSoundPrefix[] = {0x40, 0x53, 0x48, 0x83, 0xec};
        if(nullptr != filterCtor
            && nullptr != playLocalSound
            && 0 == memcmp(
                filterCtor,
                expectedFilterCtorPrefix,
                sizeof(expectedFilterCtorPrefix))
            && 0 == memcmp(
                playLocalSound,
                expectedPlayLocalSoundPrefix,
                sizeof(expectedPlayLocalSoundPrefix))) {
            g_LocalPlayerFilterCtor = reinterpret_cast<LocalPlayerFilterCtor_t>(filterCtor);
            g_PlayLocalSound = reinterpret_cast<PlayLocalSound_t>(playLocalSound);
        } else {
            advancedfx::Warning("[mirv_pov_audio] Native local Ring path was not found.\n");
        }
    }

    if(nullptr == g_SoundEventInterfaceSlot || !g_SoundEventHooked) {
        size_t doStartSoundEvent = getAddress(
            clientDll,
            "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 81 EC 90 00 00 00 8B 42 40");
        uint8_t * createSoundEvent = nullptr;
        if(0 != doStartSoundEvent) {
            uint8_t * createSoundEventCall = reinterpret_cast<uint8_t *>(doStartSoundEvent) + 0xec;
            if(0xe8 == createSoundEventCall[0]) {
                int32_t relative = *reinterpret_cast<int32_t *>(createSoundEventCall + 1);
                createSoundEvent = createSoundEventCall + 5 + relative;
            }
        }

        if(nullptr != createSoundEvent) {
            size_t soundEventInterfaceUse = getAddress(
                clientDll,
                "41 8B D6 48 8B 0D ?? ?? ?? ?? 48 83 C1 08 48 8B 01 FF 90 88 00 00 00");
            uint8_t * soundEventGlobalLoad = 0 != soundEventInterfaceUse
                ? reinterpret_cast<uint8_t *>(soundEventInterfaceUse) + 3
                : nullptr;
            if(nullptr != soundEventGlobalLoad
                && createSoundEvent <= soundEventGlobalLoad
                && soundEventGlobalLoad < createSoundEvent + 0x100) {
                int32_t relative = *reinterpret_cast<int32_t *>(soundEventGlobalLoad + 3);
                g_SoundEventInterfaceSlot = reinterpret_cast<void **>(
                    soundEventGlobalLoad + 7 + relative);
            }
        }

        if(nullptr == g_SoundEventInterfaceSlot) {
            advancedfx::Warning("[mirv_pov_audio] Native sound-event interface path was not found.\n");
        } else {
            MirvPovSoundCircle_SetSoundEventInterfaceSlot(g_SoundEventInterfaceSlot);
            if(!g_SoundEventHooked && 0 != doStartSoundEvent) {
                g_OrgDoStartSoundEvent = reinterpret_cast<DoStartSoundEvent_t>(doStartSoundEvent);
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());
                DetourAttach(
                    &(PVOID &)g_OrgDoStartSoundEvent,
                    New_DoStartSoundEvent);
                if(NO_ERROR == DetourTransactionCommit()) {
                    g_SoundEventHooked = true;
                } else {
                    g_OrgDoStartSoundEvent = nullptr;
                    advancedfx::Warning("[mirv_pov_audio] Native sound-event detour failed.\n");
                }
            }
        }
    }
}

bool MirvPovAudio_ApplyDeafenControl(uint32_t controlHash)
{
    if(0 == controlHash || nullptr == g_SoundSystemSlot) return false;

    __try {
        void * soundSystem = *g_SoundSystemSlot;
        if(nullptr == soundSystem) return false;
        void ** vtable = *reinterpret_cast<void ***>(soundSystem);
        if(nullptr == vtable) return false;
        auto triggerControl = reinterpret_cast<TriggerSoundControl_t>(
            vtable[0x1b8 / sizeof(void *)]);
        if(nullptr == triggerControl) return false;
        triggerControl(soundSystem, controlHash);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool MirvPovAudio_PlayLocalFlashbangRing(const char * soundName)
{
    if(nullptr == soundName
        || '\0' == soundName[0]
        || nullptr == g_LocalPlayerFilterCtor
        || nullptr == g_PlayLocalSound) return false;

    // The native failure path writes through outHandle+0x13.
    alignas(16) unsigned char outHandle[0x20] = {};
    alignas(16) unsigned char localPlayerFilter[0x20] = {};
    __try {
        g_LocalPlayerFilterCtor(localPlayerFilter);
        g_PlayLocalSound(
            outHandle,
            localPlayerFilter,
            -1,
            soundName,
            1.0f,
            nullptr);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
