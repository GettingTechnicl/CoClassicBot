#include "mem_stats.h"
#include <windows.h>
#include <psapi.h>
#include <spdlog/spdlog.h>

void MaybeLogMemoryStats()
{
    static DWORD s_lastLogTick = 0;
    constexpr DWORD kLogIntervalMs = 30000; // 30s — frequent enough to correlate
                                             // against other log activity, cheap
                                             // enough to leave running always.
    const DWORD now = GetTickCount();
    if (s_lastLogTick != 0 && now - s_lastLogTick < kLogIntervalMs)
        return;
    s_lastLogTick = now;

    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    // K32GetProcessMemoryInfo is available directly from kernel32.dll
    // (Vista+) — no psapi.lib link dependency needed.
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        spdlog::info("[memstats] WorkingSet={}KB Private={}KB Pagefile={}KB PeakWorkingSet={}KB",
            pmc.WorkingSetSize / 1024, pmc.PrivateUsage / 1024,
            pmc.PagefileUsage / 1024, pmc.PeakWorkingSetSize / 1024);
    }
}
