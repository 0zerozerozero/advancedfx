#include "stdafx.h"

#include "MirvPovTeamID.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "MirvPovRadar.h"
#include "WrpConsole.h"

#include "../shared/AfxConsole.h"
#include "../shared/AfxDetours.h"
#include "../shared/binutils.h"

#include <Windows.h>
#include <stdint.h>
static bool g_bMirvPovTeamIDHideEnemies = true;
static bool g_bMirvPovTeamIDDebug = false;
static int g_MirvPovTeamIDDebugCalls = 0;
static int g_MirvPovTeamIDDebugHidden = 0;
static int g_MirvPovTeamIDDebugNoObserved = 0;
static int g_MirvPovTeamIDDebugShowAll = 0;
static int g_MirvPovTeamIDDebugExceptions = 0;
static int g_MirvPovTeamIDDebugSameTeam = 0;
static int g_MirvPovTeamIDDebugDifferentTeam = 0;
static int g_MirvPovTeamIDDebugLastTargetTeam = -1;
static int g_MirvPovTeamIDDebugLastObservedTeam = -1;
static int g_MirvPovTeamIDDebugNextPrintAt = 1;

static void MirvPovTeamID_PrintStatusThrottled();

static bool MirvPovTeamID_CalcRel32(uint8_t* fromNext, uint8_t* target, int32_t& out)
{
	intptr_t rel = target - fromNext;
	if(rel < INT32_MIN || rel > INT32_MAX) return false;
	out = (int32_t)rel;
	return true;
}

static void MirvPovTeamID_EmitU8(uint8_t* code, size_t& pos, uint8_t value)
{
	code[pos++] = value;
}

static void MirvPovTeamID_EmitU32(uint8_t* code, size_t& pos, uint32_t value)
{
	memcpy(code + pos, &value, sizeof(value));
	pos += sizeof(value);
}

static void MirvPovTeamID_EmitU64(uint8_t* code, size_t& pos, uint64_t value)
{
	memcpy(code + pos, &value, sizeof(value));
	pos += sizeof(value);
}

static void MirvPovTeamID_EmitBytes(uint8_t* code, size_t& pos, const uint8_t* bytes, size_t size)
{
	memcpy(code + pos, bytes, size);
	pos += size;
}

static bool MirvPovTeamID_EmitRel32Jump(uint8_t* code, size_t& pos, uint8_t* target)
{
	int32_t rel32 = 0;
	if(!MirvPovTeamID_CalcRel32(code + pos + 5, target, rel32)) return false;
	MirvPovTeamID_EmitU8(code, pos, 0xE9);
	MirvPovTeamID_EmitU32(code, pos, (uint32_t)rel32);
	return true;
}

static bool MirvPovTeamID_EmitRel32Jcc(uint8_t* code, size_t& pos, uint8_t conditionOpcode, uint8_t* target)
{
	int32_t rel32 = 0;
	if(!MirvPovTeamID_CalcRel32(code + pos + 6, target, rel32)) return false;
	MirvPovTeamID_EmitU8(code, pos, 0x0F);
	MirvPovTeamID_EmitU8(code, pos, conditionOpcode);
	MirvPovTeamID_EmitU32(code, pos, (uint32_t)rel32);
	return true;
}

static void MirvPovTeamID_EmitMovupsRsp(uint8_t* code, size_t& pos, bool load, uint8_t xmmReg, uint8_t stackOffset)
{
	MirvPovTeamID_EmitU8(code, pos, 0x0F);
	MirvPovTeamID_EmitU8(code, pos, load ? 0x10 : 0x11);
	MirvPovTeamID_EmitU8(code, pos, (uint8_t)(0x44 + (xmmReg << 3)));
	MirvPovTeamID_EmitU8(code, pos, 0x24);
	MirvPovTeamID_EmitU8(code, pos, stackOffset);
}

static uint8_t* MirvPovTeamID_AllocNear(uint8_t* target, size_t size)
{
	SYSTEM_INFO systemInfo;
	GetSystemInfo(&systemInfo);
	size_t granularity = systemInfo.dwAllocationGranularity;
	uintptr_t targetAddr = (uintptr_t)target;
	uintptr_t minAddr = targetAddr > 0x7fff0000 ? targetAddr - 0x7fff0000 : 0;
	uintptr_t maxAddr = targetAddr + 0x7fff0000;

	for(uintptr_t offset = 0; offset < 0x7fff0000; offset += granularity) {
		if(targetAddr >= offset + granularity) {
			uintptr_t addr = (targetAddr - offset) & ~(granularity - 1);
			if(addr >= minAddr) {
				if(void* result = VirtualAlloc((void*)addr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return (uint8_t*)result;
			}
		}

		uintptr_t addr = (targetAddr + offset + granularity - 1) & ~(granularity - 1);
		if(addr <= maxAddr) {
			if(void* result = VirtualAlloc((void*)addr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return (uint8_t*)result;
		}
	}

	return nullptr;
}

static bool MirvPovTeamID_TryGetObservedTeam(int& observedTeam)
{
	auto observedController = GetFakePovRadarController();
	if(!observedController || !observedController->IsPlayerController()) return false;

	observedTeam = observedController->GetTeam();
	return observedTeam == 2 || observedTeam == 3;
}

extern "C" bool __fastcall MirvPovTeamID_ShouldHide(CEntityInstance* playerPawn, bool showAllTeamIDs)
{
	if(g_bMirvPovTeamIDDebug) ++g_MirvPovTeamIDDebugCalls;
	if(showAllTeamIDs) {
		if(g_bMirvPovTeamIDDebug) ++g_MirvPovTeamIDDebugShowAll;
		return false;
	}
	if(!MirvPov_IsEnabled() || !g_bMirvPovTeamIDHideEnemies || !playerPawn) return false;

	__try {
		if(!playerPawn->IsPlayerPawn()) return false;

		int observedTeam = -1;
		if(!MirvPovTeamID_TryGetObservedTeam(observedTeam)) {
			if(g_bMirvPovTeamIDDebug) ++g_MirvPovTeamIDDebugNoObserved;
			return false;
		}

		int targetTeam = playerPawn->GetTeam();
		if(targetTeam != 2 && targetTeam != 3) return false;
		if(g_bMirvPovTeamIDDebug) {
			g_MirvPovTeamIDDebugLastTargetTeam = targetTeam;
			g_MirvPovTeamIDDebugLastObservedTeam = observedTeam;
			if(targetTeam == observedTeam) ++g_MirvPovTeamIDDebugSameTeam;
			else ++g_MirvPovTeamIDDebugDifferentTeam;
		}

		bool result = targetTeam != observedTeam;
		if(result && g_bMirvPovTeamIDDebug) ++g_MirvPovTeamIDDebugHidden;
		if(g_bMirvPovTeamIDDebug) MirvPovTeamID_PrintStatusThrottled();
		return result;
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		if(g_bMirvPovTeamIDDebug) ++g_MirvPovTeamIDDebugExceptions;
		return false;
	}
}

void MirvPovTeamID_EnableDebug()
{
	g_bMirvPovTeamIDDebug = true;
}

void MirvPovTeamID_ResetDebug()
{
	g_MirvPovTeamIDDebugCalls = 0;
	g_MirvPovTeamIDDebugHidden = 0;
	g_MirvPovTeamIDDebugNoObserved = 0;
	g_MirvPovTeamIDDebugShowAll = 0;
	g_MirvPovTeamIDDebugExceptions = 0;
	g_MirvPovTeamIDDebugSameTeam = 0;
	g_MirvPovTeamIDDebugDifferentTeam = 0;
	g_MirvPovTeamIDDebugLastTargetTeam = -1;
	g_MirvPovTeamIDDebugLastObservedTeam = -1;
	g_MirvPovTeamIDDebugNextPrintAt = 1;
}

void MirvPovTeamID_PrintStatus()
{
	advancedfx::Message(
		"mirv_teamid debug=%i calls=%i hidden=%i noObserved=%i showAll=%i exceptions=%i sameTeam=%i differentTeam=%i lastTargetTeam=%i lastObservedTeam=%i\n"
		, g_bMirvPovTeamIDDebug ? 1 : 0
		, g_MirvPovTeamIDDebugCalls
		, g_MirvPovTeamIDDebugHidden
		, g_MirvPovTeamIDDebugNoObserved
		, g_MirvPovTeamIDDebugShowAll
		, g_MirvPovTeamIDDebugExceptions
		, g_MirvPovTeamIDDebugSameTeam
		, g_MirvPovTeamIDDebugDifferentTeam
		, g_MirvPovTeamIDDebugLastTargetTeam
		, g_MirvPovTeamIDDebugLastObservedTeam
	);
}

static void MirvPovTeamID_PrintStatusThrottled()
{
	int total = g_MirvPovTeamIDDebugCalls;
	if(total < g_MirvPovTeamIDDebugNextPrintAt) return;
	MirvPovTeamID_PrintStatus();
	if(g_MirvPovTeamIDDebugNextPrintAt < 1024) g_MirvPovTeamIDDebugNextPrintAt *= 2;
	else g_MirvPovTeamIDDebugNextPrintAt += 1024;
}

void MirvPovTeamID_ApplyPatches(HMODULE clientDll)
{
	static bool bPatched = false;
	if(bPatched) return;
	g_bMirvPovTeamIDHideEnemies = true;
	if(!clientDll) {
		advancedfx::Warning("[mirv_pov_teamid] client.dll is not loaded.\n");
		return;
	}

	Afx::BinUtils::MemRange textRange = Afx::BinUtils::MemRange::FromEmpty();
	{
		Afx::BinUtils::ImageSectionsReader sections((HMODULE)clientDll);
		if(!sections.Eof()) textRange = sections.GetMemRange();
	}

	auto cmpForHide = Afx::BinUtils::FindPatternString(textRange, "0F 5B FF 0F 2F FE 0F 82 ?? ?? ?? 00 F3 0F 10 44 24");
	auto nextPlayer = Afx::BinUtils::FindPatternString(textRange, "41 BC FF FF 00 00 48 8B ?? ?? 33 DB 48 8B FB 66 44 ?? ?? ?? ?? 0F 84 ?? ?? ?? 00");
	if(cmpForHide.IsEmpty() || nextPlayer.IsEmpty()) {
		ErrorBox(MkErrStr(__FILE__, __LINE__));
		return;
	}

	auto patchAt = (uint8_t*)cmpForHide.Start;
	auto nextPlayerAt = (uint8_t*)nextPlayer.Start;
	size_t patchSize = 18;
	auto cave = MirvPovTeamID_AllocNear(patchAt, 256);
	if(!cave) {
		advancedfx::Warning("[mirv_pov_teamid] Failed to allocate near code cave.\n");
		return;
	}

	int32_t rel32 = 0;
	uint8_t* originalJumpTarget = patchAt + 12 + *(int32_t*)(patchAt + 8);
	size_t pos = 0;
	const uint8_t pushRegs[] = {
		0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57,
		0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
		0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
	};
	const uint8_t popRegs[] = {
		0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C,
		0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58,
		0x5F, 0x5E, 0x5D, 0x5B, 0x5A, 0x59, 0x58
	};

	MirvPovTeamID_EmitBytes(cave, pos, pushRegs, sizeof(pushRegs));
	MirvPovTeamID_EmitU8(cave, pos, 0x48); MirvPovTeamID_EmitU8(cave, pos, 0x81); MirvPovTeamID_EmitU8(cave, pos, 0xEC); MirvPovTeamID_EmitU32(cave, pos, 0x88);
	for(uint8_t i = 0; i < 6; ++i) MirvPovTeamID_EmitMovupsRsp(cave, pos, false, i, (uint8_t)(0x20 + i * 0x10));
	MirvPovTeamID_EmitU8(cave, pos, 0x48); MirvPovTeamID_EmitU8(cave, pos, 0x89); MirvPovTeamID_EmitU8(cave, pos, 0xF9);
	MirvPovTeamID_EmitU8(cave, pos, 0x4C); MirvPovTeamID_EmitU8(cave, pos, 0x89); MirvPovTeamID_EmitU8(cave, pos, 0xEA);
	MirvPovTeamID_EmitU8(cave, pos, 0x48); MirvPovTeamID_EmitU8(cave, pos, 0xB8); MirvPovTeamID_EmitU64(cave, pos, (uint64_t)&MirvPovTeamID_ShouldHide);
	MirvPovTeamID_EmitU8(cave, pos, 0xFF); MirvPovTeamID_EmitU8(cave, pos, 0xD0);
	for(uint8_t i = 0; i < 6; ++i) MirvPovTeamID_EmitMovupsRsp(cave, pos, true, i, (uint8_t)(0x20 + i * 0x10));
	MirvPovTeamID_EmitU8(cave, pos, 0x48); MirvPovTeamID_EmitU8(cave, pos, 0x81); MirvPovTeamID_EmitU8(cave, pos, 0xC4); MirvPovTeamID_EmitU32(cave, pos, 0x88);
	MirvPovTeamID_EmitU8(cave, pos, 0x84); MirvPovTeamID_EmitU8(cave, pos, 0xC0);
	MirvPovTeamID_EmitBytes(cave, pos, popRegs, sizeof(popRegs));
	if(!MirvPovTeamID_EmitRel32Jcc(cave, pos, 0x85, nextPlayerAt)) {
		advancedfx::Warning("[mirv_pov_teamid] Hide-enemies jump is out of range.\n");
		VirtualFree(cave, 0, MEM_RELEASE);
		return;
	}
	MirvPovTeamID_EmitU8(cave, pos, 0x0F); MirvPovTeamID_EmitU8(cave, pos, 0x5B); MirvPovTeamID_EmitU8(cave, pos, 0xFF);
	MirvPovTeamID_EmitU8(cave, pos, 0x0F); MirvPovTeamID_EmitU8(cave, pos, 0x2F); MirvPovTeamID_EmitU8(cave, pos, 0xFE);
	if(!MirvPovTeamID_EmitRel32Jcc(cave, pos, 0x82, originalJumpTarget)) {
		advancedfx::Warning("[mirv_pov_teamid] Original jump is out of range.\n");
		VirtualFree(cave, 0, MEM_RELEASE);
		return;
	}
	MirvPovTeamID_EmitU8(cave, pos, 0xF3); MirvPovTeamID_EmitU8(cave, pos, 0x0F); MirvPovTeamID_EmitU8(cave, pos, 0x10); MirvPovTeamID_EmitU8(cave, pos, 0x44); MirvPovTeamID_EmitU8(cave, pos, 0x24); MirvPovTeamID_EmitU8(cave, pos, *(patchAt + 17));
	if(!MirvPovTeamID_EmitRel32Jump(cave, pos, patchAt + patchSize)) {
		advancedfx::Warning("[mirv_pov_teamid] Return jump is out of range.\n");
		VirtualFree(cave, 0, MEM_RELEASE);
		return;
	}
	FlushInstructionCache(GetCurrentProcess(), cave, pos);

	unsigned char patchCode[18] = { 0xE9, 0,0,0,0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
	if(!MirvPovTeamID_CalcRel32(patchAt + 5, cave, rel32)) {
		advancedfx::Warning("[mirv_pov_teamid] Cave jump is out of range.\n");
		VirtualFree(cave, 0, MEM_RELEASE);
		return;
	}
	*(int32_t*)(patchCode + 1) = rel32;

	MdtMemBlockInfos mbis;
	MdtMemAccessBegin((LPVOID)patchAt, sizeof(patchCode), &mbis);
	memcpy((LPVOID)patchAt, patchCode, sizeof(patchCode));
	FlushInstructionCache(GetCurrentProcess(), (LPCVOID)patchAt, sizeof(patchCode));
	MdtMemAccessEnd(&mbis);
	bPatched = true;

	advancedfx::Message("[mirv_pov_teamid] Patched TeamID baseline enemy filter.\n");
}

CON_COMMAND(mirv_teamid, "Manage team id overhead drawing.")
{
	auto argC = args->ArgC();
	auto arg0 = args->ArgV(0);

	if(3 <= argC && 0 == _stricmp("hideEnemies", args->ArgV(1))) {
		g_bMirvPovTeamIDHideEnemies = 0 != atoi(args->ArgV(2));
		return;
	}

	if(3 <= argC && 0 == _stricmp("debug", args->ArgV(1))) {
		g_bMirvPovTeamIDDebug = 0 != atoi(args->ArgV(2));
		return;
	}

	if(2 <= argC && 0 == _stricmp("status", args->ArgV(1))) {
		MirvPovTeamID_PrintStatus();
		return;
	}

	if(2 <= argC && 0 == _stricmp("resetDebug", args->ArgV(1))) {
		MirvPovTeamID_ResetDebug();
		return;
	}

	advancedfx::Message(
		"%s hideEnemies 0|1 - Hide enemy team id overhead while keeping teammate team id visible.\n"
		"%s debug 0|1 - Enable debug counters.\n"
		"%s status - Print debug counters.\n"
		"%s resetDebug - Reset debug counters.\n"
		"Current value: %i\n"
		, arg0
		, arg0
		, arg0
		, arg0
		, g_bMirvPovTeamIDHideEnemies ? 1 : 0
	);
}
