// viewTemp: MAHM (MSI Afterburner Hardware Monitoring) shared memory.
// Source: MSI Afterburner SDK 2.0 (public protocol).
// Afterburner ships this header at:
//   C:\Program Files (x86)\MSI Afterburner\SDK\Include\MAHMSharedMemory.h
#ifndef VIEWTEMP_MAHM_SHARED_MEMORY_H
#define VIEWTEMP_MAHM_SHARED_MEMORY_H

#include <windows.h>

#define MAHM_SIGNATURE       0x4D41484D  // 'MAHM' little-endian
#define MAHM_SIGNATURE_DEAD  0xDEADDEAD
#define MAHM_GPU_INDEX_GLOBAL 0xFFFFFFFFu

// Monitoring source IDs (from SDK 2.0).
enum {
    MAHM_SRC_GPU_TEMPERATURE      = 0x00000000,
    MAHM_SRC_PCB_TEMPERATURE      = 0x00000001,
    MAHM_SRC_MEM_TEMPERATURE      = 0x00000002,
    MAHM_SRC_VRM_TEMPERATURE      = 0x00000003,
    MAHM_SRC_CORE_CLOCK           = 0x00000020,
    MAHM_SRC_MEMORY_CLOCK         = 0x00000022,
    MAHM_SRC_GPU_USAGE            = 0x00000030,
    MAHM_SRC_MEMORY_USAGE         = 0x00000031, // % of dedicated VRAM used
    MAHM_SRC_FB_USAGE             = 0x00000032,
    MAHM_SRC_CPU_TEMPERATURE      = 0x00000080,
    MAHM_SRC_CPU_USAGE            = 0x00000090,
    MAHM_SRC_RAM_USAGE            = 0x00000091,
    MAHM_SRC_CPU_CLOCK            = 0x000000A0,
};

#pragma pack(push, 1)
typedef struct MAHM_SHARED_MEMORY_HEADER {
    DWORD dwSignature;
    DWORD dwVersion;
    DWORD dwHeaderSize;
    DWORD dwNumEntries;
    DWORD dwEntrySize;
    time_t time;
    DWORD dwNumGpuEntries;
    DWORD dwGpuEntrySize;
} MAHM_SHARED_MEMORY_HEADER;

typedef struct MAHM_SHARED_MEMORY_ENTRY {
    char  szSrcName[MAX_PATH];
    char  szSrcUnits[MAX_PATH];
    char  szLocalizedSrcName[MAX_PATH];
    char  szLocalizedSrcUnits[MAX_PATH];
    char  szRecommendedFormat[MAX_PATH];
    float data;
    float minLimit;
    float maxLimit;
    DWORD dwFlags;
    DWORD dwGpu;     // GPU index or 0xFFFFFFFF for global/CPU
    DWORD dwSrcId;
} MAHM_SHARED_MEMORY_ENTRY;

typedef struct MAHM_SHARED_MEMORY_GPU_ENTRY {
    char szGpuId[MAX_PATH];
    char szFamily[MAX_PATH];
    char szDevice[MAX_PATH];
    char szDriver[MAX_PATH];
    char szBIOS[MAX_PATH];
    DWORD dwMemAmount; // VRAM total in KB
} MAHM_SHARED_MEMORY_GPU_ENTRY;
#pragma pack(pop)

#endif // VIEWTEMP_MAHM_SHARED_MEMORY_H
