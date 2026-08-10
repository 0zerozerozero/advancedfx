#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace MirvPovHookUtils {

inline bool CalcRel32(
    const uint8_t * fromNext,
    const uint8_t * target,
    int32_t & result)
{
    const intptr_t relative = reinterpret_cast<intptr_t>(target)
        - reinterpret_cast<intptr_t>(fromNext);
    if(relative < INT32_MIN || INT32_MAX < relative) return false;
    result = static_cast<int32_t>(relative);
    return true;
}

inline uint8_t * AllocateNear(uint8_t * target, size_t size)
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
            const uintptr_t address =
                (targetAddress - offset) & ~(granularity - 1);
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

        const uintptr_t address =
            (targetAddress + offset + granularity - 1)
            & ~(granularity - 1);
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

} // namespace MirvPovHookUtils
