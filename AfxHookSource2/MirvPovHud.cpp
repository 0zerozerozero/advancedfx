#include "stdafx.h"

#include "MirvPovHud.h"

#include "ClientEntitySystem.h"
#include "MirvPovCore.h"
#include "Globals.h"
#include "MirvPanorama.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"

#define WIN32_LEAN_AND_MEAN
#include "../deps/release/Detours/src/detours.h"

#include <stdint.h>
#include <intrin.h>
#include <string.h>

#pragma intrinsic(_ReturnAddress)

static unsigned char* MirvPovHud_FindPanelByIdRecursive(unsigned char* parentPanel, const char* panelId) {
    if(!parentPanel) return nullptr;

    const auto currentPanelId = *(char**)(parentPanel + CS2::PanoramaUIPanel::panelId);
    if(currentPanelId && 0 == strcmp(currentPanelId, panelId)) return parentPanel;

    const auto children = parentPanel + CS2::PanoramaUIPanel::children;
    const auto childCount = *(int*)children;
    for(int i = 0; i < childCount; ++i) {
        if(auto panel = MirvPovHud_FindPanelByIdRecursive(((unsigned char***)children)[1][i], panelId)) return panel;
    }

    return nullptr;
}

static unsigned char* MirvPovHud_FindPanelById(unsigned char* parentPanel, const char* panelId) {
    if(!parentPanel || !panelId) return nullptr;

    const auto currentPanelId = *(char**)(parentPanel + CS2::PanoramaUIPanel::panelId);
    if(currentPanelId && 0 == strcmp(currentPanelId, panelId)) return parentPanel;

    __try {
        typedef unsigned char* (__fastcall * FindChildTraverse_t)(unsigned char*, const char*);
        auto vtable = *(void***)parentPanel;
        auto findChildTraverse = vtable ? (FindChildTraverse_t)vtable[47] : nullptr;
        if(findChildTraverse) {
            if(auto panel = findChildTraverse(parentPanel, panelId)) return panel;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    return MirvPovHud_FindPanelByIdRecursive(parentPanel, panelId);
}

static bool MirvPovHud_PanelContainsId(unsigned char* parentPanel, const char* panelId) {
    return nullptr != MirvPovHud_FindPanelById(parentPanel, panelId);
}

static bool MirvPovHud_SetStrokeSiblingVisibleForAnchor(unsigned char* parentPanel, const char* anchorId) {
    if(!parentPanel) return false;

    const auto children = parentPanel + CS2::PanoramaUIPanel::children;
    const auto childCount = *(int*)children;

    if(2 == childCount) {
        int anchorChild = -1;
        for(int i = 0; i < childCount; ++i) {
            if(MirvPovHud_PanelContainsId(((unsigned char***)children)[1][i], anchorId)) {
                anchorChild = i;
                break;
            }
        }

        if(-1 != anchorChild) {
            const auto strokePanel = ((unsigned char***)children)[1][1 - anchorChild];
            if(Panorama_SetPanelVisible(strokePanel, true)) return true;
        }
    }

    for(int i = 0; i < childCount; ++i) {
        if(MirvPovHud_SetStrokeSiblingVisibleForAnchor(((unsigned char***)children)[1][i], anchorId)) return true;
    }

    return false;
}

static void MirvPovHud_ShowHealthAmmoCenterStrokes() {
    if(!CS2::PanoramaUIPanel::hudPanel) return;

    auto hudPanel = ((unsigned char***)CS2::PanoramaUIPanel::hudPanel)[0][1];
    if(!hudPanel) return;

    MirvPovHud_SetStrokeSiblingVisibleForAnchor(hudPanel, "hud-HA-main");
    MirvPovHud_SetStrokeSiblingVisibleForAnchor(hudPanel, "hud-WPN-main");
}

static void MirvPovHud_HideSpecPlayerPanel() {
    if(!CS2::PanoramaUIPanel::hudPanel) return;

    auto hudPanel = ((unsigned char***)CS2::PanoramaUIPanel::hudPanel)[0][1];
    if(!hudPanel) return;

    auto specPlayerBg = MirvPovHud_FindPanelById(hudPanel, "jsHudSpecplayer__Bg");
    if(specPlayerBg) Panorama_SetPanelVisible(specPlayerBg, false);

    auto specPlayerAvatar = MirvPovHud_FindPanelById(hudPanel, "HudSpecplayer__Avatar");
    if(specPlayerAvatar) Panorama_SetPanelVisible(specPlayerAvatar, false);
}

static unsigned char* g_SpectatorHotKeyLabelContainerPanel = nullptr;

static void MirvPovHud_SetSpectatorHotKeyLabelsVisible(bool visible) {
    if(!CS2::PanoramaUIPanel::hudPanel) return;

    __try {
        if(!g_SpectatorHotKeyLabelContainerPanel) {
            auto hudPanel = ((unsigned char***)CS2::PanoramaUIPanel::hudPanel)[0][1];
            if(!hudPanel) return;

            g_SpectatorHotKeyLabelContainerPanel = MirvPovHud_FindPanelById(
                hudPanel,
                "HotKeyLabelContainer");
        }

        // Reuse the native DemoUI state that hudlegend.css already handles:
        // .DemoControllerFull .HudSpecplayer__key-hints-text { visibility: collapse; }
        if(g_SpectatorHotKeyLabelContainerPanel) {
            if(!Panorama_SetPanelClass(
                g_SpectatorHotKeyLabelContainerPanel,
                "DemoControllerFull",
                !visible)) {
                g_SpectatorHotKeyLabelContainerPanel = nullptr;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_SpectatorHotKeyLabelContainerPanel = nullptr;
    }
}

void MirvPovHud_OnPanoramaLayoutFileLoaded(const char* filePath) {
    if(0 == strcmp("panorama\\layout\\hud\\hudhealthammocenter.xml", filePath)) {
        MirvPovHud_HideSpecPlayerPanel();
        MirvPovHud_ShowHealthAmmoCenterStrokes();
    } else if(0 == strcmp("panorama\\layout\\hud\\hudlegend.xml", filePath)) {
        g_SpectatorHotKeyLabelContainerPanel = nullptr;
        MirvPovHud_SetSpectatorHotKeyLabelsVisible(!MirvPov_IsEnabled());
    }
}

static int g_ScoreboardSeekSuppressFrames = 0;
static int g_LastDemoTick = -1;

void MirvPovHud_UpdateSeekDetection(int curTick) {
    MirvPovHud_SetSpectatorHotKeyLabelsVisible(false);

    if(g_LastDemoTick >= 0) {
        int delta = curTick - g_LastDemoTick;
        if(delta < 0) delta = -delta;
        if(delta > 2) {
            g_ScoreboardSeekSuppressFrames = 16;
        }
    }
    g_LastDemoTick = curTick;
    if(g_ScoreboardSeekSuppressFrames > 0) {
        g_ScoreboardSeekSuppressFrames--;
    }
}

bool MirvPovHud_ShouldSuppressFrame() {
    return g_ScoreboardSeekSuppressFrames > 0;
}

typedef bool (__fastcall * FlashViewPredicate_t)();
static FlashViewPredicate_t g_OrgFlashViewPredicate = nullptr;
static bool g_bFlashViewPredicateHooked = false;
static void * g_FlashViewPredicateReturnAddresses[2] = {};
static bool g_FlashHooksActive = false;

static uint8_t * MirvPovHud_GetRelativeCallTarget(uint8_t * callSite) {
    if(nullptr == callSite || 0xE8 != callSite[0]) return nullptr;
    int32_t relative = *reinterpret_cast<int32_t *>(callSite + 1);
    return callSite + 5 + relative;
}

static bool __fastcall New_FlashViewPredicate() {
    void * previousReturnAddress = MirvPov_PushHookReturnAddress(_ReturnAddress());
    void * returnAddress = MirvPov_GetHookReturnAddress();
    bool result = g_OrgFlashViewPredicate();
    MirvPov_PopHookReturnAddress(previousReturnAddress);

    if(!g_FlashHooksActive || !MirvPov_IsEnabled()) return result;
    if(returnAddress == g_FlashViewPredicateReturnAddresses[0]
        || returnAddress == g_FlashViewPredicateReturnAddresses[1]) return false;
    return result;
}

static bool MirvPovHud_ResolveFlashContexts(HMODULE clientDll) {
    const size_t compactPathMatch = getAddress(
        clientDll,
        "48 8B F2 48 8B E9 E8 ?? ?? ?? ?? 84 C0 0F 85");
    const size_t perViewPathMatch = getAddress(
        clientDll,
        "84 C0 74 4C 8B 85 B0 02 00 00 49 8D 8D 48 03 00 00");
    if(0 == compactPathMatch || 0 == perViewPathMatch) {
        advancedfx::Warning("[mirv_pov_flash] Flash render contexts were not found.\n");
        return false;
    }

    uint8_t * compactPredicateCall = reinterpret_cast<uint8_t *>(compactPathMatch) + 6;
    uint8_t * perViewPredicateCall = reinterpret_cast<uint8_t *>(perViewPathMatch) - 5;
    uint8_t * compactPredicateTarget = MirvPovHud_GetRelativeCallTarget(compactPredicateCall);
    uint8_t * perViewPredicateTarget = MirvPovHud_GetRelativeCallTarget(perViewPredicateCall);
    if(nullptr == compactPredicateTarget || compactPredicateTarget != perViewPredicateTarget) {
        advancedfx::Warning("[mirv_pov_flash] Flash view predicate validation failed.\n");
        return false;
    }

    g_OrgFlashViewPredicate = reinterpret_cast<FlashViewPredicate_t>(compactPredicateTarget);
    g_FlashViewPredicateReturnAddresses[0] = compactPredicateCall + 5;
    g_FlashViewPredicateReturnAddresses[1] = perViewPredicateCall + 5;
    return true;
}

static bool MirvPovHud_InstallFlashPredicateHook(HMODULE clientDll) {
    if(g_bFlashViewPredicateHooked) return true;
    if(!MirvPovHud_ResolveFlashContexts(clientDll)) return false;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)g_OrgFlashViewPredicate, New_FlashViewPredicate);
    if(NO_ERROR != DetourTransactionCommit()) {
        g_OrgFlashViewPredicate = nullptr;
        advancedfx::Warning("[mirv_pov_flash] Flash view predicate Detour failed.\n");
        return false;
    }

    g_bFlashViewPredicateHooked = true;
    return true;
}

static void MirvPovHud_RemoveFlashPredicateHook() {
    if(!g_bFlashViewPredicateHooked || nullptr == g_OrgFlashViewPredicate) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID &)g_OrgFlashViewPredicate, New_FlashViewPredicate);
    if(NO_ERROR != DetourTransactionCommit()) {
        advancedfx::Warning("[mirv_pov_flash] Flash view predicate Detour removal failed.\n");
        return;
    }

    g_bFlashViewPredicateHooked = false;
    g_OrgFlashViewPredicate = nullptr;
}

void MirvPovHud_ApplyPatches(HMODULE clientDll) {
    if(nullptr == clientDll) {
        advancedfx::Warning("[mirv_pov_hud] client.dll is not loaded.\n");
        return;
    }

    MirvPovHud_InstallFlashPredicateHook(clientDll);
    g_FlashHooksActive = true;

    MirvPovHud_HideSpecPlayerPanel();
    MirvPovHud_ShowHealthAmmoCenterStrokes();
    MirvPovHud_SetSpectatorHotKeyLabelsVisible(false);

    return;
}

void MirvPovHud_RemovePatches() {
    g_FlashHooksActive = false;
    MirvPovHud_RemoveFlashPredicateHook();
    g_FlashViewPredicateReturnAddresses[0] = nullptr;
    g_FlashViewPredicateReturnAddresses[1] = nullptr;
    MirvPovHud_SetSpectatorHotKeyLabelsVisible(true);
    g_SpectatorHotKeyLabelContainerPanel = nullptr;
    g_ScoreboardSeekSuppressFrames = 0;
    g_LastDemoTick = -1;
}
