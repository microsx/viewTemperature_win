// hardware_db.h — viewTemp hardware temperature threshold database.
//
// This is a deliberately SMALL database (~14 entries). It is NOT a complete
// hardware catalog — viewTemp is not LibreHardwareMonitor. When the model
// string doesn't match, we fall back to the default threshold (85 °C).
//
// Each entry's `tjunction` is the manufacturer-published maximum junction
// temperature (Tjunction Max / Maximum GPU Temperature). The `warn` value
// is tjunction - 10 °C — a conservative "safe operating" floor that gives
// the user ~10 °C of warning headroom before the chip starts throttling.
//
// Sources are spelled out per entry. If you add a new entry, cite the
// source in the comment so future-you (or future-me) can verify.
//
// Matching strategy: case-insensitive substring (e.g. "RTX 4090" matches
// "NVIDIA GeForce RTX 4090"). First match wins, so put more-specific
// patterns BEFORE generic ones.

#pragma once

#include <windows.h>

struct HardwareEntry {
    const TCHAR* pattern;     // substring to match against g_cpuName / g_gpuName
    int           tjunction;  // manufacturer-published max junction (°C)
    int           warn;       // flash threshold = tjunction - 10 °C
    const TCHAR*  source;     // data source (URL or attribution)
};

static const HardwareEntry kCpuDb[] = {
    // Intel 13th/14th gen K-series: 100 °C TJunction (Intel ARK official)
    { TEXT("i9-14900K"), 100, 90, TEXT("Intel ARK 236773") },
    { TEXT("i9-13900K"), 100, 90, TEXT("Intel ARK") },
    { TEXT("i7-14700K"), 100, 90, TEXT("Intel ARK") },
    { TEXT("i7-13700K"), 100, 90, TEXT("Intel ARK 230500") },
    { TEXT("i5-13600K"), 100, 90, TEXT("Intel ARK") },
    { TEXT("i5-12600K"), 100, 90, TEXT("Intel ARK") },
    // Intel 13th/14th gen HX/H-series (笔记本): 100 °C Tjunction (Intel ARK)
    // HX is the laptop replacement for K; same Tjunction. Substring matching
    // is safe here: "i9-13900K" is NOT a substring of "i9-13900HX" because
    // K != HX, so the two patterns can't accidentally cross-match.
    { TEXT("i9-13900HX"), 100, 90, TEXT("Intel ARK + NotebookCheck") },
    { TEXT("i9-14900HX"), 100, 90, TEXT("Intel ARK") },
    { TEXT("i7-13700HX"), 100, 90, TEXT("Intel ARK") },
    { TEXT("i7-14700HX"), 100, 90, TEXT("Intel ARK") },
    { TEXT("i9-13900H"),  100, 90, TEXT("Intel ARK") },
    { TEXT("i7-13700H"),  100, 90, TEXT("Intel ARK") },
    // AMD Ryzen 7000 series: 95 °C thermal ceiling (AMD public, PCMag reporting)
    { TEXT("Ryzen 9 7950X"),   95, 85, TEXT("AMD public; PCMag 2023") },
    { TEXT("Ryzen 9 7900X"),   95, 85, TEXT("AMD public; PCMag 2023") },
    { TEXT("Ryzen 7 7800X3D"), 95, 85, TEXT("AMD public; PCMag 2023") },
    { TEXT("Ryzen 7 7700X"),   95, 85, TEXT("AMD public; PCMag 2023") },
    { TEXT("Ryzen 5 7600X"),   95, 85, TEXT("AMD public; PCMag 2023") },
    // AMD Ryzen 7045HX/7040HS 笔记本系列: 95 °C (AMD public)
    { TEXT("Ryzen 9 7945HX"),  95, 85, TEXT("AMD public + NotebookCheck") },
    { TEXT("Ryzen 9 7845HX"),  95, 85, TEXT("AMD public + NotebookCheck") },
};

static const HardwareEntry kGpuDb[] = {
    // NVIDIA GeForce RTX 40 series: 90 °C "Maximum GPU Temperature" (NVIDIA official)
    { TEXT("RTX 4090"),     90, 80, TEXT("NVIDIA product page") },
    { TEXT("RTX 4080"),     90, 80, TEXT("NVIDIA product page") },
    { TEXT("RTX 4070 Ti"),  90, 80, TEXT("NVIDIA product page") },
    { TEXT("RTX 4070"),     90, 80, TEXT("NVIDIA product page") },
    { TEXT("RTX 4060 Ti"),  90, 80, TEXT("NVIDIA product page") },
    { TEXT("RTX 4060"),     90, 80, TEXT("NVIDIA product page") },
    // AMD Radeon RX 7000 series: 110 °C hotspot (AMD official confirmation;
    // note this is HOTSPOT not edge — MAHM reports edge temp which is ~25-35°C
    // lower. warn=95 is conservative for edge-temp reports.)
    { TEXT("RX 7900 XTX"),  110, 95, TEXT("AMD official + hardware guides") },
    { TEXT("RX 7900 XT"),   110, 95, TEXT("AMD official + hardware guides") },
    { TEXT("RX 7800 XT"),   110, 95, TEXT("AMD official + hardware guides") },
    { TEXT("RX 7700 XT"),   105, 90, TEXT("AMD official + hardware guides") },
    { TEXT("RX 7600"),      105, 90, TEXT("AMD official + hardware guides") },
};

// Lookup helper. Iterates the given table; returns TRUE and writes warn into
// *outWarn if any pattern is a case-insensitive substring of `name`.
// Returns FALSE if no match.
static BOOL LookupHardwareThreshold(const HardwareEntry* table, size_t n,
                                    const TCHAR* name, int* outWarn)
{
    if (!name || !*name) return FALSE;
    // Lowercase a copy of the name once; CharLower mutates in place so we
    // can't pass the caller's buffer directly.
    TCHAR haystack[256];
    _tcscpy_s(haystack, _countof(haystack), name);
    CharLower(haystack);
    for (size_t i = 0; i < n; ++i) {
        TCHAR needle[64];
        _tcscpy_s(needle, _countof(needle), table[i].pattern);
        CharLower(needle);
        if (_tcsstr(haystack, needle)) {
            *outWarn = table[i].warn;
            return TRUE;
        }
    }
    return FALSE;
}