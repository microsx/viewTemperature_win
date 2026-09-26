// MAHM_shim.h
//
// Local reimplementation of the relevant subset of MSI Afterburner Hardware
// Monitoring (MAHM) shared memory format (RivaTuner Statistics Server SDK).
//
// The SDK header (MAHMSharedMemory.h) declares MAHM_SHARED_MEMORY_HEADER with
// a `time` field whose size depends on `_WIN64` and `time_t`:
//
//     #ifdef _WIN64
//         __time32_t time;     // 4 bytes when _USE_32BIT_TIME_T is set
//     #else
//         time_t time;          // 4 bytes on x86
//     #endif
//
// On x64 (our build) `_USE_32BIT_TIME_T` cannot be combined with `_WIN64`
// (ucrt's <corecrt.h> enforces this with `#error`). Without the macro, `time_t`
// is 8 bytes, and MAHM_SHARED_MEMORY_HEADER ends up 4 bytes larger than the
// on-the-wire layout that RivaTuner Statistics Server writes. Reading through
// the SDK struct then yields garbage in every subsequent field:
//   - dwNumEntries   reads the value of dwNumGpuEntries (1304 here)
//   - dwEntrySize    reads a misaligned field (542462019 here)
//   - GetSource iterates 1304 entries with a 542 MB stride and finds nothing
//   - Every value prints as "--"
//
// The fix is structural: we declare our own copy of the header layout with the
// time field hard-wired to 4 bytes, matching the v2.0 on-the-wire format. This
// header only defines the subset of fields the project actually reads.

#ifndef _MAHM_SHIM_INCLUDED_
#define _MAHM_SHIM_INCLUDED_

#include <windows.h>

// Source IDs (subset of MSIAB SDK)
#define MAHM_SRC_GPU_TEMPERATURE      0x00000000u
#define MAHM_SRC_GPU_USAGE            0x00000030u
#define MAHM_SRC_MEMORY_USAGE         0x00000031u
#define MAHM_SRC_CPU_TEMPERATURE      0x00000080u
#define MAHM_SRC_CPU_USAGE            0x00000090u

#define MAHM_SIGNATURE                0x4D41484Du   // 'MAHM'
#define MAHM_SIGNATURE_DEAD           0x0000DEADu
#define MAHM_GPU_INDEX_GLOBAL         0xFFFFFFFFu

typedef struct MAHM_SHARED_MEMORY_HEADER
{
    DWORD dwSignature;
    DWORD dwVersion;
    DWORD dwHeaderSize;
    DWORD dwNumEntries;
    DWORD dwEntrySize;
    DWORD time;              // 4 bytes, fixed -- see header comment above
    DWORD dwNumGpuEntries;   // v2.0+ fields
    DWORD dwGpuEntrySize;
} MAHM_SHARED_MEMORY_HEADER, *LPMAHM_SHARED_MEMORY_HEADER;

typedef struct MAHM_SHARED_MEMORY_ENTRY
{
    char  szSrcName[MAX_PATH];
    char  szSrcUnits[MAX_PATH];
    char  szLocalizedSrcName[MAX_PATH];
    char  szLocalizedSrcUnits[MAX_PATH];
    char  szRecommendedFormat[MAX_PATH];
    float data;
    float minLimit;
    float maxLimit;
    DWORD dwFlags;
    DWORD dwGpu;             // GPU index or 0xFFFFFFFF for global sources
    DWORD dwSrcId;
} MAHM_SHARED_MEMORY_ENTRY, *LPMAHM_SHARED_MEMORY_ENTRY;

typedef struct MAHM_SHARED_MEMORY_GPU_ENTRY
{
    char  szGpuId[MAX_PATH];
    char  szFamily[MAX_PATH];
    char  szDevice[MAX_PATH];
    char  szDriver[MAX_PATH];
    char  szBIOS[MAX_PATH];
    DWORD dwMemAmount;       // on-board memory amount in KB
} MAHM_SHARED_MEMORY_GPU_ENTRY, *LPMAHM_SHARED_MEMORY_GPU_ENTRY;

#endif // _MAHM_SHIM_INCLUDED_