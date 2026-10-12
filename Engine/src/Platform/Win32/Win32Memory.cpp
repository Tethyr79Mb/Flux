#include "Flux/Platform/Memory.hpp"

#include <windows.h>
#include <psapi.h>

namespace Flux::Platform {
    ProcessMemory GetProcessMemory() {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        // K32… : dans kernel32 depuis Windows 7, sans lier psapi.lib.
        if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
            return {};
        }
        return {.privateBytes = counters.PrivateUsage, .workingSet = counters.WorkingSetSize};
    }
}
