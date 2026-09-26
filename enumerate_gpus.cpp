// enumerate_gpus.cpp
//
// Standalone diagnostic tool: enumerates all GPU entries from MSI Afterburner
// Hardware Monitoring (MAHM) shared memory, prints their identifying fields,
// then probes each known source id per GPU to verify data is actually being
// written.
//
// NOT linked into viewTemp.exe. Compiled by enumerate_gpus.bat into
// enumerate_gpus.exe. Lives next to viewTemp.cpp / MAHM_shim.h.
//
// Output:
//   - stdout (console)
//   - enumerate_gpus.log (UTF-8 BOM, overwritten each run)
//
// Why this tool exists:
//   老林 enabled iGPU in mixed-mode; viewTemp (which only reads GPU entry 0)
//   shows no iGPU info. Before refactoring viewTemp.cpp to "GPU set" model, we
//   need ground truth: how many GPU entries does MAHM report, does the iGPU
//   entry actually carry temperature / usage / VRAM, and is dwMemAmount (the
//   on-board VRAM size in KB) populated for the iGPU too -- or is it 0 like
//   the dGPU path (see continue.md 坑 2)?
//
//   This program is the answer machine. After running it once, paste the log
//   back and we know exactly what the viewTemp.cpp refactor must handle.

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include "MAHM_shim.h"

#pragma comment(lib, "user32.lib")

// ---- Globals (mirroring viewTemp.cpp) --------------------------------------

static HANDLE g_hMap = NULL;
static HANDLE g_hMutex = NULL;
static LPMAHM_SHARED_MEMORY_HEADER g_header = NULL;
static LPMAHM_SHARED_MEMORY_ENTRY g_entries = NULL;
static LPMAHM_SHARED_MEMORY_GPU_ENTRY g_gpuEntries = NULL;
static DWORD g_numEntries = 0;
static DWORD g_entrySize = 0;
static DWORD g_numGpuEntries = 0;
static DWORD g_gpuEntrySize = 0;

// ---- Logging ---------------------------------------------------------------

static FILE* g_logFile = NULL;

static void LogOpen() {
    // UTF-8, no BOM. 老林's Notepad handles BOM correctly anyway, and tools
    // like type / Get-Content mis-handle a BOM as the first byte of the first
    // line.
    g_logFile = fopen("enumerate_gpus.log", "w");
}

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    if (g_logFile) vfprintf(g_logFile, fmt, ap);
    va_end(ap);
}

static void LogClose() {
    if (g_logFile) { fclose(g_logFile); g_logFile = NULL; }
}

// ---- MAHM plumbing (mirrored from viewTemp.cpp v0.14) ----------------------

static BOOL MapMahm() {
    Log("[MapMahm] opening Global\\MAHMSharedMemory ...\n");
    g_hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, "MAHMSharedMemory");
    if (!g_hMap) {
        Log("[MapMahm] FAILED: OpenFileMapping err=%lu\n", GetLastError());
        return FALSE;
    }
    g_header = (LPMAHM_SHARED_MEMORY_HEADER)MapViewOfFile(
        g_hMap, FILE_MAP_READ, 0, 0, 0);
    if (!g_header) {
        Log("[MapMahm] FAILED: MapViewOfFile err=%lu\n", GetLastError());
        CloseHandle(g_hMap);
        g_hMap = NULL;
        return FALSE;
    }

    // RivaTuner Statistics Server uses a mutex named "MAHMSharedMemory" too
    g_hMutex = OpenMutexA(SYNCHRONIZE, FALSE, "MAHMSharedMemoryMutex");
    if (!g_hMutex) {
        // Try alternate names seen in the wild (some RTSS builds use no mutex)
        Log("[MapMahm] MAHMSharedMemoryMutex open err=%lu (continuing)\n",
            GetLastError());
    }

    // Mirror viewTemp.cpp's "no mutex or not signalled" path: just read.
    // viewTemp has been shipping with this for a year; if RTSS isn't running,
    // the header sig check below will catch it.
    (void)g_hMutex;

    Log("[MapMahm] sig=0x%08lX version=%lu headerSize=%lu\n",
        g_header->dwSignature, g_header->dwVersion, g_header->dwHeaderSize);

    if (g_header->dwSignature == MAHM_SIGNATURE_DEAD) {
        Log("[MapMahm] FAILED: signature DEAD -- RTSS not running?\n");
        return FALSE;
    }
    if (g_header->dwSignature != MAHM_SIGNATURE) {
        Log("[MapMahm] FAILED: signature mismatch (got 0x%08lX, want 0x%08lX)\n",
            g_header->dwSignature, (unsigned long)MAHM_SIGNATURE);
        return FALSE;
    }

    g_numEntries = g_header->dwNumEntries;
    g_entrySize = g_header->dwEntrySize;
    g_numGpuEntries = g_header->dwNumGpuEntries;
    g_gpuEntrySize = g_header->dwGpuEntrySize;

    Log("[MapMahm] OK: numEntries=%lu entrySize=%lu numGpuEntries=%lu gpuEntrySize=%lu\n",
        g_numEntries, g_entrySize, g_numGpuEntries, g_gpuEntrySize);

    g_entries = (LPMAHM_SHARED_MEMORY_ENTRY)((BYTE*)g_header + g_header->dwHeaderSize);
    g_gpuEntries = (LPMAHM_SHARED_MEMORY_GPU_ENTRY)(
        (BYTE*)g_header + g_header->dwHeaderSize
        + (size_t)g_numEntries * g_entrySize);
    return TRUE;
}

static void UnmapMahm() {
    if (g_header) { UnmapViewOfFile(g_header); g_header = NULL; }
    if (g_hMap) { CloseHandle(g_hMap); g_hMap = NULL; }
    if (g_hMutex) { CloseHandle(g_hMutex); g_hMutex = NULL; }
    g_entries = NULL;
    g_gpuEntries = NULL;
}

// GetGpuEntry: bounds-checked accessor. viewTemp.cpp uses this exact pattern.
static LPMAHM_SHARED_MEMORY_GPU_ENTRY GetGpuEntry(DWORD index) {
    if (!g_gpuEntries) return NULL;
    if (index >= g_numGpuEntries) return NULL;
    return (LPMAHM_SHARED_MEMORY_GPU_ENTRY)(
        (BYTE*)g_gpuEntries + (size_t)index * g_gpuEntrySize);
}

// GetSource: linear search over entry list for (gpuIndex, srcId).
// viewTemp.cpp uses this. We print intermediate state for the probe report.
static BOOL GetSource(DWORD gpuIndex, DWORD srcId, LPVOID dest, DWORD destSize) {
    if (!g_entries || gpuIndex == MAHM_GPU_INDEX_GLOBAL) return FALSE;

    for (DWORD i = 0; i < g_numEntries; i++) {
        LPMAHM_SHARED_MEMORY_ENTRY e =
            (LPMAHM_SHARED_MEMORY_ENTRY)((BYTE*)g_entries + (size_t)i * g_entrySize);
        if (e->dwGpu == gpuIndex && e->dwSrcId == srcId) {
            if (destSize >= sizeof(float)) {
                memcpy(dest, &e->data, sizeof(float));
            }
            return TRUE;
        }
    }
    return FALSE;
}

// ---- Helpers ---------------------------------------------------------------

static const char* SrcIdName(DWORD srcId) {
    switch (srcId) {
        case MAHM_SRC_GPU_TEMPERATURE: return "GPU_TEMPERATURE";
        case MAHM_SRC_GPU_USAGE:       return "GPU_USAGE";
        case MAHM_SRC_MEMORY_USAGE:    return "MEMORY_USAGE";
        case MAHM_SRC_CPU_TEMPERATURE: return "CPU_TEMPERATURE";
        case MAHM_SRC_CPU_USAGE:       return "CPU_USAGE";
        default: return "?";
    }
}

// Truncated printable version of a possibly-binary szGpuId (which contains
// PCI device paths with backslashes, etc). We just stop at first NUL.
static void PrintField(const char* label, const char* field, size_t fieldMax) {
    // Find NUL-terminated length within fieldMax
    size_t n = 0;
    while (n < fieldMax && field[n] != 0) n++;
    Log("    %s = %.*s%s\n", label, (int)n, field,
        (n == fieldMax ? " (TRUNC)" : ""));
}

// ---- Main ------------------------------------------------------------------

int wmain(int argc, wchar_t** argv) {
    LogOpen();

    time_t now = time(NULL);
    Log("=== enumerate_gpus run at %s", ctime(&now));
    Log("=== purpose: discover MAHM GPU entries + verify iGPU data\n");
    Log("=== exe arch: %s, sizeof(time_t)=%zu, sizeof(DWORD)=%zu\n\n",
        sizeof(void*) == 8 ? "x64" : "x86",
        sizeof(time_t), sizeof(DWORD));

    if (!MapMahm()) {
        Log("\n[FATAL] MAHM unavailable. Is RivaTuner Statistics Server running?\n");
        LogClose();
        return 1;
    }

    Log("\n[Section 1] GPU entries reported by MAHM\n");
    Log("  dwNumGpuEntries = %lu (max index = %lu)\n",
        g_numGpuEntries, g_numGpuEntries ? g_numGpuEntries - 1 : 0);

    int activeCount = 0;
    for (DWORD i = 0; i < g_numGpuEntries; i++) {
        LPMAHM_SHARED_MEMORY_GPU_ENTRY e = GetGpuEntry(i);
        if (!e) { Log("  gpu[%lu] GetGpuEntry returned NULL\n", i); continue; }

        BOOL empty = (e->szGpuId[0] == 0 && e->szFamily[0] == 0
                      && e->szDevice[0] == 0);
        if (empty) {
            Log("  gpu[%lu] (empty / not populated by RTSS)\n", i);
            continue;
        }
        activeCount++;

        Log("  gpu[%lu] (active)\n", i);
        PrintField("szGpuId  ", e->szGpuId,   MAX_PATH);
        PrintField("szFamily ", e->szFamily,  MAX_PATH);
        PrintField("szDevice ", e->szDevice,  MAX_PATH);
        PrintField("szDriver ", e->szDriver,  MAX_PATH);
        PrintField("szBIOS   ", e->szBIOS,    MAX_PATH);
        Log("    dwMemAmount = %lu KB = %.3f GB  <-- total VRAM on this card\n",
            e->dwMemAmount, (double)e->dwMemAmount / (1024.0 * 1024.0));
    }
    Log("\n  active GPU count = %d\n", activeCount);

    Log("\n[Section 2] Per-GPU source probe\n");
    Log("  (raw float returned; units NOT applied here -- see continue.md 坑 2)\n\n");

    static const DWORD kProbeSrcs[] = {
        MAHM_SRC_GPU_TEMPERATURE,
        MAHM_SRC_GPU_USAGE,
        MAHM_SRC_MEMORY_USAGE,
    };

    for (DWORD i = 0; i < g_numGpuEntries; i++) {
        LPMAHM_SHARED_MEMORY_GPU_ENTRY e = GetGpuEntry(i);
        if (!e) continue;
        BOOL empty = (e->szGpuId[0] == 0 && e->szFamily[0] == 0
                      && e->szDevice[0] == 0);
        if (empty) continue;

        // Print friendly name (prefer szDevice, fallback szFamily)
        const char* friendly = e->szDevice[0] ? e->szDevice
                            : e->szFamily[0] ? e->szFamily
                            : e->szGpuId;

        Log("  gpu[%lu] %s\n", i, friendly);
        for (size_t s = 0; s < sizeof(kProbeSrcs)/sizeof(kProbeSrcs[0]); s++) {
            DWORD srcId = kProbeSrcs[s];
            float v = 0.0f;
            BOOL ok = GetSource(i, srcId, &v, sizeof(v));
            if (ok) {
                Log("    %-18s raw=%-14.4f   (interpreting: see continue.md 坑 2)\n",
                    SrcIdName(srcId), (double)v);
            } else {
                Log("    %-18s NOT FOUND in entry list\n", SrcIdName(srcId));
            }
        }
    }

    Log("\n[Section 3] CPU (for reference -- viewTemp reads CPU globally)\n");
    float cpuTemp = 0, cpuUsage = 0;
    BOOL hasCpuTemp = GetSource(MAHM_GPU_INDEX_GLOBAL, MAHM_SRC_CPU_TEMPERATURE,
                                &cpuTemp, sizeof(cpuTemp));
    BOOL hasCpuUsage = GetSource(MAHM_GPU_INDEX_GLOBAL, MAHM_SRC_CPU_USAGE,
                                 &cpuUsage, sizeof(cpuUsage));
    Log("  CPU_TEMPERATURE raw=%.4f (%s)\n", (double)cpuTemp,
        hasCpuTemp ? "ok" : "missing");
    Log("  CPU_USAGE       raw=%.4f (%s)\n", (double)cpuUsage,
        hasCpuUsage ? "ok" : "missing");

    Log("\n[Section 4] Interpretation hints (NOT data, just reminders)\n");
    Log("  - dwMemAmount=0  on a card  -> MAHM/RTSS doesn't fill total VRAM\n");
    Log("                          for that card. Need DXGI/NVAPI fallback.\n");
    Log("  - GPU_USAGE raw ~ [0..100]  -> looks like percent directly\n");
    Log("  - MEMORY_USAGE raw = percent * 100 (e.g. 6.3%% -> 6.30 * 100 = ~630)\n");
    Log("                          -> /100 for display (坑 2)\n");
    Log("  - GPU_TEMPERATURE raw = celsius directly\n");

    UnmapMahm();
    Log("\n=== done\n");
    LogClose();
    return 0;
}