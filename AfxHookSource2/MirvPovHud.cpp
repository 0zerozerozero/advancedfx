#include "stdafx.h"

#include "MirvPovHud.h"

#include "Globals.h"
#include "MirvPanorama.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"

#define WIN32_LEAN_AND_MEAN
#include "../deps/release/Detours/src/detours.h"

#include <stdint.h>
#include <string.h>

static unsigned char* MirvPovHud_FindPanelById(unsigned char* parentPanel, const char* panelId) {
    if(!parentPanel) return nullptr;

    const auto currentPanelId = *(char**)(parentPanel + CS2::PanoramaUIPanel::panelId);
    if(currentPanelId && 0 == strcmp(currentPanelId, panelId)) return parentPanel;

    const auto children = parentPanel + CS2::PanoramaUIPanel::children;
    const auto childCount = *(int*)children;
    for(int i = 0; i < childCount; ++i) {
        if(auto panel = MirvPovHud_FindPanelById(((unsigned char***)children)[1][i], panelId)) return panel;
    }

    return nullptr;
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

void MirvPovHud_OnPanoramaLayoutFileLoaded(const char* filePath) {
    if(0 != strcmp("panorama\\layout\\hud\\hudhealthammocenter.xml", filePath)) return;
    MirvPovHud_HideSpecPlayerPanel();
    MirvPovHud_ShowHealthAmmoCenterStrokes();
}

static int g_IsLocalPlayerHLTV_SuppressFrames = 0;
static int g_IsLocalPlayerHLTV_LastDemoTick = -1;

void MirvPovHud_UpdateSeekDetection(int curTick) {
    if(g_IsLocalPlayerHLTV_LastDemoTick >= 0) {
        int delta = curTick - g_IsLocalPlayerHLTV_LastDemoTick;
        if(delta < 0) delta = -delta;
        if(delta > 2) {
            g_IsLocalPlayerHLTV_SuppressFrames = 16;
        }
    }
    g_IsLocalPlayerHLTV_LastDemoTick = curTick;
    if(g_IsLocalPlayerHLTV_SuppressFrames > 0) {
        g_IsLocalPlayerHLTV_SuppressFrames--;
    }
}

bool MirvPovHud_ShouldSuppressFrame() {
    return g_IsLocalPlayerHLTV_SuppressFrames > 0;
}

// Hook GameStateAPI::IsLocalPlayerHLTV (sub_180EFF830) - Panorama bridge callback.
// The radar JS calls this to decide spectator vs player color mode.
// Return original behavior on the stable baseline.
typedef bool (__fastcall * IsLocalPlayerHLTV_t)();
static IsLocalPlayerHLTV_t g_Org_IsLocalPlayerHLTV = nullptr;
static bool g_bIsLocalPlayerHLTVHooked = false;

static bool __fastcall New_IsLocalPlayerHLTV() {
    return g_Org_IsLocalPlayerHLTV();
}

// Hook GameStateAPI::IsDemoOrHltv (sub_180EFEEE0) - Panorama bridge callback.
// Stable baseline keeps original demo/HLTV behavior.
typedef bool (__fastcall * IsDemoOrHltv_t)();
static IsDemoOrHltv_t g_Org_IsDemoOrHltv = nullptr;
static bool g_bIsDemoOrHltvHooked = false;

static bool __fastcall New_IsDemoOrHltv() {
    return g_Org_IsDemoOrHltv();
}

// Hook sub_180BD7830 (GetEffectiveLocalPlayer for HUD) - this function is
// used by the HUD to determine spectator state. It calls sub_1808E0E70(0)
// directly, bypassing our GetLocalPlayerController hook.
// Instead of hooking the function (which crashes during demo transitions),
// keep the spectator CSS state intact for xray/head markers.
static uint8_t * g_pHudSpectatorCheckPatchAddr = nullptr;
static uint8_t g_HudSpectatorCheckOrigByte = 0;
static bool g_bHudSpectatorCheckPatched = false;

static uint8_t * g_pFlashUpHudGatePatchAddr = nullptr;
static uint8_t g_FlashUpHudGateOrigBytes[2] = {};
static bool g_bFlashUpHudGatePatched = false;

static uint8_t * g_pFlashDownHudGatePatchAddr = nullptr;
static uint8_t g_FlashDownHudGateOrigBytes[5] = {};
static bool g_bFlashDownHudGatePatched = false;

static bool MirvPovHud_PatchTwoBytes(uint8_t* patchAddr, const uint8_t patchBytes[2], uint8_t originalBytes[2], const char* name) {
    DWORD oldProtect;
    if(!VirtualProtect(patchAddr, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        advancedfx::Warning("[mirv_pov_flash] VirtualProtect failed for %s (error %lu)\n", name, GetLastError());
        return false;
    }

    memcpy(originalBytes, patchAddr, 2);
    memcpy(patchAddr, patchBytes, 2);
    bool patched = 0 == memcmp(patchAddr, patchBytes, 2)
        && 0 != FlushInstructionCache(GetCurrentProcess(), patchAddr, 2);
    if(!patched) {
        memcpy(patchAddr, originalBytes, 2);
        FlushInstructionCache(GetCurrentProcess(), patchAddr, 2);
    }

    DWORD dummy;
    if(!VirtualProtect(patchAddr, 2, oldProtect, &dummy)) {
        advancedfx::Warning("[mirv_pov_flash] Failed to restore page protection for %s (error %lu)\n", name, GetLastError());
    }
    return patched;
}

static bool MirvPovHud_RestoreTwoBytes(uint8_t* patchAddr, uint8_t originalBytes[2], const char* name) {
    DWORD oldProtect;
    if(!VirtualProtect(patchAddr, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        advancedfx::Warning("[mirv_pov_flash] Failed to restore %s page protection (error %lu)\n", name, GetLastError());
        return false;
    }

    memcpy(patchAddr, originalBytes, 2);
    bool restored = 0 == memcmp(patchAddr, originalBytes, 2)
        && 0 != FlushInstructionCache(GetCurrentProcess(), patchAddr, 2);
    DWORD dummy;
    if(!VirtualProtect(patchAddr, 2, oldProtect, &dummy)) {
        advancedfx::Warning("[mirv_pov_flash] Failed to restore page protection for %s (error %lu)\n", name, GetLastError());
    }
    return restored;
}

static bool MirvPovHud_ApplyFlashDownHudGatePatch(HMODULE clientDll) {
    if(g_bFlashDownHudGatePatched) return true;

    const size_t matchAddr = getAddress(clientDll, "84 C0 74 4C 8B 85 B0 02 00 00 49 8D 8D 48 03 00 00");
    if(0 == matchAddr) {
        advancedfx::Warning("[mirv_pov_flash] flash down-HUD gate pattern not found\n");
        return false;
    }

    uint8_t * testAddr = (uint8_t *)matchAddr;
    uint8_t * patchAddr = testAddr - 5;
    if(0xE8 != patchAddr[0]) {
        advancedfx::Warning("[mirv_pov_flash] flash down-HUD gate call-site has unexpected opcode.\n");
        return false;
    }

    DWORD oldProtect;
    if(!VirtualProtect(patchAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        advancedfx::Warning("[mirv_pov_flash] VirtualProtect failed for flash down-HUD gate (error %lu)\n", GetLastError());
        return false;
    }

    memcpy(g_FlashDownHudGateOrigBytes, patchAddr, sizeof(g_FlashDownHudGateOrigBytes));
    const uint8_t patchBytes[5] = { 0xB8, 0, 0, 0, 0 }; // mov eax, 0
    memcpy(patchAddr, patchBytes, sizeof(patchBytes));
    bool patched = 0 == memcmp(patchAddr, patchBytes, sizeof(patchBytes))
        && 0 != FlushInstructionCache(GetCurrentProcess(), patchAddr, sizeof(patchBytes));
    if(!patched) {
        memcpy(patchAddr, g_FlashDownHudGateOrigBytes, sizeof(g_FlashDownHudGateOrigBytes));
        FlushInstructionCache(GetCurrentProcess(), patchAddr, sizeof(g_FlashDownHudGateOrigBytes));
    }
    DWORD dummy;
    if(!VirtualProtect(patchAddr, 5, oldProtect, &dummy)) {
        advancedfx::Warning("[mirv_pov_flash] Failed to restore flash down-HUD page protection (error %lu)\n", GetLastError());
    }
    if(!patched) {
        advancedfx::Warning("[mirv_pov_flash] Failed to apply flash down-HUD gate patch\n");
        return false;
    }

    g_pFlashDownHudGatePatchAddr = patchAddr;
    g_bFlashDownHudGatePatched = true;
    return true;
}

static void MirvPovHud_ApplyFlashHudGatePatches(HMODULE clientDll) {
    if(!g_bFlashUpHudGatePatched) {
        const size_t matchAddr = getAddress(clientDll, "48 8B F2 48 8B E9 E8 ?? ?? ?? ?? 84 C0 0F 85");
        if(0 == matchAddr) {
            advancedfx::Warning("[mirv_pov_flash] flash up-HUD gate pattern not found\n");
        } else {
            uint8_t * patchAddr = (uint8_t *)(matchAddr + 11);
            const uint8_t patchBytes[2] = { 0x30, 0xC0 };
            if(0x84 != patchAddr[0] || 0xC0 != patchAddr[1]) {
                advancedfx::Warning("[mirv_pov_flash] flash up-HUD gate landed on unexpected bytes.\n");
            } else if(MirvPovHud_PatchTwoBytes(patchAddr, patchBytes, g_FlashUpHudGateOrigBytes, "flash up-HUD gate")) {
                g_pFlashUpHudGatePatchAddr = patchAddr;
                g_bFlashUpHudGatePatched = true;
            }
        }
    }

    MirvPovHud_ApplyFlashDownHudGatePatch(clientDll);
}

static void MirvPovHud_RemoveFlashHudGatePatches() {
    if(g_bFlashUpHudGatePatched && g_pFlashUpHudGatePatchAddr) {
        if(MirvPovHud_RestoreTwoBytes(g_pFlashUpHudGatePatchAddr, g_FlashUpHudGateOrigBytes, "flash up-HUD gate")) {
            g_pFlashUpHudGatePatchAddr = nullptr;
            g_bFlashUpHudGatePatched = false;
        } else {
            advancedfx::Warning("[mirv_pov_flash] Failed to restore flash up-HUD gate\n");
        }
    }

    if(g_bFlashDownHudGatePatched && g_pFlashDownHudGatePatchAddr) {
        DWORD oldProtect;
        if(VirtualProtect(g_pFlashDownHudGatePatchAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(g_pFlashDownHudGatePatchAddr, g_FlashDownHudGateOrigBytes, sizeof(g_FlashDownHudGateOrigBytes));
            bool restored = 0 == memcmp(g_pFlashDownHudGatePatchAddr, g_FlashDownHudGateOrigBytes, sizeof(g_FlashDownHudGateOrigBytes))
                && 0 != FlushInstructionCache(GetCurrentProcess(), g_pFlashDownHudGatePatchAddr, 5);
            DWORD dummy;
            if(!VirtualProtect(g_pFlashDownHudGatePatchAddr, 5, oldProtect, &dummy)) {
                advancedfx::Warning("[mirv_pov_flash] Failed to restore flash down-HUD page protection (error %lu)\n", GetLastError());
            }
            if(restored) {
                g_pFlashDownHudGatePatchAddr = nullptr;
                g_bFlashDownHudGatePatched = false;
            } else {
                advancedfx::Warning("[mirv_pov_flash] Failed to restore flash down-HUD gate\n");
            }
        } else {
            advancedfx::Warning("[mirv_pov_flash] Failed to restore flash down-HUD page protection (error %lu)\n", GetLastError());
        }
    }
}

void MirvPovHud_ApplyPatches(HMODULE clientDll) {
    if(g_bHudSpectatorCheckPatched && g_bIsLocalPlayerHLTVHooked && g_bIsDemoOrHltvHooked
        && g_bFlashUpHudGatePatched && g_bFlashDownHudGatePatched) return;
    if(nullptr == clientDll) {
        advancedfx::Warning("[mirv_pov_radar_patch] No client.dll handle\n");
        return;
    }

    MirvPovHud_ApplyFlashHudGatePatches(clientDll);

    // --- Hook IsLocalPlayerHLTV (Panorama GameStateAPI callback) ---
    // DISABLED: interferes with xray / head markers in demo POV. Kept code for reference.
    if(false) {
        size_t funcAddr = getAddress(clientDll, "48 83 EC ?? 33 C9 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 80 B8");
        if(0 == funcAddr) {
            advancedfx::Warning("[mirv_pov_radar_patch] IsLocalPlayerHLTV pattern not found\n");
        } else {
            g_Org_IsLocalPlayerHLTV = (IsLocalPlayerHLTV_t)funcAddr;
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)g_Org_IsLocalPlayerHLTV, New_IsLocalPlayerHLTV);
            if(NO_ERROR == DetourTransactionCommit()) {
                g_bIsLocalPlayerHLTVHooked = true;
            } else {
                advancedfx::Warning("[mirv_pov_radar_patch] IsLocalPlayerHLTV detour failed\n");
                g_Org_IsLocalPlayerHLTV = nullptr;
            }
        }
    }

    // --- IsDemoOrHltv hook: DISABLED (interferes with xray / head markers in demo POV) ---
    if(false) {
        size_t funcAddr = 0;
        unsigned char * base = (unsigned char *)clientDll;
        IMAGE_DOS_HEADER * dosHeader = (IMAGE_DOS_HEADER *)base;
        IMAGE_NT_HEADERS * ntHeaders = (IMAGE_NT_HEADERS *)(base + dosHeader->e_lfanew);
        size_t size = ntHeaders->OptionalHeader.SizeOfImage;

        const char * searchStr = "IsDemoOrHltv";
        size_t searchLen = strlen(searchStr);
        size_t strAddr = 0;

        for(size_t i = 0; i + searchLen < size; i++) {
            if(0 == memcmp(base + i, searchStr, searchLen + 1)) {
                strAddr = (size_t)(base + i);
                break;
            }
        }

        if(strAddr) {
            // Find LEA instruction referencing this string (RIP-relative: REX.W 8D ModRM[rm=5] disp32)
            for(size_t i = 0; i + 7 < size; i++) {
                unsigned char * p = base + i;
                if((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0x07) == 0x05) {
                    int32_t disp = *(int32_t *)(p + 3);
                    size_t target = (size_t)(p + 7) + disp;
                    if(target == strAddr) {
                        // Found LEA loading "IsDemoOrHltv". Scan nearby for another LEA (function ptr).
                        for(int delta = -64; delta <= 64; delta++) {
                            if(delta >= -3 && delta <= 6) continue;
                            unsigned char * q = p + delta;
                            if(q < base || q + 7 >= base + size) continue;
                            if((q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8D && (q[2] & 0x07) == 0x05) {
                                int32_t disp2 = *(int32_t *)(q + 3);
                                size_t candidate = (size_t)(q + 7) + disp2;
                                if(candidate >= (size_t)base && candidate < (size_t)base + size) {
                                    unsigned char * cand = (unsigned char *)candidate;
                                    // Heuristic: looks like function prologue
                                    if(cand[0] == 0x48 || cand[0] == 0x40 || cand[0] == 0x55 ||
                                       cand[0] == 0x53 || cand[0] == 0x56 || cand[0] == 0x41 ||
                                       cand[0] == 0xB0 || (cand[0] == 0x33 && cand[1] == 0xC0) ||
                                       cand[0] == 0x8B) {
                                        funcAddr = candidate;
                                        break;
                                    }
                                }
                            }
                        }
                        if(funcAddr) break;
                    }
                }
            }
        } else {
            advancedfx::Warning("[mirv_pov_radar_patch] IsDemoOrHltv string not found in client.dll\n");
        }

        if(0 == funcAddr) {
            advancedfx::Warning("[mirv_pov_radar_patch] IsDemoOrHltv function not found\n");
            g_bIsDemoOrHltvHooked = true;
        } else {
            g_Org_IsDemoOrHltv = (IsDemoOrHltv_t)funcAddr;
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)g_Org_IsDemoOrHltv, New_IsDemoOrHltv);
            if(NO_ERROR == DetourTransactionCommit()) {
                g_bIsDemoOrHltvHooked = true;
            } else {
                advancedfx::Warning("[mirv_pov_radar_patch] IsDemoOrHltv detour failed\n");
                g_Org_IsDemoOrHltv = nullptr;
                g_bIsDemoOrHltvHooked = true;
            }
        }
    }

    // --- Patch 3: HUD spectator check (cmp byte ptr [rax+3EBh], 1 -> 0xFF) ---
    // DISABLED: this toggles the Panorama "HUD--localplayer--spectator" CSS class,
    // which also drives spectator head markers / xray overlay. Forcing it off removed
    // those. Bottom spectator bar is still hidden separately by Patch 4.
    if(false) {
        size_t match3 = getAddress(clientDll, "80 B8 EB 03 00 00 01 48 8B 11 41 0F 94 C0");
        if(0 == match3) {
            advancedfx::Warning("[mirv_pov_radar_patch] HUD spectator check pattern not found\n");
        } else {
            uint8_t * patchAddr = (uint8_t *)(match3 + 6);
            g_HudSpectatorCheckOrigByte = *patchAddr;

            DWORD oldProtect;
            if(VirtualProtect(patchAddr, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                *patchAddr = 0xFF;
                DWORD dummy;
                VirtualProtect(patchAddr, 1, oldProtect, &dummy);
                g_pHudSpectatorCheckPatchAddr = patchAddr;
                g_bHudSpectatorCheckPatched = true;
            } else {
                advancedfx::Warning("[mirv_pov_radar_patch] VirtualProtect failed for HUD spectator patch (error %lu)\n", GetLastError());
            }
        }
    }

    MirvPovHud_HideSpecPlayerPanel();
    MirvPovHud_ShowHealthAmmoCenterStrokes();

    return;
}

void MirvPovHud_RemovePatches() {
    MirvPovHud_RemoveFlashHudGatePatches();

    if(g_bIsLocalPlayerHLTVHooked && g_Org_IsLocalPlayerHLTV) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_Org_IsLocalPlayerHLTV, New_IsLocalPlayerHLTV);
        DetourTransactionCommit();
        g_bIsLocalPlayerHLTVHooked = false;
        g_IsLocalPlayerHLTV_SuppressFrames = 0;
        g_IsLocalPlayerHLTV_LastDemoTick = -1;
    }

    if(g_bIsDemoOrHltvHooked && g_Org_IsDemoOrHltv) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_Org_IsDemoOrHltv, New_IsDemoOrHltv);
        DetourTransactionCommit();
        g_bIsDemoOrHltvHooked = false;
    }

    if(g_bHudSpectatorCheckPatched && g_pHudSpectatorCheckPatchAddr) {
        DWORD oldProtect;
        if(VirtualProtect(g_pHudSpectatorCheckPatchAddr, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *g_pHudSpectatorCheckPatchAddr = g_HudSpectatorCheckOrigByte;
            DWORD dummy;
            VirtualProtect(g_pHudSpectatorCheckPatchAddr, 1, oldProtect, &dummy);
        }
        g_bHudSpectatorCheckPatched = false;
        g_pHudSpectatorCheckPatchAddr = nullptr;
    }

}

