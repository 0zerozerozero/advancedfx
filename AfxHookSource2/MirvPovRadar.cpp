#include "stdafx.h"

#include "MirvPovRadar.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "MirvPovHud.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <limits.h>
#include <stdint.h>
#include <string.h>

namespace {

enum RadarPlayerStyle : uint64_t {
    RadarPlayerStyleCt = 9,
    RadarPlayerStyleT = 13,
    RadarPlayerStyleEnemy = 17
};

struct RadarPatchState {
    const char * name;
    uint8_t * address = nullptr;
    uint8_t originalBytes[16] = {};
    uint8_t * trampoline = nullptr;
    size_t size = 0;
    bool applied = false;
};

RadarPatchState g_RadarTeammatePatch = {"teammate visibility"};
RadarPatchState g_RadarEnemyColorPatch = {"enemy color"};
RadarPatchState g_RadarCompetitiveColorPathPatch = {"competitive color path"};
RadarPatchState g_RadarTCompetitiveColorPatch = {"T competitive color"};
RadarPatchState g_RadarCtCompetitiveColorPatch = {"CT competitive color"};

constexpr uint8_t kPushRegisters[] = {
    0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57,
    0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
};

constexpr uint8_t kPopRegisters[] = {
    0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C,
    0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58,
    0x5F, 0x5E, 0x5D, 0x5B, 0x5A, 0x59, 0x58
};

bool ShouldApplyRadarOverrides()
{
    return MirvPov_IsEnabled() && !MirvPovHud_ShouldSuppressFrame();
}

bool __fastcall ShouldForceTeammateVisible(CEntityInstance * targetPawn)
{
    if(!ShouldApplyRadarOverrides() || nullptr == targetPawn) return false;

    __try {
        CEntityInstance * observedPawn = GetCurrentPovPlayerPawn();
        return nullptr != observedPawn && observedPawn->GetTeam() == targetPawn->GetTeam();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t __fastcall AdjustRadarPlayerStyle(uint64_t style)
{
    if(!ShouldApplyRadarOverrides()) return style;

    __try {
        CEntityInstance * observedPawn = GetCurrentPovPlayerPawn();
        if(nullptr == observedPawn) return style;

        int observedTeam = observedPawn->GetTeam();
        if((3 == observedTeam && RadarPlayerStyleT == style)
            || (2 == observedTeam && RadarPlayerStyleCt == style)) {
            return RadarPlayerStyleEnemy;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }

    return style;
}

void EmitU8(uint8_t * code, size_t & pos, uint8_t value)
{
    code[pos++] = value;
}

void EmitU32(uint8_t * code, size_t & pos, uint32_t value)
{
    memcpy(code + pos, &value, sizeof(value));
    pos += sizeof(value);
}

void EmitU64(uint8_t * code, size_t & pos, uint64_t value)
{
    memcpy(code + pos, &value, sizeof(value));
    pos += sizeof(value);
}

void EmitBytes(uint8_t * code, size_t & pos, const uint8_t * bytes, size_t count)
{
    memcpy(code + pos, bytes, count);
    pos += count;
}

bool EmitRel32Jump(uint8_t * code, size_t & pos, uint8_t * target)
{
    intptr_t relative = target - (code + pos + 5);
    if(relative < INT32_MIN || INT32_MAX < relative) return false;

    EmitU8(code, pos, 0xE9);
    EmitU32(code, pos, static_cast<uint32_t>(static_cast<int32_t>(relative)));
    return true;
}

bool EmitRel32Jcc(uint8_t * code, size_t & pos, uint8_t condition, uint8_t * target)
{
    intptr_t relative = target - (code + pos + 6);
    if(relative < INT32_MIN || INT32_MAX < relative) return false;

    EmitU8(code, pos, 0x0F);
    EmitU8(code, pos, condition);
    EmitU32(code, pos, static_cast<uint32_t>(static_cast<int32_t>(relative)));
    return true;
}

void EmitAbsoluteCall(uint8_t * code, size_t & pos, const void * target)
{
    EmitU8(code, pos, 0x48);
    EmitU8(code, pos, 0xB8); // mov rax, imm64
    EmitU64(code, pos, reinterpret_cast<uint64_t>(target));
    EmitU8(code, pos, 0xFF);
    EmitU8(code, pos, 0xD0); // call rax
}

void EmitAbsoluteIndirectCall(uint8_t * code, size_t & pos, const void * target)
{
    EmitU8(code, pos, 0xFF);
    EmitU8(code, pos, 0x15);
    EmitU32(code, pos, 2); // call qword ptr [rip+2]
    EmitU8(code, pos, 0xEB);
    EmitU8(code, pos, 8); // Skip the inline target after returning.
    EmitU64(code, pos, reinterpret_cast<uint64_t>(target));
}

void EmitSaveContext(uint8_t * code, size_t & pos, uint8_t stackAllocation)
{
    EmitBytes(code, pos, kPushRegisters, sizeof(kPushRegisters));

    EmitU8(code, pos, 0x48);
    EmitU8(code, pos, 0x81);
    EmitU8(code, pos, 0xEC); // sub rsp, stackAllocation
    EmitU32(code, pos, stackAllocation);

    for(uint8_t xmm = 0; xmm < 6; ++xmm) {
        EmitU8(code, pos, 0xF3);
        EmitU8(code, pos, 0x0F);
        EmitU8(code, pos, 0x7F); // movdqu [rsp+disp8], xmmN
        EmitU8(code, pos, static_cast<uint8_t>(0x44 + 8 * xmm));
        EmitU8(code, pos, 0x24);
        EmitU8(code, pos, static_cast<uint8_t>(0x20 + 0x10 * xmm));
    }
}

void EmitRestoreContext(uint8_t * code, size_t & pos, uint8_t stackAllocation)
{
    for(uint8_t xmm = 0; xmm < 6; ++xmm) {
        EmitU8(code, pos, 0xF3);
        EmitU8(code, pos, 0x0F);
        EmitU8(code, pos, 0x6F); // movdqu xmmN, [rsp+disp8]
        EmitU8(code, pos, static_cast<uint8_t>(0x44 + 8 * xmm));
        EmitU8(code, pos, 0x24);
        EmitU8(code, pos, static_cast<uint8_t>(0x20 + 0x10 * xmm));
    }

    EmitU8(code, pos, 0x48);
    EmitU8(code, pos, 0x81);
    EmitU8(code, pos, 0xC4); // add rsp, stackAllocation
    EmitU32(code, pos, stackAllocation);
}

uint8_t * AllocateNear(uint8_t * target, size_t size)
{
    SYSTEM_INFO systemInfo = {};
    GetSystemInfo(&systemInfo);

    const uintptr_t granularity = systemInfo.dwAllocationGranularity;
    const uintptr_t targetAddress = reinterpret_cast<uintptr_t>(target);
    const uintptr_t minimumAddress = targetAddress > 0x7fff0000
        ? targetAddress - 0x7fff0000
        : 0;
    const uintptr_t maximumAddress = targetAddress + 0x7fff0000;

    for(uintptr_t offset = 0; offset < 0x7fff0000; offset += granularity) {
        if(targetAddress >= offset + granularity) {
            uintptr_t address = (targetAddress - offset) & ~(granularity - 1);
            if(minimumAddress <= address) {
                if(void * result = VirtualAlloc(
                    reinterpret_cast<void *>(address),
                    size,
                    MEM_COMMIT | MEM_RESERVE,
                    PAGE_EXECUTE_READWRITE)) {
                    return static_cast<uint8_t *>(result);
                }
            }
        }

        uintptr_t address = (targetAddress + offset + granularity - 1) & ~(granularity - 1);
        if(address <= maximumAddress) {
            if(void * result = VirtualAlloc(
                reinterpret_cast<void *>(address),
                size,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_EXECUTE_READWRITE)) {
                return static_cast<uint8_t *>(result);
            }
        }
    }

    return nullptr;
}

bool ApplyPatch(
    RadarPatchState & state,
    uint8_t * address,
    size_t size,
    uint8_t * trampoline)
{
    if(state.applied) return true;
    if(nullptr == address || nullptr == trampoline || size < 5 || sizeof(state.originalBytes) < size) {
        return false;
    }

    intptr_t relative = trampoline - (address + 5);
    if(relative < INT32_MIN || INT32_MAX < relative) return false;

    DWORD oldProtect = 0;
    if(!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        advancedfx::Warning(
            "[mirv_pov_radar] VirtualProtect failed for %s (error %lu).\n",
            state.name,
            GetLastError());
        return false;
    }

    memcpy(state.originalBytes, address, size);
    address[0] = 0xE9;
    *reinterpret_cast<int32_t *>(address + 1) = static_cast<int32_t>(relative);
    memset(address + 5, 0x90, size - 5);
    FlushInstructionCache(GetCurrentProcess(), address, size);

    DWORD unused = 0;
    if(!VirtualProtect(address, size, oldProtect, &unused)) {
        advancedfx::Warning(
            "[mirv_pov_radar] Failed to restore protection for %s (error %lu).\n",
            state.name,
            GetLastError());
    }

    state.address = address;
    state.trampoline = trampoline;
    state.size = size;
    state.applied = true;
    return true;
}

bool RestorePatch(RadarPatchState & state)
{
    if(!state.applied) return true;

    DWORD oldProtect = 0;
    if(!VirtualProtect(state.address, state.size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        advancedfx::Warning(
            "[mirv_pov_radar] Could not restore %s patch (error %lu).\n",
            state.name,
            GetLastError());
        return false;
    }

    memcpy(state.address, state.originalBytes, state.size);
    FlushInstructionCache(GetCurrentProcess(), state.address, state.size);

    DWORD unused = 0;
    if(!VirtualProtect(state.address, state.size, oldProtect, &unused)) {
        advancedfx::Warning(
            "[mirv_pov_radar] Failed to restore protection for %s (error %lu).\n",
            state.name,
            GetLastError());
    }

    VirtualFree(state.trampoline, 0, MEM_RELEASE);
    state.address = nullptr;
    state.trampoline = nullptr;
    state.size = 0;
    state.applied = false;
    return true;
}

bool PatchCompetitiveColorPath(HMODULE clientDll)
{
    if(g_RadarCompetitiveColorPathPatch.applied) return true;

    size_t match = getAddress(
        clientDll,
        "4C 89 6C 24 ?? 84 DB 0F 84");
    if(0 == match) {
        advancedfx::Warning("[mirv_pov_radar] Competitive color path pattern not found.\n");
        return false;
    }

    uint8_t * address = reinterpret_cast<uint8_t *>(match);
    constexpr size_t patchSize = 5;
    uint8_t * trampoline = AllocateNear(address, 64);
    if(nullptr == trampoline) {
        advancedfx::Warning("[mirv_pov_radar] Could not allocate competitive color path trampoline.\n");
        return false;
    }

    size_t pos = 0;
    EmitBytes(trampoline, pos, address, patchSize); // mov [rsp+disp8], r13
    EmitU8(trampoline, pos, 0x31);
    EmitU8(trampoline, pos, 0xDB); // xor ebx, ebx

    bool emitted = EmitRel32Jump(trampoline, pos, address + patchSize);
    if(!emitted || !ApplyPatch(
        g_RadarCompetitiveColorPathPatch,
        address,
        patchSize,
        trampoline)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    return true;
}

bool PatchCompetitiveTeamColor(
    HMODULE clientDll,
    RadarPatchState & state,
    const char * pattern,
    uint32_t team)
{
    if(state.applied) return true;

    size_t match = getAddress(clientDll, pattern);
    if(0 == match) {
        advancedfx::Warning("[mirv_pov_radar] %s pattern not found.\n", state.name);
        return false;
    }

    uint8_t * address = reinterpret_cast<uint8_t *>(match);
    constexpr size_t patchSize = 5;
    int32_t originalCall = *reinterpret_cast<int32_t *>(address + 1);
    uint8_t * originalCallTarget = address + patchSize + originalCall;
    uint8_t * trampoline = AllocateNear(address, 64);
    if(nullptr == trampoline) {
        advancedfx::Warning(
            "[mirv_pov_radar] Could not allocate %s trampoline.\n",
            state.name);
        return false;
    }

    size_t pos = 0;
    EmitAbsoluteIndirectCall(trampoline, pos, originalCallTarget);
    EmitU8(trampoline, pos, 0xB8);
    EmitU32(trampoline, pos, team); // Override the original call's return value.

    bool emitted = EmitRel32Jump(trampoline, pos, address + patchSize);
    if(!emitted || !ApplyPatch(state, address, patchSize, trampoline)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    return true;
}

bool PatchTeammateVisibility(HMODULE clientDll)
{
    if(g_RadarTeammatePatch.applied) return true;

    size_t match = getAddress(
        clientDll,
        "38 5C 24 ?? 0F 84 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ??");
    if(0 == match) {
        advancedfx::Warning("[mirv_pov_radar] Teammate visibility pattern not found.\n");
        return false;
    }

    uint8_t * address = reinterpret_cast<uint8_t *>(match);
    constexpr size_t patchSize = 10;
    uint8_t * continueAddress = address + patchSize;
    int32_t originalBranch = *reinterpret_cast<int32_t *>(address + 6);
    uint8_t * originalBranchTarget = continueAddress + originalBranch;

    uint8_t * trampoline = AllocateNear(address, 512);
    if(nullptr == trampoline) {
        advancedfx::Warning("[mirv_pov_radar] Could not allocate teammate visibility trampoline.\n");
        return false;
    }

    size_t pos = 0;
    EmitSaveContext(trampoline, pos, 0x88); // Mid-function RSP is 16-byte aligned.
    EmitU8(trampoline, pos, 0x4C);
    EmitU8(trampoline, pos, 0x89);
    EmitU8(trampoline, pos, 0xF9); // mov rcx, r15
    EmitAbsoluteCall(trampoline, pos, reinterpret_cast<const void *>(&ShouldForceTeammateVisible));
    EmitRestoreContext(trampoline, pos, 0x88);
    EmitU8(trampoline, pos, 0x84);
    EmitU8(trampoline, pos, 0xC0); // test al, al
    EmitBytes(trampoline, pos, kPopRegisters, sizeof(kPopRegisters));

    bool emitted = EmitRel32Jcc(trampoline, pos, 0x85, continueAddress); // jne display path
    EmitBytes(trampoline, pos, address, 4); // cmp byte ptr [rsp+disp8], bl
    emitted = emitted && EmitRel32Jcc(trampoline, pos, 0x84, originalBranchTarget);
    emitted = emitted && EmitRel32Jump(trampoline, pos, continueAddress);
    if(!emitted || !ApplyPatch(g_RadarTeammatePatch, address, patchSize, trampoline)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    return true;
}

bool PatchEnemyColor(HMODULE clientDll)
{
    if(g_RadarEnemyColorPatch.applied) return true;

    size_t match = getAddress(
        clientDll,
        "48 8B 6C 24 ?? 41 39 9E ?? ?? ?? ?? 74 ?? 33 D2");
    if(0 == match) {
        advancedfx::Warning("[mirv_pov_radar] Enemy color pattern not found.\n");
        return false;
    }

    uint8_t * address = reinterpret_cast<uint8_t *>(match);
    constexpr size_t patchSize = 12;
    uint8_t * trampoline = AllocateNear(address, 512);
    if(nullptr == trampoline) {
        advancedfx::Warning("[mirv_pov_radar] Could not allocate enemy color trampoline.\n");
        return false;
    }

    size_t pos = 0;
    EmitSaveContext(trampoline, pos, 0x88); // Mid-function RSP is 16-byte aligned.
    EmitU8(trampoline, pos, 0x48);
    EmitU8(trampoline, pos, 0x89);
    EmitU8(trampoline, pos, 0xD9); // mov rcx, rbx
    EmitAbsoluteCall(trampoline, pos, reinterpret_cast<const void *>(&AdjustRadarPlayerStyle));
    EmitRestoreContext(trampoline, pos, 0x88);
    EmitU8(trampoline, pos, 0x48);
    EmitU8(trampoline, pos, 0x89);
    EmitU8(trampoline, pos, 0x44);
    EmitU8(trampoline, pos, 0x24);
    EmitU8(trampoline, pos, 0x58); // mov [saved rbx], rax
    EmitBytes(trampoline, pos, kPopRegisters, sizeof(kPopRegisters));

    // Both overwritten instructions are position-independent. Copying them also
    // preserves the current stack and radar-entry displacements from the signature.
    EmitBytes(trampoline, pos, address, patchSize);
    bool emitted = EmitRel32Jump(trampoline, pos, address + patchSize);
    if(!emitted || !ApplyPatch(g_RadarEnemyColorPatch, address, patchSize, trampoline)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    return true;
}

} // namespace

void MirvPov_ApplyRadarPatches(HMODULE clientDll)
{
    if(nullptr == clientDll) {
        advancedfx::Warning("[mirv_pov_radar] client.dll is not loaded.\n");
        return;
    }

    PatchCompetitiveColorPath(clientDll);
    PatchCompetitiveTeamColor(
        clientDll,
        g_RadarTCompetitiveColorPatch,
        "E8 ?? ?? ?? ?? 41 3B C5 0F 85 ?? ?? ?? ?? F6 86",
        2);
    PatchCompetitiveTeamColor(
        clientDll,
        g_RadarCtCompetitiveColorPatch,
        "E8 ?? ?? ?? ?? 83 F8 03 75 ?? 8B D3",
        3);
    PatchTeammateVisibility(clientDll);
    PatchEnemyColor(clientDll);
}

void MirvPov_RemoveRadarPatches()
{
    RestorePatch(g_RadarEnemyColorPatch);
    RestorePatch(g_RadarTeammatePatch);
    RestorePatch(g_RadarCtCompetitiveColorPatch);
    RestorePatch(g_RadarTCompetitiveColorPatch);
    RestorePatch(g_RadarCompetitiveColorPathPatch);
}
