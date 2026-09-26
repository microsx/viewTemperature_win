// viewTemp: NVAPI stub. The original code tried to use NVAPI via flat C
// exports (NvAPI_Initialize, etc.), but NVIDIA's modern driver ships
// nvapi64.dll as a COM-style library with only two exports
// (nvapi_QueryInterface, nvapi_Direct_GetMethod). None of the symbols we
// need exist. Switching to the QueryInterface path is a major rewrite
// (requires NVAPI SDK IIDs and vtable layout, neither of which is fully
// public); not worth it for a 4-line VRAM fallback. So we keep the
// declarations as no-op stubs — anything that called NvApi_Init/etc. now
// gets FLT_MAX or 0 back, and the call sites in viewTemp.cpp handle that
// by skipping NVAPI and falling through to MAHM.
//
// See continue.md "坑 2" for the full story.
#pragma once

#include <windows.h>
#include <cfloat>

// Always unavailable; keep callers compiling but always returning "no data".
static BOOL   NvApi_Init() { return FALSE; }
static void   NvApi_Shutdown() {}
static float  NvApi_GetGpuTemp(int /*gpuIndex*/) { return FLT_MAX; }
static DWORD  NvApi_GetVramTotalMb(int /*gpuIndex*/) { return 0; }
static DWORD  NvApi_GetVramUsedMb(int /*gpuIndex*/) { return 0; }

// Stub of the global state struct so viewTemp.cpp's diagnostic logging
// and log lines like `(int)g_nvapi.ok` keep compiling. ok is hard-wired
// FALSE so any remaining call to NvApi_Init() short-circuits. log lines
// will show `nvapi.ok=0` (truthful: NVAPI is permanently unavailable).
struct NvApi {
    BOOL ok;          // always FALSE
};
static struct NvApi g_nvapi = { FALSE };