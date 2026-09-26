// viewTemp.cpp
// Top-most floating overlay showing CPU/GPU temperature + GPU VRAM usage.
// Reads sensors via MSI Afterburner Hardware Monitoring (MAHM) shared memory.
//
// Build: see build.bat (cl.exe, /MT static link, Release | MinSize).

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// Helper: clamp int to [lo, hi]. Avoids std::min/max (which need <algorithm>)
// and the min/max macros from windows.h (suppressed by NOMINMAX).
static inline int ClampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <strsafe.h>
#include <objbase.h>
#include <gdiplus.h>
// INITGUID makes dxgi.h's DEFINE_GUID(IID_IDXGIFactory, ...) allocate the
// GUID storage in this TU. Without it the symbol stays extern-only and
// __uuidof() / CreateDXGIFactory() either link-fail or silently pass an
// all-zero GUID. Must come BEFORE <dxgi.h>.
#define INITGUID
#include <dxgi.h>
#include <tchar.h>
#include <cfloat>
#include <stdio.h>
#include <stdarg.h>
#include <io.h>
#include <fcntl.h>
#include <string.h>
#include <math.h>

// MAHM v2.0 runtime header layout assumes a 4-byte time_t field, but on
// x64 cl refuses _USE_32BIT_TIME_T and the SDK header uses 8-byte time_t,
// shifting all subsequent fields by 4 bytes (proven empirically: reading
// dwNumEntries yielded 1304, dwEntrySize yielded 542462019). To avoid the
// interlock we drop the SDK header entirely and use a local shim with a
// hard-coded 4-byte time field matching the on-the-wire layout.
#include "MAHM_shim.h"
#include "NvApi.h"
#include "hardware_db.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

// ---------------------------------------------------------------------------
// i18n — UI language picked once at startup from GetUserDefaultUILanguage.
// LANG_IF picks one of two literals at compile cost (the unused branch is
// dead-code-eliminated) so we don't pay runtime branch cost in the hot path.
// Strings that are user-visible (menus, tray tip) use LANG_IF; log/ini keys
// stay English.
// ---------------------------------------------------------------------------
#define LANG_IF(zh, en) ((g_isChinese) ? TEXT(zh) : TEXT(en))

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static const TCHAR* kWndClass       = TEXT("viewTempClass");
static const TCHAR* kMapName        = TEXT("MAHMSharedMemory");
static const UINT  kTrayMsg         = WM_USER + 1;
static const UINT  kTimerSample     = 1;
static const UINT  kTimerIdle       = 2;
static const UINT  kTimerHoverIn    = 3;
static const UINT  kTimerHoverPoll  = 4;
static const UINT  kTimerFlash      = 5;
static const UINT  kHoverDelayMs    = 600;
static const UINT  kHoverPollMs     = 100;
static const UINT  kFlashIntervalMs = 360;     // flash toggle cadence
static const UINT  kSampleIntervalMs = 1000;   // active
static const UINT  kIdleIntervalMs   = 2000;   // when not focused

struct Config {
    int   fontSize;
    BOOL  transparentBg;
    BYTE  bgAlpha;
    BYTE  fgAlpha;
    COLORREF fgColor;
    BOOL  topMost;
    int   intervalMs;
    BOOL  showCpu;
    BOOL  showGpu;
    BOOL  showUsage;
    BOOL  showVram;
    BOOL  showRam;
    int   opacityPct;
};

static Config g_cfg = {
    /*fontSize*/  20,
    /*transparentBg*/ TRUE,
    /*bgAlpha*/    220,
    /*fgAlpha*/    255,
    /*fgColor*/    RGB(255, 255, 0),
    /*topMost*/    TRUE,
    /*intervalMs*/ kSampleIntervalMs,
    /*showCpu*/    TRUE,
    /*showGpu*/    TRUE,
    /*showUsage*/  FALSE,
    /*showVram*/   TRUE,
    /*showRam*/    TRUE,
    /*opacityPct*/ 100,
};

static BOOL    g_trayAdded     = FALSE;
static NOTIFYICONDATA g_nid    = {};
static BOOL    g_isChinese     = TRUE;   // set at startup via GetUserDefaultUILanguage
static HANDLE  g_mapMahm       = NULL;
static LPVOID  g_pMahm         = NULL;
static DWORD   g_entryStride   = 0;
static DWORD   g_numEntries    = 0;
static DWORD   g_gpuEntryCount = 0;

// ---------------------------------------------------------------------------
// GPU set — dynamic collection. Initialized once at WM_CREATE from MAHM +
// DXGI; mutated at runtime by SampleMahm (sampled fields) and the context menu
// (per-GPU showInPanel toggle). Count is whatever MAHM reports as active
// (szGpuId[0] != 0), so the set is compact and menu IDs stay contiguous
// (1015 + i, i in [0, g_gpuCount)).
//
// IMPORTANT: g_gpus[] is the runtime truth, not a fixed MAX-size array. New
// hardware or rearranged adapter ordering produces a fresh set each launch
// and the menu/ini rebuild from it. See notes/continue.md "GPU set" entry.
// ---------------------------------------------------------------------------
struct GpuEntry {
    BOOL   active;            // always TRUE (filtered out inactive slots)
    int    mahmIndex;         // MAHM gpuEntry array index (for GetSource)
    TCHAR  name[64];          // friendly name (szDevice[0]?szDevice:szGpuId)
    TCHAR  device[160];       // MAHM szDevice verbatim
    TCHAR  gpuId[160];        // MAHM szGpuId verbatim ("VEN_10DE&DEV_28A0&...")
    TCHAR  gpuIdSafe[160];    // gpuId with '&' '=' replaced by '_' (ini key)
    double vramTotalGB;       // filled by DXGI match at startup; 0 = no match
    BOOL   vramFromDxgi;      // for log only
    double tempC;             // last sample
    double usagePct;
    double vramUsedGB;        // computed from vramTotalGB * (raw/100)/100
    TCHAR  tempStr[16];
    TCHAR  usageStr[16];
    TCHAR  vramStr[32];
    BOOL   showInPanel;       // ini-persisted; menu ID = 1015 + i
};

static GpuEntry* g_gpus      = NULL;
static int       g_gpuCount  = 0;

// DXGI adapter info — enumerated once at startup for VRAM total fallback.
// MAHM doesn't fill dwMemAmount reliably (see notes/continue.md "坑 2"), so
// DXGI is the only public-API way to get per-card VRAM total.
struct DxgiAdapterInfo {
    TCHAR   description[128];
    UINT    vendorId;        // 0x10DE NVIDIA, 0x8086 Intel, 0x1002 AMD, ...
    UINT64  dedicatedBytes;  // OS-visible dedicated VRAM (e.g. 7.77GB on a
                             // nominal-8GB card — WDDM reserves ~256MB)
};
static DxgiAdapterInfo* g_dxgi = NULL;
static int              g_dxgiCount = 0;

// Cached strings (formatted once per sample tick). Only CPU/RAM remain
// global; GPU strings live per-node inside g_gpus[].name/tempStr/etc.
static TCHAR   g_cpuTempStr[32]   = TEXT("--");
static TCHAR   g_cpuUsageStr[32]  = TEXT("--");
static TCHAR   g_ramStr[32]       = TEXT("--");     // "X.XG / YY.YG (NN%)"
static TCHAR   g_cpuName[64]      = TEXT("CPU");
static BOOL    g_mahmAvailable    = FALSE;

// GPU threshold (single value for all GPUs). Per-GPU threshold is overkill
// for the floating overlay -- one global threshold keeps the flash state
// machine and ini semantics simple. Set by ini [threshold] Gpu= or hardware
// auto-match (see notes/continue.md).

// Hover-fade-in state: when the cursor sits inside the window for
// kHoverDelayMs, the window leaves "click-through" mode (WS_EX_TRANSPARENT)
// and renders fully opaque so the user can see the overlay is now interactive.
// When the cursor leaves the window, click-through is restored and the alpha
// is reverted to whatever the user set in the Config dialog.
static BOOL    g_hoverActive        = FALSE;   // currently in hover-fade-in mode
static POINT   g_lastCursorPt       = {-1, -1}; // last cursor position seen by HoverPoll
static BOOL    g_lastCursorInRect   = FALSE;    // last PtInRect result
static int     g_savedBgAlpha       = -1;      // -1 means "no saved value"
static int     g_savedFgAlpha       = -1;

static int      g_cpuThreshold     = 85;
static int      g_gpuThreshold     = 85;

// Flash state machine. Toggled by kTimerFlash (every 360 ms). When FALSE the
// row draws in fgColor; when TRUE the row draws in warning red. The state is
// pushed/popped by SampleMahm based on whether the latest reading crosses the
// threshold — hysteresis-free, but the 1 s SampleMahm cadence already filters
// out sub-second jitter.
//
// Flash flags: one per CPU and one shared across GPUs (single threshold
// implies single flash flag — see g_gpuThreshold comment above).
static BOOL     g_flashCpuOn       = FALSE;
static BOOL     g_flashGpuOn       = FALSE;
static BOOL     g_flashTickOdd     = FALSE;  // toggled each kTimerFlash fire
static BOOL     g_hwFingerprintChecked = FALSE;  // one-shot after first SampleMahm

// ---------------------------------------------------------------------------
// Logging — writes to viewTemp.log next to viewTemp.exe (truncated on launch)
//          AND to OutputDebugString. Both UTF-16, line-buffered by the OS.
// ---------------------------------------------------------------------------
static FILE* g_log = NULL;
static void LogOpen()
{
    if (g_log) return;
    TCHAR path[MAX_PATH];
    GetModuleFileName(NULL, path, MAX_PATH);
    TCHAR* slash = _tcsrchr(path, TEXT('\\'));
    if (slash) slash[1] = 0;
    _tcscat_s(path, MAX_PATH, TEXT("viewTemp.log"));
    // ccs= encoding is set on the FILE* via _setmode below; keep fopen mode simple.
    errno_t e = _wfopen_s(&g_log, path, L"w");
    if (g_log) {
        // Force UTF-16 little-endian output so the BOM and every code unit
        // land on disk as written. Without this, "w" with TCHAR=WCHAR writes
        // raw UTF-16LE on Windows (no BOM), which is what we want anyway;
        // the call is here as a defensive hook in case a future runtime
        // changes its default.
        int fd = _fileno(g_log);
        if (fd >= 0) _setmode(fd, _O_BINARY);
    }
    TCHAR dbg[512];
#ifdef VERSION_MAJOR
    _sntprintf_s(dbg, _countof(dbg), _TRUNCATE,
                 TEXT("[viewTemp] log open path=%s err=%d ver=v%d.%d\r\n"),
                 path, (int)e, VERSION_MAJOR, VERSION_MINOR);
#else
    _sntprintf_s(dbg, _countof(dbg), _TRUNCATE,
                 TEXT("[viewTemp] log open path=%s err=%d ver=(undefined)\r\n"),
                 path, (int)e);
#endif
    OutputDebugString(dbg);
    if (g_log) { _fputts(dbg, g_log); fflush(g_log); }
}
static void Log(const TCHAR* fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    TCHAR buf[1024];
    _vsntprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    _fputts(buf, g_log);
    fflush(g_log);
    OutputDebugString(buf);
}
static void LogClose()
{
    if (g_log) { Log(TEXT("[viewTemp] exit\r\n")); fclose(g_log); g_log = NULL; }
}

// ---------------------------------------------------------------------------
// Forward decls
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static HWND  CreateOverlay(HINSTANCE);
static void  AddTrayIcon(HWND);
static void  RemoveTrayIcon();
static void  UpdateLayered(HWND);
static BOOL  MapMahm();
static void  UnmapMahm();
static BOOL  SampleMahm();
static void  ShowContextMenu(HWND, POINT);
static void  ApplyConfigToWindow(HWND);
static void  LoadConfigFromIni();
static void  SaveConfigToIni();
static void  GetIniPath(TCHAR* out, size_t cch);
static void  ReadCpuNameFromRegistry(TCHAR* out, size_t cch);
static BOOL  MatchHardwareThresholds(int* outCpu, int* outGpu);
static void  WriteThresholdSection(int cpuThr, int gpuThr);
static void  WriteHardwareFingerprintSection(const TCHAR* cpuName, const TCHAR* gpuName);
static BOOL  LoadHardwareFingerprint(TCHAR* cpuOut, size_t cpuCch,
                                      TCHAR* gpuOut, size_t gpuCch);
// static void  Dxgi_InitVramTotal();  // removed -- superseded by InitDxgiAdapterMap
static void  InitGpuSet();                  // build g_gpus[] from MAHM
static void  InitDxgiAdapterMap();          // build g_dxgi[] + match vramTotalGB
static void  BuildGpuIdSafe(const char* src, TCHAR* dst, size_t cch);
static void  LoadGpuShowFromIni();          // per-GPU showInPanel ini read
static void  LogOpen();
static void  Log(const TCHAR* fmt, ...);
static void  LogClose();
static void  EnterHoverMode(HWND);
static void  LeaveHoverMode(HWND);
static void  HoverPoll(HWND);
static BOOL  IsAutoStartEnabled();
static void  SetAutoStartEnabled(BOOL enable);

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    LogOpen();
    Log(TEXT("[viewTemp] wWinMain enter pid=%lu\r\n"), GetCurrentProcessId());

    // Pick UI language once. PRIMARYLANGID extracts the language family
    // (LANG_CHINESE vs LANG_ENGLISH) so we don't care about sub-langs
    // (zh-CN vs zh-TW, en-US vs en-GB all collapse correctly).
    {
        LANGID lid = GetUserDefaultUILanguage();
        WORD primary = PRIMARYLANGID(lid);
        g_isChinese = (primary == LANG_CHINESE);
        Log(TEXT("[i18n] GetUserDefaultUILanguage=0x%04x primary=0x%02x -> %s\r\n"),
            lid, primary, g_isChinese ? TEXT("zh") : TEXT("en"));
    }

    Gdiplus::GdiplusStartupInput gdipsi;
    ULONG_PTR gdipToken;
    if (Gdiplus::GdiplusStartup(&gdipToken, &gdipsi, NULL) != Gdiplus::Ok)
        return 1;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    LoadConfigFromIni();

    WNDCLASSEX wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = kWndClass;
    if (!RegisterClassEx(&wc))
        return 1;

    // Position: top-right corner of the entire Windows desktop
    // (no work-area inset — 老林 wants the literal top-right of the screen).
    const int kInitW = 260;
    const int kInitH = 90;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int initX = screenW - kInitW;
    if (initX < 0) initX = 0;

    HWND hwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        kWndClass,
        TEXT("viewTemp"),
        WS_POPUP,
        initX, 0, kInitW, kInitH,
        NULL, NULL, hInst, NULL);
    if (!hwnd)
        return 1;

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateLayered(hwnd);

    SetTimer(hwnd, kTimerSample, g_cfg.intervalMs, NULL);
    SetTimer(hwnd, kTimerIdle,   5000,        NULL);
    SetTimer(hwnd, kTimerHoverPoll, kHoverPollMs, NULL);
    SetTimer(hwnd, kTimerFlash, kFlashIntervalMs, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    LogClose();
    CoUninitialize();
    Gdiplus::GdiplusShutdown(gdipToken);
    return (int)msg.wParam;
}

// ---------------------------------------------------------------------------
// Window proc
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_CREATE:
        Log(TEXT("[viewTemp] WM_CREATE hwnd=%p\r\n"), hwnd);
        AddTrayIcon(hwnd);
        MapMahm();
        InitGpuSet();           // populate g_gpus[] from MAHM
        InitDxgiAdapterMap();   // populate g_dxgi[] + match vramTotalGB
        SampleMahm();
        return 0;

    case WM_TIMER:
        if (w == kTimerSample) {
            if (SampleMahm())
                UpdateLayered(hwnd);
        } else if (w == kTimerIdle) {
            BOOL focused = (GetForegroundWindow() == hwnd) ||
                           (GetCapture() == hwnd);
            UINT want = focused ? kSampleIntervalMs : kIdleIntervalMs;
            if (want != (UINT)g_cfg.intervalMs) {
                g_cfg.intervalMs = want;
                KillTimer(hwnd, kTimerSample);
                SetTimer(hwnd, kTimerSample, want, NULL);
            }
        } else if (w == kTimerHoverIn) {
            KillTimer(hwnd, kTimerHoverIn);
            EnterHoverMode(hwnd);
        } else if (w == kTimerHoverPoll) {
            HoverPoll(hwnd);
        } else if (w == kTimerFlash) {
            // Toggle the flash phase. Only redraw if at least one row is in
            // the alert state — otherwise the flicker would cost a full GDI+
            // repaint every 360 ms for no visible change.
            g_flashTickOdd = !g_flashTickOdd;
            if (g_flashCpuOn || g_flashGpuOn)
                UpdateLayered(hwnd);
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_NCHITTEST: {
        LRESULT ht = DefWindowProc(hwnd, msg, w, l);
        // While topMost the window should drag from anywhere in the client
        // area (covers the "hover-only shows the value if I'm topmost" case).
        // When NOT topMost we leave NCHITTEST alone so the window can sit
        // behind other apps without stealing drag/click events.
        if (g_cfg.topMost && ht == HTCLIENT) return HTCAPTION;
        return ht;
    }

    // Note: WM_NCMOUSEMOVE / WM_NCMOUSELEAVE removed. With WS_EX_LAYERED +
    // a partially transparent background, hit-test (and therefore NC mouse
    // tracking) skips pixels whose alpha < 255 — i.e. anywhere except the
    // text glyphs themselves. The hover-fade-in detector therefore used to
    // think the cursor "left" the window whenever it slid off the text, even
    // while still physically inside the window rect. We now poll the cursor
    // position every kHoverPollMs and use PtInRect(GetWindowRect) for the
    // "inside / outside" decision. See HoverPoll() below.

    case WM_LBUTTONDBLCLK: {
        LONG ex = GetWindowLong(hwnd, GWL_EXSTYLE);
        ex ^= WS_EX_TRANSPARENT;
        SetWindowLong(hwnd, GWL_EXSTYLE, ex);
        // Cancel any pending hover-fade-in countdown — the user just made an
        // explicit choice about click-through, so the auto-toggle is no
        // longer welcome. If we were already in hover-fade-in, leave the
        // alpha alone (the user can see the overlay is opaque now); the next
        // cursor-leave will still call LeaveHoverMode and re-add
        // WS_EX_TRANSPARENT regardless of the toggle direction.
        KillTimer(hwnd, kTimerHoverIn);
        return 0;
    }

    case WM_RBUTTONUP:
    case WM_NCRBUTTONUP: {
        // WM_NCRBUTTONUP fires here because WM_NCHITTEST rewrites HTCLIENT
        // to HTCAPTION (so the user can drag the overlay from anywhere).
        // The window never sees a plain WM_RBUTTONUP unless the user clicks
        // on a part that really is the client area, which is none right now.
        // Handling both keeps the menu reachable from any future layout.
        POINT p; GetCursorPos(&p);
        ShowContextMenu(hwnd, p);
        return 0;
    }

    case kTrayMsg:
        if (l == WM_RBUTTONUP) {
            POINT p; GetCursorPos(&p);
            SetForegroundWindow(hwnd);
            ShowContextMenu(hwnd, p);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kTimerHoverPoll);
        KillTimer(hwnd, kTimerHoverIn);
        KillTimer(hwnd, kTimerFlash);
        KillTimer(hwnd, kTimerSample);
        KillTimer(hwnd, kTimerIdle);
        RemoveTrayIcon();
        UnmapMahm();
        // Free GPU set + DXGI arrays (allocated in InitGpuSet / InitDxgiAdapterMap)
        delete[] g_gpus; g_gpus = NULL; g_gpuCount = 0;
        delete[] g_dxgi;  g_dxgi  = NULL; g_dxgiCount = 0;
        PostQuitMessage(0);
        return 0;

    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_MINIMIZE) return 0;
        return DefWindowProc(hwnd, msg, w, l);

    default:
        return DefWindowProc(hwnd, msg, w, l);
    }
}

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------
static void ShowContextMenu(HWND hwnd, POINT pt)
{
    HMENU hm = CreatePopupMenu();
    AppendMenu(hm, MF_STRING | (g_cfg.topMost    ? MF_CHECKED : 0), 1001, LANG_IF("始终置顶", "Always on Top"));
    AppendMenu(hm, MF_STRING | (g_cfg.showCpu     ? MF_CHECKED : 0), 1002, LANG_IF("显示 CPU", "Show CPU"));
    AppendMenu(hm, MF_STRING | (g_cfg.showRam     ? MF_CHECKED : 0), 1006, LANG_IF("显示 RAM", "Show RAM"));
    AppendMenu(hm, MF_STRING | (g_cfg.showUsage   ? MF_CHECKED : 0), 1004, LANG_IF("显示使用率", "Show Usage"));
    AppendMenu(hm, MF_STRING | (g_cfg.showVram    ? MF_CHECKED : 0), 1005, LANG_IF("显示 VRAM", "Show VRAM"));
    AppendMenu(hm, MF_SEPARATOR, 0, NULL);
    // ---- GPU set: one checkbox per detected adapter. IDs start at 1015+i
    // (避开 1003/1006)。g_gpuCount 决定菜单项数。1003 (老 showGpu 单项) 已废弃
    // ——新的"显 GPU"语义由"任一 g_gpus[i].showInPanel"承担,等价于老 g_cfg.showGpu。
    AppendMenu(hm, MF_STRING | (g_cfg.showGpu     ? MF_CHECKED : 0), 1003, LANG_IF("显示 GPU", "Show GPU"));
    for (int i = 0; i < g_gpuCount; i++) {
        TCHAR item[200];
        _sntprintf_s(item, _countof(item), _TRUNCATE,
                     TEXT("%s GPU %d: %s"),
                     g_gpus[i].showInPanel ? TEXT("[v]") : TEXT("[ ]"),
                     i, g_gpus[i].name);
        AppendMenu(hm, MF_STRING, (UINT)(1015 + i), item);
    }
    AppendMenu(hm, MF_STRING, 1032, LANG_IF("自动匹配当前硬件阈值", "Auto-match Hardware Thresholds"));
    AppendMenu(hm, MF_SEPARATOR, 0, NULL);
    AppendMenu(hm, MF_STRING, 1010, LANG_IF("字体加大 +", "Larger Font +"));
    AppendMenu(hm, MF_STRING, 1011, LANG_IF("字体缩小 -", "Smaller Font -"));
    AppendMenu(hm, MF_STRING, 1012, LANG_IF("透明度 +", "Opacity +"));
    AppendMenu(hm, MF_STRING, 1013, LANG_IF("透明度 -", "Opacity -"));
    AppendMenu(hm, MF_SEPARATOR, 0, NULL);
    AppendMenu(hm, MF_STRING, 1020, LANG_IF("字体颜色：白", "Font Color: White"));
    AppendMenu(hm, MF_STRING, 1021, LANG_IF("字体颜色：黄", "Font Color: Yellow"));
    AppendMenu(hm, MF_STRING, 1022, LANG_IF("字体颜色：绿", "Font Color: Green"));
    AppendMenu(hm, MF_STRING, 1023, LANG_IF("字体颜色：青", "Font Color: Cyan"));
    AppendMenu(hm, MF_STRING, 1024, LANG_IF("字体颜色：红", "Font Color: Red"));
    AppendMenu(hm, MF_SEPARATOR, 0, NULL);
    AppendMenu(hm, MF_STRING, 1030, LANG_IF("保存配置", "Save Config"));
    AppendMenu(hm, MF_STRING, 1031, LANG_IF("重置位置", "Reset Position"));
    AppendMenu(hm, MF_SEPARATOR, 0, NULL);
    AppendMenu(hm, MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : 0),
               1050, LANG_IF("随 Windows 启动", "Start with Windows"));
    AppendMenu(hm, MF_SEPARATOR, 0, NULL);
    AppendMenu(hm, MF_STRING, 1099, LANG_IF("退出 viewTemp", "Exit viewTemp"));

    int cmd = TrackPopupMenu(hm, TPM_RETURNCMD | TPM_NONOTIFY,
                             pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hm);

    // ---- GPU set submenu: 1015 + i for i in [0, g_gpuCount) ----
    if (cmd >= 1015 && cmd < 1015 + g_gpuCount) {
        int slot = cmd - 1015;
        g_gpus[slot].showInPanel = !g_gpus[slot].showInPanel;
        // Persist immediately under the stable PCI-path-derived key (see
        // notes/continue.md "menu ID vs ini key separation" rationale).
        TCHAR iniPath[MAX_PATH]; GetIniPath(iniPath, MAX_PATH);
        TCHAR key[200];
        _sntprintf_s(key, _countof(key), _TRUNCATE,
                     TEXT("ShowGpu_%s"), g_gpus[slot].gpuIdSafe);
        WritePrivateProfileString(TEXT("view"), key,
            g_gpus[slot].showInPanel ? TEXT("1") : TEXT("0"), iniPath);
        Log(TEXT("[Menu] gpu[%d] '%s' showInPanel -> %d (ini key ShowGpu_%s)\r\n"),
            slot, g_gpus[slot].name, (int)g_gpus[slot].showInPanel,
            g_gpus[slot].gpuIdSafe);
        ApplyConfigToWindow(hwnd);
        UpdateLayered(hwnd);
        return;
    }

    switch (cmd) {
    case 1001:
        g_cfg.topMost = !g_cfg.topMost;
        SetWindowPos(hwnd, g_cfg.topMost ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0,0,0,0, SWP_NOMOVE|SWP_NOSIZE);
        // If we just dropped topMost, kill any pending hover-fade-in: hover
        // is only meaningful while topMost (the window should not steal
        // clicks from other apps when sitting behind them).
        if (!g_cfg.topMost) {
            KillTimer(hwnd, kTimerHoverIn);
            LeaveHoverMode(hwnd);
        }
        break;
    case 1002: g_cfg.showCpu = !g_cfg.showCpu; break;
    case 1003:
        // 老 showGpu 单项:作为总开关。现在等价于"全部 hide/show"。
        // 翻转 g_cfg.showGpu;新代码读它判断"GPU 段是否整段渲染"。
        g_cfg.showGpu = !g_cfg.showGpu;
        break;
    case 1004: g_cfg.showUsage = !g_cfg.showUsage; break;
    case 1005: g_cfg.showVram = !g_cfg.showVram; break;
    case 1006: g_cfg.showRam = !g_cfg.showRam; break;
    case 1010: g_cfg.fontSize = ClampInt(g_cfg.fontSize + 2, 8, 96); break;
    case 1011: g_cfg.fontSize = ClampInt(g_cfg.fontSize - 2, 8, 96); break;
    case 1012: g_cfg.opacityPct = ClampInt(g_cfg.opacityPct + 5, 20, 100); break;
    case 1013: g_cfg.opacityPct = ClampInt(g_cfg.opacityPct - 5, 20, 100); break;
    case 1020: g_cfg.fgColor = RGB(255,255,255); break;
    case 1021: g_cfg.fgColor = RGB(255,255,0);   break;
    case 1022: g_cfg.fgColor = RGB(0,255,0);     break;
    case 1023: g_cfg.fgColor = RGB(0,255,255);   break;
    case 1024: g_cfg.fgColor = RGB(255,64,64);   break;
    case 1030: SaveConfigToIni(); break;
    case 1031: {
        // Reset position to top-right of the primary desktop. Use the window's
        // CURRENT width (not the create-time 260) because UpdateLayeredWindow
        // resizes the window to the measured content width on every paint tick.
        RECT rc; GetWindowRect(hwnd, &rc);
        int curW  = rc.right - rc.left;
        int scrW  = GetSystemMetrics(SM_CXSCREEN);
        int newX  = scrW - curW;
        if (newX < 0) newX = 0;
        SetWindowPos(hwnd, 0, newX, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        break;
    }
    case 1032: {
        // Force-overwrite the [threshold] section from the hardware database,
        // ignoring whatever the user previously wrote. SampleMahm's hot-reload
        // will pick up the new values within ≤1 s, which then re-evaluates
        // the flash state machine.
        int cpuThr = 0, gpuThr = 0;
        MatchHardwareThresholds(&cpuThr, &gpuThr);
        g_cpuThreshold = cpuThr;
        g_gpuThreshold = gpuThr;
        WriteThresholdSection(cpuThr, gpuThr);
        // Refresh the fingerprint so the next startup doesn't re-trigger.
        // Pick the first dGPU (NVIDIA/AMD) name for the match; fallback to
        // g_gpus[0].name if none are dGPU. See SampleMahm phase 2 for the
        // same selection logic — must agree so fingerprints don't bounce.
        TCHAR curCpu[128] = {};
        ReadCpuNameFromRegistry(curCpu, _countof(curCpu));
        const TCHAR* matchName = TEXT("");
        if (g_gpuCount > 0) matchName = g_gpus[0].name;
        for (int i = 0; i < g_gpuCount; i++) {
            // Vendor bytes "10DE" / "1002" at offset 4-7 of gpuId ("VEN_10DE...")
            if (g_gpus[i].gpuId[4] == TEXT('1') && g_gpus[i].gpuId[5] == TEXT('0') &&
                (g_gpus[i].gpuId[6] == TEXT('D') && g_gpus[i].gpuId[7] == TEXT('E')) ||
                (g_gpus[i].gpuId[6] == TEXT('0') && g_gpus[i].gpuId[7] == TEXT('2'))) {
                matchName = g_gpus[i].name;
                break;
            }
        }
        WriteHardwareFingerprintSection(curCpu, matchName);
        Log(TEXT("[Menu] auto-match thresholds -> cpu=%d gpu=%d (gpu='%s')\r\n"),
            cpuThr, gpuThr, matchName);
        break;
    }
    case 1050: {
        BOOL now = IsAutoStartEnabled();
        SetAutoStartEnabled(!now);
        Log(TEXT("[AutoStart] %s -> %s\r\n"),
            now ? TEXT("ON") : TEXT("OFF"),
            now ? TEXT("OFF") : TEXT("ON"));
        break;
    }
    case 1099: DestroyWindow(hwnd); return;
    }

    g_cfg.fgAlpha = (BYTE)(g_cfg.opacityPct * 255 / 100);
    g_cfg.bgAlpha = (BYTE)(g_cfg.opacityPct * 220 / 100);
    ApplyConfigToWindow(hwnd);
    UpdateLayered(hwnd);
}

// ---------------------------------------------------------------------------
// Windows auto-start
// ---------------------------------------------------------------------------
// Reads/writes the user-scope Run registry key:
//   HKCU\Software\Microsoft\Windows\CurrentVersion\Run\viewTemp = "<exe path>"
// The exe path is captured at startup (GetModuleFileName returns the running
// .exe's full path even when launched via Run). We compare it loosely — any
// non-empty value whose file path equals our exe counts as "enabled".
static const TCHAR* kRunKeyPath =
    TEXT("Software\\Microsoft\\Windows\\CurrentVersion\\Run");
static const TCHAR* kRunValueName = TEXT("viewTemp");

static BOOL IsAutoStartEnabled()
{
    HKEY hKey = NULL;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, kRunKeyPath, 0,
                     KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) return FALSE;
    TCHAR val[MAX_PATH] = {};
    DWORD cb = sizeof(val), type = 0;
    LONG rc = RegQueryValueEx(hKey, kRunValueName, NULL, &type,
                              (LPBYTE)val, &cb);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return FALSE;
    // Match if the registered command line contains our exe path. Compare
    // against the actual running exe path so a stale value (or a renamed
    // .exe) doesn't keep auto-start "on" forever.
    TCHAR self[MAX_PATH] = {};
    GetModuleFileName(NULL, self, MAX_PATH);
    if (!self[0] || !val[0]) return FALSE;
    return _tcsstr(val, self) != NULL;
}

static void SetAutoStartEnabled(BOOL enable)
{
    HKEY hKey = NULL;
    DWORD disp = 0;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, kRunKeyPath, 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL,
                       &hKey, &disp) != ERROR_SUCCESS) return;

    if (!enable) {
        RegDeleteValue(hKey, kRunValueName);
        RegCloseKey(hKey);
        return;
    }
    // Register with quotes around the path so spaces in the install dir
    // are handled; no arguments needed since the overlay has no file to open.
    TCHAR self[MAX_PATH] = {};
    GetModuleFileName(NULL, self, MAX_PATH);
    TCHAR cmdLine[MAX_PATH + 4];
    _sntprintf_s(cmdLine, _countof(cmdLine), _TRUNCATE, TEXT("\"%s\""), self);
    RegSetValueEx(hKey, kRunValueName, 0, REG_SZ,
                  (LPBYTE)cmdLine, (DWORD)((_tcslen(cmdLine) + 1) * sizeof(TCHAR)));
    RegCloseKey(hKey);
}

// ---------------------------------------------------------------------------
// Layered rendering
// ---------------------------------------------------------------------------
static void UpdateLayered(HWND hwnd)
{
    HDC screen = GetDC(NULL);
    if (!screen) return;

    // Pick a font family. FontFamily::operator= is private, so we must
    // construct one and check its status before using it. Consolas ships
    // with Win10/11; fall back to Tahoma or the system default.
    Gdiplus::FontFamily ff(L"Consolas");
    Gdiplus::FontFamily ffFallback(L"Tahoma");
    Gdiplus::FontFamily* pFamily = &ff;
    if (ffFallback.GetLastStatus() == Gdiplus::Ok) pFamily = &ffFallback;
    Gdiplus::Font font(pFamily, (Gdiplus::REAL)g_cfg.fontSize,
                       Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    // Background fill uses an ARGB color, not a SolidBrush ctor with 4 BYTEs.
    Gdiplus::Color fgColor(g_cfg.fgAlpha,
                           GetRValue(g_cfg.fgColor),
                           GetGValue(g_cfg.fgColor),
                           GetBValue(g_cfg.fgColor));
    Gdiplus::SolidBrush fg(fgColor);

    // Warning color for flashing rows: fully opaque red, independent of the
    // fgAlpha/opacity slider — a half-transparent alert color is hard to read.
    const Gdiplus::Color warnColor(255, 255, 32, 32);

    // Pick a brush for a temperature row. flashOn toggles the visible color
    // between the user-configured fgColor and warn red on each kTimerFlash
    // tick — giving the row a heartbeat when its sensor is over threshold.
    auto tempBrush = [&](BOOL flashOn)->Gdiplus::SolidBrush* {
        static Gdiplus::SolidBrush normal(fgColor);
        static Gdiplus::SolidBrush warn (warnColor);
        if (flashOn && g_flashTickOdd) return &warn;
        normal.SetColor(fgColor);
        return &normal;
    };

    WCHAR lineBuf[256];
    int  lh = g_cfg.fontSize + 6;
    int  widthPx = 200, heightPx = 6;

    auto measure = [&](const WCHAR* s)->int {
        if (!s || !*s) return 0;
        HDC sc2 = GetDC(NULL);
        Gdiplus::Graphics gx(sc2);
        gx.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        Gdiplus::RectF bbox;
        Gdiplus::RectF layoutRect(0, 0, 4096, 4096);
        gx.MeasureString(s, -1, &font, layoutRect, &bbox);
        ReleaseDC(NULL, sc2);
        return (int)ceil(bbox.Width);
    };

    if (g_cfg.showCpu) {
        StringCchPrintfW(lineBuf, 256, L"%s: %s", g_cpuName, g_cpuTempStr);
        int w = measure(lineBuf);
        if (w > widthPx) widthPx = w;
        heightPx += lh;
    }
    // ---- GPU set (replaces old single-GPU showGpu block) ----
    // For each active GPU, render up to 4 lines:
    //   name (always, when showGpu master is on and this card is checked),
    //   + temp (if master showGpu && showVram OR master showUsage; we keep
    //           all 4 always shown for parity with the old single-GPU look),
    //   + usage,
    //   + vram.
    // showGpu (master) AND showInPanel (per-card) both gate the entire block.
    // showUsage / showVram gate just the temp/usage/vram fields when set.
    if (g_cfg.showGpu) {
        for (int i = 0; i < g_gpuCount; i++) {
            GpuEntry* entry = &g_gpus[i];
            if (!entry->showInPanel) continue;
            StringCchPrintfW(lineBuf, 256, L"%s", entry->name);
            int w = measure(lineBuf);
            if (w > widthPx) widthPx = w;
            heightPx += lh;
            // Temp line (always shown when this card is shown; flash honored)
            StringCchPrintfW(lineBuf, 256, L"  Temp: %s", entry->tempStr);
            w = measure(lineBuf);
            if (w > widthPx) widthPx = w;
            heightPx += lh;
            // Usage line
            if (g_cfg.showUsage) {
                StringCchPrintfW(lineBuf, 256, L"  Usage: %s", entry->usageStr);
                w = measure(lineBuf);
                if (w > widthPx) widthPx = w;
                heightPx += lh;
            }
            // VRAM line
            if (g_cfg.showVram) {
                StringCchPrintfW(lineBuf, 256, L"  VRAM: %s", entry->vramStr);
                w = measure(lineBuf);
                if (w > widthPx) widthPx = w;
                heightPx += lh;
            }
        }
    }
    if (g_cfg.showRam) {
        StringCchPrintfW(lineBuf, 256, L"RAM: %s", g_ramStr);
        int w = measure(lineBuf);
        if (w > widthPx) widthPx = w;
        heightPx += lh;
    }
    // Note: showUsage's OLD single line ("CPU %s / GPU %s") is dropped in favor
    // of per-GPU "Usage: %s" rows above. showVram is per-GPU now too.
    widthPx += 20;
    heightPx += 12;

    RECT rc; GetWindowRect(hwnd, &rc);
    // Anchor the window's right edge to the right side of the primary screen.
    // We can't trust the window's reported left (rc.left) — UpdateLayeredWindow
    // resizes the window to (widthPx x heightPx) on every call, so if rc.left
    // stays at the create-time value while widthPx grows, the right edge spills
    // off-screen. Recompute X from rc.right (always tracks screenW at startup).
    POINT dst = { rc.right - widthPx, rc.top };
    if (dst.x < 0) dst.x = 0;
    SIZE  sz  = { widthPx, heightPx };
    POINT src = { 0, 0 };

    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, widthPx, heightPx);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    Gdiplus::Graphics g(mem);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    Gdiplus::SolidBrush clear(Gdiplus::Color(0, 0, 0, 0));
    g.FillRectangle(&clear, (Gdiplus::REAL)0, (Gdiplus::REAL)0,
                    (Gdiplus::REAL)widthPx, (Gdiplus::REAL)heightPx);

    // Background fill. transparentBg=TRUE means "no fill at all" (a clean alpha
// rectangle). When the user is hovering and we want a strong "you can click
// here" cue, we override and FORCE a fill (and a border), regardless of
// transparentBg — otherwise the hover indicator would never be visible.
{
    BOOL drawBg = !g_cfg.transparentBg || g_hoverActive;
    if (drawBg) {
        BYTE bgA = g_cfg.bgAlpha;
        BYTE bgR = 30, bgG = 30, bgB = 30;
        if (g_hoverActive) {
            // Bright accent: dark green panel, fully opaque.
            bgA = 255;
            bgR = 30; bgG = 80; bgB = 30;
        }
        Gdiplus::SolidBrush bgFill(Gdiplus::Color(bgA, bgR, bgG, bgB));
        g.FillRectangle(&bgFill, (Gdiplus::REAL)0, (Gdiplus::REAL)0,
                        (Gdiplus::REAL)widthPx, (Gdiplus::REAL)heightPx);
    }

    // Hover indicator: bright 2px outline only while interactive. A border
    // is the strongest signal we can render through the existing alpha
    // pipeline — bumping bgAlpha from 220 to 255 was imperceptible, and
    // even a color shift to dark green fails when transparentBg=TRUE
    // suppresses the fill entirely.
    if (g_hoverActive) {
        Gdiplus::Pen outlinePen(Gdiplus::Color(255, 0, 220, 0), 2.0f);
        g.DrawRectangle(&outlinePen, 0, 0, widthPx - 1, heightPx - 1);
    }
}

    int yy = 6;
    if (g_cfg.showCpu) {
        StringCchPrintfW(lineBuf, 256, L"%s: %s", g_cpuName, g_cpuTempStr);
        g.DrawString(lineBuf, -1, &font, Gdiplus::PointF(10, (Gdiplus::REAL)yy), tempBrush(g_flashCpuOn));
        yy += lh;
    }
    // ---- GPU set rendering (mirrors the measurement loop above) ----
    if (g_cfg.showGpu) {
        for (int i = 0; i < g_gpuCount; i++) {
            GpuEntry* entry = &g_gpus[i];
            if (!entry->showInPanel) continue;
            // Name line (no flash on names — temp is the alerted field)
            StringCchPrintfW(lineBuf, 256, L"%s", entry->name);
            g.DrawString(lineBuf, -1, &font, Gdiplus::PointF(10, (Gdiplus::REAL)yy), &fg);
            yy += lh;
            // Temp line (flash honored — any GPU over threshold triggers)
            StringCchPrintfW(lineBuf, 256, L"  Temp: %s", entry->tempStr);
            g.DrawString(lineBuf, -1, &font, Gdiplus::PointF(10, (Gdiplus::REAL)yy), tempBrush(g_flashGpuOn));
            yy += lh;
            // Usage
            if (g_cfg.showUsage) {
                StringCchPrintfW(lineBuf, 256, L"  Usage: %s", entry->usageStr);
                g.DrawString(lineBuf, -1, &font, Gdiplus::PointF(10, (Gdiplus::REAL)yy), &fg);
                yy += lh;
            }
            // VRAM
            if (g_cfg.showVram) {
                StringCchPrintfW(lineBuf, 256, L"  VRAM: %s", entry->vramStr);
                g.DrawString(lineBuf, -1, &font, Gdiplus::PointF(10, (Gdiplus::REAL)yy), &fg);
                yy += lh;
            }
        }
    }
    if (g_cfg.showRam) {
        StringCchPrintfW(lineBuf, 256, L"RAM: %s", g_ramStr);
        g.DrawString(lineBuf, -1, &font, Gdiplus::PointF(10, (Gdiplus::REAL)yy), &fg);
        yy += lh;
    }
    // showUsage's old single line ("CPU X / GPU Y") is dropped — see measurement section.

    BLENDFUNCTION bf = {};
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.AlphaFormat         = g_cfg.transparentBg ? AC_SRC_ALPHA : 0;
    bf.SourceConstantAlpha = 255;
    UpdateLayeredWindow(hwnd, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
}

static void ApplyConfigToWindow(HWND hwnd)
{
    LONG ex = GetWindowLong(hwnd, GWL_EXSTYLE);
    if (g_cfg.topMost) ex |= WS_EX_TOPMOST;
    else               ex &= ~WS_EX_TOPMOST;
    SetWindowLong(hwnd, GWL_EXSTYLE, ex);
}

// ---------------------------------------------------------------------------
// Hover-fade-in (poll-based, not TrackMouseEvent-based)
// ---------------------------------------------------------------------------
// Background: WM_NCMOUSEMOVE / WM_NCMOUSELEAVE behave erratically for a
// WS_EX_LAYERED window that draws its own background with an alpha < 255.
// Hit-testing skips low-alpha pixels, so the non-client mouse tracker thinks
// the cursor has "left" the window the instant it slides off the text onto
// the semi-transparent background — even though the cursor is still inside
// the window's screen rect. We therefore ignore the WM_NC* family entirely
// and poll GetCursorPos + PtInRect(GetWindowRect) every kHoverPollMs. The
// counter only restarts when the cursor's screen position actually changes,
// so a parked cursor reliably times out into hover-fade-in after kHoverDelayMs.

static void HoverPoll(HWND hwnd)
{
    if (!g_cfg.topMost) {
        // Not topMost: hover-fade-in is disabled (would steal clicks from
        // apps that should be in front). Make sure we are not stuck in the
        // hover-active state from a previous topMost session.
        if (g_hoverActive) LeaveHoverMode(hwnd);
        g_lastCursorInRect = FALSE;
        g_lastCursorPt.x = g_lastCursorPt.y = -1;
        return;
    }

    POINT pt;
    GetCursorPos(&pt);

    RECT rc;
    GetWindowRect(hwnd, &rc);
    BOOL inRect = PtInRect(&rc, pt);

    if (inRect != g_lastCursorInRect) {
        Log(TEXT("[Hover] poll inRect %d -> %d pt=(%ld,%ld)\r\n"),
            (int)g_lastCursorInRect, (int)inRect, (long)pt.x, (long)pt.y);
        g_lastCursorInRect = inRect;
    }

    if (!inRect) {
        // Outside the window rect: leave hover-fade-in and stop the timer.
        KillTimer(hwnd, kTimerHoverIn);
        LeaveHoverMode(hwnd);
        g_lastCursorPt = pt;
        return;
    }

    // Inside the rect. Restart the 1.5s countdown only if the cursor MOVED
    // since the last poll — otherwise a parked cursor would never time out.
    if (pt.x != g_lastCursorPt.x || pt.y != g_lastCursorPt.y) {
        g_lastCursorPt = pt;
        KillTimer(hwnd, kTimerHoverIn);
        SetTimer(hwnd, kTimerHoverIn, kHoverDelayMs, NULL);
    }
}

// EnterHoverMode: the cursor has been parked inside the window for kHoverDelayMs.
// Make the window "feel alive" — remove click-through (WS_EX_TRANSPARENT) so
// every pixel of the overlay now responds to mouse drag, and bump the alpha
// to fully opaque so the user sees the change visually.
//
// LeaveHoverMode: the cursor left the window. Restore click-through and the
// saved alpha so the overlay returns to its idle floating-overlay look.
static void EnterHoverMode(HWND hwnd)
{
    if (g_hoverActive) return;
    g_hoverActive = TRUE;
    Log(TEXT("[Hover] Enter bgAlpha=%d fgAlpha=%d ex=0x%08X topMost=%d\r\n"),
        (int)g_cfg.bgAlpha, (int)g_cfg.fgAlpha,
        (unsigned)GetWindowLong(hwnd, GWL_EXSTYLE), (int)g_cfg.topMost);

    // Save current alpha so LeaveHoverMode can restore it exactly.
    g_savedBgAlpha = g_cfg.bgAlpha;
    g_savedFgAlpha = g_cfg.fgAlpha;

    LONG ex = GetWindowLong(hwnd, GWL_EXSTYLE);
    ex &= ~WS_EX_TRANSPARENT;
    SetWindowLong(hwnd, GWL_EXSTYLE, ex);

    g_cfg.bgAlpha = 255;
    g_cfg.fgAlpha = 255;
    UpdateLayered(hwnd);
}

static void LeaveHoverMode(HWND hwnd)
{
    if (!g_hoverActive) return;
    g_hoverActive = FALSE;

    // Restore alpha (clamp to safe range — saved values should always be valid
    // because the only path that writes them is EnterHoverMode above).
    if (g_savedBgAlpha >= 0 && g_savedBgAlpha <= 255)
        g_cfg.bgAlpha = (BYTE)g_savedBgAlpha;
    if (g_savedFgAlpha >= 0 && g_savedFgAlpha <= 255)
        g_cfg.fgAlpha = (BYTE)g_savedFgAlpha;
    g_savedBgAlpha = -1;
    g_savedFgAlpha = -1;

    LONG ex = GetWindowLong(hwnd, GWL_EXSTYLE);
    ex |= WS_EX_TRANSPARENT;
    SetWindowLong(hwnd, GWL_EXSTYLE, ex);

    UpdateLayered(hwnd);

    Log(TEXT("[Hover] Leave bgAlpha=%d fgAlpha=%d exAfter=0x%08X\r\n"),
        (int)g_cfg.bgAlpha, (int)g_cfg.fgAlpha,
        (unsigned)GetWindowLong(hwnd, GWL_EXSTYLE));
}

// ---------------------------------------------------------------------------
// Tray icon
// ---------------------------------------------------------------------------
static void AddTrayIcon(HWND hwnd)
{
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = kTrayMsg;
    g_nid.hIcon            = LoadIcon(NULL, IDI_INFORMATION);
    StringCchCopy(g_nid.szTip, _countof(g_nid.szTip), LANG_IF("viewTemp 硬件监控", "viewTemp hardware monitor"));
    g_trayAdded = Shell_NotifyIcon(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon()
{
    if (g_trayAdded) {
        Shell_NotifyIcon(NIM_DELETE, &g_nid);
        g_trayAdded = FALSE;
    }
}

// ---------------------------------------------------------------------------
// MAHM shared memory
// ---------------------------------------------------------------------------
static BOOL MapMahm()
{
    if (!g_mapMahm) g_mapMahm = OpenFileMapping(FILE_MAP_READ, FALSE, kMapName);
    if (!g_mapMahm) {
        DWORD gle = GetLastError();
        Log(TEXT("[MapMahm] OpenFileMapping FAIL name=%s gle=%lu\r\n"), kMapName, gle);
        return FALSE;
    }
    if (!g_pMahm)  g_pMahm  = MapViewOfFile(g_mapMahm, FILE_MAP_READ, 0, 0, 0);
    if (!g_pMahm)  {
        DWORD gle = GetLastError();
        Log(TEXT("[MapMahm] MapViewOfFile FAIL gle=%lu\r\n"), gle);
        return FALSE;
    }

    auto* h = (MAHM_SHARED_MEMORY_HEADER*)g_pMahm;
    if (h->dwSignature != MAHM_SIGNATURE) {
        DWORD sig = h->dwSignature;
        Log(TEXT("[MapMahm] BAD signature got=0x%08X expect=0x%08X\r\n"),
            sig, (DWORD)MAHM_SIGNATURE);
        UnmapMahm();
        return FALSE;
    }
    g_entryStride   = h->dwEntrySize;
    g_numEntries    = h->dwNumEntries;
    g_gpuEntryCount = h->dwNumGpuEntries;
    Log(TEXT("[MapMahm] OK sig=0x%08X numEntries=%lu entryStride=%lu gpuEntries=%lu gpuEntrySize=%lu\r\n"),
        (DWORD)h->dwSignature, (unsigned long)g_numEntries,
        (unsigned long)g_entryStride, (unsigned long)g_gpuEntryCount,
        (unsigned long)h->dwGpuEntrySize);
    return TRUE;
}

static void UnmapMahm()
{
    if (g_pMahm)  { UnmapViewOfFile(g_pMahm); g_pMahm = NULL; }
    if (g_mapMahm){ CloseHandle(g_mapMahm);   g_mapMahm = NULL; }
}

// Helper: get value of a single MAHM source.
// gpu == 0xFFFFFFFF -> look at global/CPU entries; otherwise match dwGpu index.
// Returns FLT_MAX if not found.
static float GetSource(DWORD gpu, DWORD srcId)
{
    if (!g_pMahm || g_numEntries == 0 || g_entryStride == 0) return FLT_MAX;
    BYTE* base = (BYTE*)g_pMahm + sizeof(MAHM_SHARED_MEMORY_HEADER);
    for (DWORD i = 0; i < g_numEntries; ++i) {
        auto* e = (MAHM_SHARED_MEMORY_ENTRY*)(base + i * g_entryStride);
        if (e->dwSrcId == srcId && e->dwGpu == gpu) {
            return e->data;
        }
    }
    return FLT_MAX;
}

// Read first GPU's total VRAM (KB) into out. Returns TRUE if known.
// SUPERSEDED by InitGpuSet + InitDxgiAdapterMap. Kept as a no-op stub so
// any stray linker reference compiles (in case notes/continue.md "坑 2"
// test code or future tooling references it). New code MUST use the GPU
// set's vramTotalGB directly.
static BOOL GetGpuVramTotalKb(DWORD gpuIndex, DWORD* outKb)
{
    (void)gpuIndex;
    if (outKb) *outKb = 0;
    return FALSE;
}

// Get GPU device display name (e.g. "GeForce RTX 4090") if known.
// SUPERSEDED by InitGpuSet (g_gpus[i].name). Kept as a no-op stub.
static void GetGpuName(DWORD gpuIndex, TCHAR* out, size_t cch)
{
    (void)gpuIndex;
    if (out && cch) out[0] = 0;
}

static BOOL SampleMahm()
{
    if (!g_pMahm && !MapMahm()) {
        StringCchCopy(g_cpuTempStr, _countof(g_cpuTempStr), TEXT("N/A"));
        for (int i = 0; i < g_gpuCount; i++) {
            StringCchCopy(g_gpus[i].tempStr,  _countof(g_gpus[i].tempStr),  TEXT("N/A"));
            StringCchCopy(g_gpus[i].usageStr, _countof(g_gpus[i].usageStr), TEXT("N/A"));
            StringCchCopy(g_gpus[i].vramStr,  _countof(g_gpus[i].vramStr),  TEXT("N/A"));
        }
        StringCchCopy(g_ramStr,     _countof(g_ramStr),     TEXT("N/A"));
        g_mahmAvailable = FALSE;
        return TRUE;
    }

    auto* h = (MAHM_SHARED_MEMORY_HEADER*)g_pMahm;
    if (h->dwSignature == MAHM_SIGNATURE_DEAD || g_numEntries == 0) {
        UnmapMahm();
        MapMahm();
        if (!g_pMahm) {
            StringCchCopy(g_cpuTempStr, _countof(g_cpuTempStr), TEXT("--"));
            for (int i = 0; i < g_gpuCount; i++) {
                StringCchCopy(g_gpus[i].tempStr,  _countof(g_gpus[i].tempStr),  TEXT("--"));
                StringCchCopy(g_gpus[i].usageStr, _countof(g_gpus[i].usageStr), TEXT("--"));
                StringCchCopy(g_gpus[i].vramStr,  _countof(g_gpus[i].vramStr),  TEXT("--"));
            }
            StringCchCopy(g_ramStr,     _countof(g_ramStr),     TEXT("--"));
            g_mahmAvailable = FALSE;
            return TRUE;
        }
        h = (MAHM_SHARED_MEMORY_HEADER*)g_pMahm;
        g_entryStride   = h->dwEntrySize;
        g_numEntries    = h->dwNumEntries;
        g_gpuEntryCount = h->dwNumGpuEntries;
    }

    // ---- CPU (global) ----
    float cpuT = GetSource(MAHM_GPU_INDEX_GLOBAL, MAHM_SRC_CPU_TEMPERATURE);
    if (cpuT == FLT_MAX || cpuT <= -100.f) {
        StringCchCopy(g_cpuTempStr, _countof(g_cpuTempStr), TEXT("--"));
    } else {
        StringCchPrintfW(g_cpuTempStr, _countof(g_cpuTempStr), L"%.0f°C", cpuT);
    }
    float cpuU = GetSource(MAHM_GPU_INDEX_GLOBAL, MAHM_SRC_CPU_USAGE);
    if (cpuU == FLT_MAX) StringCchCopy(g_cpuUsageStr, _countof(g_cpuUsageStr), TEXT("--"));
    else                 StringCchPrintfW(g_cpuUsageStr, _countof(g_cpuUsageStr), L"%.0f%%", cpuU);

    // ---- GPU set: per-card sample loop ----
    // Each iteration:
    //   1. Read MAHM_SRC_GPU_TEMPERATURE / GPU_USAGE / MEMORY_USAGE
    //   2. Convert raw -> string, write into g_gpus[i].tempStr/usageStr/vramStr
    //   3. For VRAM: combine MAHM percentage (raw=percent*100) with the
    //      per-card vramTotalGB populated at startup by InitDxgiAdapterMap().
    //      If vramTotalGB==0 (no DXGI match), fall back to "percent only" —
    //      "(NN.N%)" — same as the existing single-GPU fallback.
    for (int i = 0; i < g_gpuCount; i++) {
        GpuEntry* entry = &g_gpus[i];
        DWORD midx = (DWORD)entry->mahmIndex;

        float gpuT = GetSource(midx, MAHM_SRC_GPU_TEMPERATURE);
        if (gpuT == FLT_MAX) {
            StringCchCopy(entry->tempStr, _countof(entry->tempStr), TEXT("--"));
            entry->tempC = 0.0;
        } else {
            entry->tempC = (double)gpuT;
            StringCchPrintfW(entry->tempStr, _countof(entry->tempStr), L"%.0f°C", gpuT);
        }

        float gpuU = GetSource(midx, MAHM_SRC_GPU_USAGE);
        if (gpuU == FLT_MAX) {
            StringCchCopy(entry->usageStr, _countof(entry->usageStr), TEXT("--"));
            entry->usagePct = 0.0;
        } else {
            entry->usagePct = (double)gpuU;
            StringCchPrintfW(entry->usageStr, _countof(entry->usageStr), L"%.0f%%", gpuU);
        }

        float vramPct = GetSource(midx, MAHM_SRC_MEMORY_USAGE);
        if (vramPct == FLT_MAX) {
            StringCchCopy(entry->vramStr, _countof(entry->vramStr), TEXT("--"));
            entry->vramUsedGB = 0.0;
        } else {
            double pct = (double)vramPct / 100.0;  // raw is "percent*100"
            if (entry->vramTotalGB > 0.0) {
                double usedG = entry->vramTotalGB * pct / 100.0;
                entry->vramUsedGB = usedG;
                StringCchPrintfW(entry->vramStr, _countof(entry->vramStr),
                    L"%.2fG / %.2fG (%.1f%%)", usedG, entry->vramTotalGB, pct);
            } else {
                // No DXGI total — display percent only.
                entry->vramUsedGB = 0.0;
                StringCchPrintfW(entry->vramStr, _countof(entry->vramStr),
                    L"%.1f%%", pct);
            }
        }
    }

    // ---- System RAM (Win32 GlobalMemoryStatusEx; not from MAHM) ----
    {
        MEMORYSTATUSEX ms = { sizeof(ms) };
        if (GlobalMemoryStatusEx(&ms) && ms.ullTotalPhys > 0) {
            double totalG  = (double)ms.ullTotalPhys  / (1024.0 * 1024.0 * 1024.0);
            double usedG   = (double)(ms.ullTotalPhys - ms.ullAvailPhys)
                              / (1024.0 * 1024.0 * 1024.0);
            double pct     = (double)ms.dwMemoryLoad;
            StringCchPrintfW(g_ramStr, _countof(g_ramStr),
                              L"%.2fG / %.2fG (%.0f%%)", usedG, totalG, pct);
        } else {
            StringCchCopy(g_ramStr, _countof(g_ramStr), TEXT("--"));
        }
    }

    g_mahmAvailable = TRUE;
    // One-line summary per GPU for log readability. Long lines but 老林 uses
    // DebugView / Notepad -- both wrap fine.
    {
        TCHAR gpuLog[1024] = {};
        TCHAR* p = gpuLog;
        size_t left = _countof(gpuLog);
        for (int i = 0; i < g_gpuCount && left > 32; i++) {
            int wrote = _sntprintf_s(p, left, _TRUNCATE,
                TEXT("[gpu%d '%s'] t=%s u=%s v=%s%s"),
                i, g_gpus[i].name,
                g_gpus[i].tempStr, g_gpus[i].usageStr, g_gpus[i].vramStr,
                (i + 1 < g_gpuCount) ? TEXT(" ") : TEXT(""));
            if (wrote < 0) break;
            p += wrote; left -= wrote;
        }
        Log(TEXT("[Sample-MAHM] cpu=%s cpuU=%s ram=%s %s\r\n"),
            g_cpuTempStr, g_cpuUsageStr, g_ramStr, gpuLog);
    }

    // ---- Threshold flash state machine ----
    // Single GPU flash flag covers all GPU rows (one threshold for the whole
    // set; per-GPU threshold would multiply complexity for no real value).
    // "ANY GPU over threshold -> flash on".
    {
        auto parseCelsius = [](const TCHAR* s) -> float {
            if (!s || !*s || s[0] == TEXT('-') || s[0] == TEXT('N')) return -FLT_MAX;
            return (float)_tcstod(s, nullptr);
        };
        float cpuT = parseCelsius(g_cpuTempStr);
        float maxGpuT = -FLT_MAX;
        for (int i = 0; i < g_gpuCount; i++) {
            float t = parseCelsius(g_gpus[i].tempStr);
            if (t > maxGpuT) maxGpuT = t;
        }
        BOOL cpuHot = (cpuT > (float)g_cpuThreshold);
        BOOL gpuHot = (maxGpuT > (float)g_gpuThreshold);
        if (cpuHot != g_flashCpuOn || gpuHot != g_flashGpuOn) {
            Log(TEXT("[Threshold] cpuT=%.1f cpuThr=%d -> flashCpu=%d  maxGpuT=%.1f gpuThr=%d -> flashGpu=%d\r\n"),
                cpuT, g_cpuThreshold, (int)cpuHot, maxGpuT, g_gpuThreshold, (int)gpuHot);
            g_flashCpuOn = cpuHot;
            g_flashGpuOn = gpuHot;
        }

        // Hot-reload thresholds from ini each tick so users can edit
        // viewTemp.ini and see the change within ≤1 s without restarting.
        // Default arg is the existing value — if the ini/key is missing we
        // keep what we had rather than snapping back to the hardcoded 85.
        TCHAR iniPath[MAX_PATH]; GetIniPath(iniPath, MAX_PATH);
        int newCpu = GetPrivateProfileInt(TEXT("threshold"), TEXT("Cpu"), g_cpuThreshold, iniPath);
        int newGpu = GetPrivateProfileInt(TEXT("threshold"), TEXT("Gpu"), g_gpuThreshold, iniPath);
        if (newCpu < 0)   newCpu = 0;
        if (newGpu < 0)   newGpu = 0;
        if (newCpu > 150) newCpu = 150;
        if (newGpu > 150) newGpu = 150;
        if (newCpu != g_cpuThreshold || newGpu != g_gpuThreshold) {
            Log(TEXT("[Ini] thresholds hot-reload cpu=%d gpu=%d\r\n"), newCpu, newGpu);
            g_cpuThreshold = newCpu;
            g_gpuThreshold = newGpu;
        }
    }

    // ---- (NVAPI fallback removed — see NvApi.h stub and continue.md 坑 2) ----

    // Hardware-fingerprint phase 2: now that g_gpus[0].name has been set
    // from MAHM (it carries the friendly name from szDevice), compare
    // against [hardware] in the ini. A mismatch means the user swapped
    // hardware (or our database grew a new match). Re-run the match and
    // update ini.
    //
    // Guard: skip until the set has been populated by InitGpuSet. We use
    // g_gpus[0].active as the signal — g_gpuCount==0 means we never built
    // the set (MAHM unavailable at startup), so there's nothing to compare.
    if (!g_hwFingerprintChecked && g_gpuCount > 0 && g_gpus[0].name[0]) {
        g_hwFingerprintChecked = TRUE;
        // Use the first *dGPU* (NVIDIA/AMD) for the threshold match — iGPU
        // temperatures often aren't in hardware_db.h. If none of the GPUs
        // are a dGPU, fall back to the first GPU in the set.
        const TCHAR* matchName = g_gpus[0].name;
        UINT matchVendor = 0;
        for (int i = 0; i < g_gpuCount; i++) {
            // szGpuId starts with "VEN_XXXX" — extract the vendor nibbles.
            UINT ven = 0;
            if (_sntscanf_s(g_gpus[i].gpuId, _countof(g_gpus[i].gpuId),
                            TEXT("VEN_%x"), &ven) == 1) {
                if (ven == 0x10DE /* NVIDIA */ || ven == 0x1002 /* AMD */) {
                    matchName = g_gpus[i].name;
                    matchVendor = ven;
                    break;
                }
            }
        }
        Log(TEXT("[HwDetect] threshold match name='%s' (vendor=0x%04X)\r\n"),
            matchName, matchVendor);

        TCHAR prevCpu[128] = {}, prevGpu[128] = {};
        if (LoadHardwareFingerprint(prevCpu, _countof(prevCpu),
                                    prevGpu, _countof(prevGpu))) {
            TCHAR curCpu[128] = {};
            ReadCpuNameFromRegistry(curCpu, _countof(curCpu));
            if (_tcscmp(prevCpu, curCpu) != 0 || _tcscmp(prevGpu, matchName) != 0) {
                int cpuThr = 0, gpuThr = 0;
                MatchHardwareThresholds(&cpuThr, &gpuThr);
                if (cpuThr != g_cpuThreshold || gpuThr != g_gpuThreshold) {
                    g_cpuThreshold = cpuThr;
                    g_gpuThreshold = gpuThr;
                    WriteThresholdSection(cpuThr, gpuThr);
                    Log(TEXT("[Ini] hw changed -> [threshold] cpu=%d gpu=%d\r\n"),
                        cpuThr, gpuThr);
                }
                WriteHardwareFingerprintSection(curCpu, matchName);
            }
        } else {
            // No fingerprint recorded yet but [threshold] exists — record
            // the fingerprint without changing thresholds. This handles the
            // upgrade path: existing users have [threshold] but no [hardware].
            TCHAR curCpu[128] = {};
            ReadCpuNameFromRegistry(curCpu, _countof(curCpu));
            WriteHardwareFingerprintSection(curCpu, matchName);
            Log(TEXT("[Ini] recorded [hardware] fp for existing install: cpu='%s' gpu='%s'\r\n"),
                curCpu, matchName);
        }
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Config persistence
// ---------------------------------------------------------------------------
static void GetIniPath(TCHAR* out, size_t cch)
{
    TCHAR exe[MAX_PATH] = {};
    GetModuleFileName(NULL, exe, MAX_PATH);
    TCHAR* slash = _tcsrchr(exe, TEXT('\\'));
    if (slash) slash[1] = 0;
    StringCchPrintf(out, cch, TEXT("%sviewTemp.ini"), exe);
}

// Reads the CPU brand string from HKLM\HARDWARE\DESCRIPTION\System\
// CentralProcessor\0\ProcessorNameString. Returns FALSE on any error
// (registry access denied, key missing on a non-Windows platform).
// The string is trimmed of leading/trailing whitespace.
static void ReadCpuNameFromRegistry(TCHAR* out, size_t cch)
{
    if (!out || cch == 0) return;
    out[0] = 0;
    HKEY hKey = NULL;
    LSTATUS rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE,
        TEXT("HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0"),
        0, KEY_READ, &hKey);
    if (rc != ERROR_SUCCESS || !hKey) {
        Log(TEXT("[HwDetect] RegOpenKeyEx CPU failed rc=%lu\r\n"), rc);
        return;
    }
    DWORD type = 0, cbData = (DWORD)(cch * sizeof(TCHAR));
    rc = RegQueryValueEx(hKey, TEXT("ProcessorNameString"),
                         NULL, &type, (LPBYTE)out, &cbData);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || type != REG_SZ) {
        Log(TEXT("[HwDetect] RegQueryValueEx CPU failed rc=%lu type=%lu\r\n"), rc, type);
        out[0] = 0;
        return;
    }
    // Trim leading/trailing whitespace.
    TCHAR* p = out;
    while (*p == TEXT(' ') || *p == TEXT('\t')) p++;
    if (p != out) {
        memmove(out, p, (_tcslen(p) + 1) * sizeof(TCHAR));
    }
    size_t len = _tcslen(out);
    while (len > 0 && (out[len-1] == TEXT(' ') || out[len-1] == TEXT('\t') ||
                       out[len-1] == TEXT('\r') || out[len-1] == TEXT('\n'))) {
        out[--len] = 0;
    }
    Log(TEXT("[HwDetect] CPU brand: %s\r\n"), out);
}

// Returns the warn temperature for the current CPU/GPU by looking up the
// CPU brand and the first dGPU name in hardware_db.h. If either sensor
// fails to match the database, the corresponding default (85 °C) is
// returned and the "matched" flag in the output pair is FALSE.
static BOOL MatchHardwareThresholds(int* outCpu, int* outGpu)
{
    // Ensure we have a fresh CPU name (g_cpuName is "CPU" by default and is
    // never updated from MAHM, unlike g_gpus[i].name which InitGpuSet populates).
    TCHAR cpuName[128] = {};
    ReadCpuNameFromRegistry(cpuName, _countof(cpuName));
    const TCHAR* cpuLookup = (cpuName[0] ? cpuName : g_cpuName);
    // SampleMahm picks the dGPU name for the hardware_db lookup (iGPU names
    // rarely match the DB). matchName is owned by the caller (g_gpus[i].name).
    // We don't have access to g_gpus here — caller is expected to pass a
    // dGPU-derived name. Fall back to g_cpuName-suffixed placeholder if empty.
    const TCHAR* gpuLookup = TEXT("");
    for (int i = 0; i < g_gpuCount; i++) {
        if (g_gpus[i].name[0] && (g_gpus[i].gpuId[3] == TEXT('1') &&
            g_gpus[i].gpuId[4] == TEXT('0') && g_gpus[i].gpuId[5] == TEXT('D') &&
            g_gpus[i].gpuId[6] == TEXT('E') /* 0x10DE NVIDIA */) ||
            (g_gpus[i].gpuId[3] == TEXT('1') && g_gpus[i].gpuId[4] == TEXT('0') &&
             g_gpus[i].gpuId[5] == TEXT('0') && g_gpus[i].gpuId[6] == TEXT('2') /* 0x1002 AMD */)) {
            gpuLookup = g_gpus[i].name;
            break;
        }
    }
    if (!gpuLookup[0] && g_gpuCount > 0) gpuLookup = g_gpus[0].name;

    int cpuWarn = 85, gpuWarn = 85;
    BOOL cpuHit = LookupHardwareThreshold(kCpuDb, _countof(kCpuDb),
                                          cpuLookup, &cpuWarn);
    BOOL gpuHit = LookupHardwareThreshold(kGpuDb, _countof(kGpuDb),
                                          gpuLookup, &gpuWarn);
    if (outCpu) *outCpu = cpuWarn;
    if (outGpu) *outGpu = gpuWarn;
    Log(TEXT("[HwDetect] match cpu='%s' -> %d (%s), gpu='%s' -> %d (%s)\r\n"),
        cpuLookup, cpuWarn, cpuHit ? TEXT("DB") : TEXT("default"),
        gpuLookup, gpuWarn, gpuHit ? TEXT("DB") : TEXT("default"));
    return cpuHit || gpuHit;
}

// Writes the [threshold] section to MdViewer.ini. Called both from
// LoadConfigFromIni (to seed on first run) and from the context-menu
// "auto-match current hardware thresholds" item (to force overwrite).
static void WriteThresholdSection(int cpuThr, int gpuThr)
{
    TCHAR path[MAX_PATH]; GetIniPath(path, MAX_PATH);
    TCHAR buf[16];
    StringCchPrintf(buf, _countof(buf), TEXT("%d"), cpuThr);
    WritePrivateProfileString(TEXT("threshold"), TEXT("Cpu"), buf, path);
    StringCchPrintf(buf, _countof(buf), TEXT("%d"), gpuThr);
    WritePrivateProfileString(TEXT("threshold"), TEXT("Gpu"), buf, path);
}

// Writes the [hardware] section with the CPU+GPU model strings we last
// successfully matched against. On startup we read this and compare against
// the current hardware — a mismatch means the user swapped hardware (or our
// database got new entries that hit a previously-unmatched model), so we
// re-run the match and update [threshold] accordingly.
static void WriteHardwareFingerprintSection(const TCHAR* cpuName, const TCHAR* gpuName)
{
    TCHAR path[MAX_PATH]; GetIniPath(path, MAX_PATH);
    WritePrivateProfileString(TEXT("hardware"), TEXT("CpuName"),
                              cpuName ? cpuName : TEXT(""), path);
    WritePrivateProfileString(TEXT("hardware"), TEXT("GpuName"),
                              gpuName ? gpuName : TEXT(""), path);
}

static BOOL LoadHardwareFingerprint(TCHAR* cpuOut, size_t cpuCch,
                                    TCHAR* gpuOut, size_t gpuCch)
{
    TCHAR path[MAX_PATH]; GetIniPath(path, MAX_PATH);
    // GetPrivateProfileString returns 0 if the section/key is missing.
    DWORD gotCpu = GetPrivateProfileString(TEXT("hardware"), TEXT("CpuName"),
                                           TEXT(""), cpuOut, (DWORD)cpuCch, path);
    DWORD gotGpu = GetPrivateProfileString(TEXT("hardware"), TEXT("GpuName"),
                                           TEXT(""), gpuOut, (DWORD)gpuCch, path);
    return (gotCpu > 0 && gotGpu > 0);
}

// Build GpuEntry set from MAHM. Two-pass scan:
//   pass 1: count active GPUs (szGpuId[0] != 0) so we can size g_gpus[].
//   pass 2: populate each node — name from szDevice, gpuId from szGpuId,
//           gpuIdSafe from gpuId with '&' '=' replaced by '_' (ini key).
//
// Called once from WM_CREATE after MapMahm. Safe to call when MAHM isn't
// mapped (then g_numEntries/g_gpuEntryCount are 0 and the function does
// nothing — g_gpus stays NULL, g_gpuCount stays 0).
static void InitGpuSet()
{
    delete[] g_gpus; g_gpus = NULL; g_gpuCount = 0;

    if (!g_pMahm || g_gpuEntryCount == 0) {
        Log(TEXT("[GpuSet] no MAHM GPU entries\r\n"));
        return;
    }

    auto* h = (MAHM_SHARED_MEMORY_HEADER*)g_pMahm;
    BYTE* gpuBase = (BYTE*)g_pMahm + sizeof(MAHM_SHARED_MEMORY_HEADER)
                    + h->dwNumEntries * h->dwEntrySize;

    // Pass 1: count.
    int active = 0;
    for (DWORD i = 0; i < g_gpuEntryCount; i++) {
        auto* e = (MAHM_SHARED_MEMORY_GPU_ENTRY*)(gpuBase + i * h->dwGpuEntrySize);
        if (e->szGpuId[0] != 0) active++;
    }
    if (active == 0) {
        Log(TEXT("[GpuSet] 0 active GPUs in MAHM\r\n"));
        return;
    }
    g_gpuCount = active;
    g_gpus = new GpuEntry[active];
    ZeroMemory(g_gpus, sizeof(GpuEntry) * active);

    // Pass 2: populate.
    int slot = 0;
    for (DWORD i = 0; i < g_gpuEntryCount && slot < active; i++) {
        auto* e = (MAHM_SHARED_MEMORY_GPU_ENTRY*)(gpuBase + i * h->dwGpuEntrySize);
        if (e->szGpuId[0] == 0) continue;
        GpuEntry* entry = &g_gpus[slot];
        entry->active = TRUE;
        entry->mahmIndex = (int)i;

        // Friendly name: prefer szDevice, fallback to szFamily, then szGpuId.
        const char* nm = e->szDevice[0] ? e->szDevice
                       : e->szFamily[0] ? e->szFamily
                       : e->szGpuId;
        MultiByteToWideChar(CP_ACP, 0, nm, -1, entry->name, _countof(entry->name));
        entry->name[_countof(entry->name) - 1] = 0;

        // szDevice verbatim (for log / hardware fingerprint).
        MultiByteToWideChar(CP_ACP, 0, e->szDevice, -1,
                            entry->device, _countof(entry->device));
        entry->device[_countof(entry->device) - 1] = 0;

        // szGpuId verbatim (PCI path).
        MultiByteToWideChar(CP_ACP, 0, e->szGpuId, -1,
                            entry->gpuId, _countof(entry->gpuId));
        entry->gpuId[_countof(entry->gpuId) - 1] = 0;

        // gpuIdSafe = gpuId with '&' '=' replaced by '_' (ini key chars).
        BuildGpuIdSafe(e->szGpuId, entry->gpuIdSafe, _countof(entry->gpuIdSafe));

        // Defaults — refreshed later by LoadConfigFromIni if a saved value
        // exists under [view] ShowGpu_<gpuIdSafe>=.
        entry->showInPanel = TRUE;
        entry->vramTotalGB = 0.0;
        entry->vramFromDxgi = FALSE;

        Log(TEXT("[GpuSet] gpu[%d] mahmIdx=%lu name='%s' id='%s' safe='%s'\r\n"),
            slot, (unsigned long)i, entry->name, entry->gpuId, entry->gpuIdSafe);
        slot++;
    }

    // Per-GPU showInPanel from ini (after the set exists — LoadConfigFromIni
    // runs in wWinMain BEFORE CreateWindowEx, so g_gpus is NULL at that
    // point). Defaults: TRUE (key missing = show).
    LoadGpuShowFromIni();
}

// Copy src (ASCII PCI path like "VEN_10DE&DEV_28A0&...") into dst, replacing
// '&' and '=' with '_'. ini keys can't contain '&' or '=' safely — see
// notes/continue.md "menu ID vs ini key separation" rationale.
static void BuildGpuIdSafe(const char* src, TCHAR* dst, size_t cch)
{
    if (!dst || cch == 0) return;
    if (!src) { dst[0] = 0; return; }
    for (size_t k = 0; k + 1 < cch; k++) {
        char c = src[k];
        if (c == 0) { dst[k] = 0; return; }
        if (c == '&' || c == '=') c = '_';
        dst[k] = (TCHAR)(unsigned char)c;
    }
    dst[cch - 1] = 0;
}

// Enumerate DXGI adapters. Two-pass scan:
//   pass 1: count via EnumAdapters loop
//   pass 2: pull Description/VendorId/DedicatedVideoMemory for each
// Then match each MAHM GPU entry (g_gpus[i]) to a DXGI adapter by parsing
// the VEN_xxxx prefix in gpuId[] and comparing against adapter.VendorId.
// When matched, store adapter->DedicatedVideoMemory into g_gpus[i].vramTotalGB
// (in GB).
//
// We use VendorId as the primary key because two GPUs from the same vendor
// (e.g. dual NVIDIA) are rare in the consumer laptop / desktop segments
// viewTemp targets. If 老林 later hits a same-vendor multi-GPU scenario the
// fallback is "first match wins" — good enough until we hit the problem.
//
// Note: DXGI also reports WDDM virtual adapters (e.g. Microsoft Basic Render
// Driver, "virtual" displays). These have VendorId=0x1414 (Microsoft) and
// DedicatedVideoMemory=0 — the matching logic naturally ignores them because
// g_gpus[] has no entry with VEN_1414.
static void InitDxgiAdapterMap()
{
    delete[] g_dxgi; g_dxgi = NULL; g_dxgiCount = 0;

    IDXGIFactory* factory = NULL;
    HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory),
                                   reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        Log(TEXT("[Dxgi] CreateDXGIFactory FAILED hr=0x%08lx\r\n"), hr);
        return;
    }

    // Pass 1: count.
    int n = 0;
    for (UINT i = 0; ; i++) {
        IDXGIAdapter* probe = NULL;
        if (factory->EnumAdapters(i, &probe) == DXGI_ERROR_NOT_FOUND) break;
        if (probe) probe->Release();
        n++;
    }
    if (n == 0) {
        Log(TEXT("[Dxgi] EnumAdapters returned 0 adapters\r\n"));
        factory->Release();
        return;
    }
    g_dxgiCount = n;
    g_dxgi = new DxgiAdapterInfo[n];
    ZeroMemory(g_dxgi, sizeof(DxgiAdapterInfo) * n);

    // Pass 2: populate.
    int kept = 0;
    for (UINT i = 0; i < (UINT)n; i++) {
        IDXGIAdapter* ad = NULL;
        if (FAILED(factory->EnumAdapters(i, &ad)) || !ad) continue;
        DXGI_ADAPTER_DESC desc = {};
        if (SUCCEEDED(ad->GetDesc(&desc))) {
            DxgiAdapterInfo* a = &g_dxgi[kept];
            a->vendorId = desc.VendorId;
            a->dedicatedBytes = desc.DedicatedVideoMemory;
            // Description is WCHAR; we copied into a TCHAR buffer for simplicity.
            _tcsncpy_s(a->description, _countof(a->description),
                       desc.Description, _TRUNCATE);
            double gb = (double)desc.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0);
            Log(TEXT("[Dxgi] adapter[%d] desc='%ls' vendor=0x%04X vram=%.2fGB (%llu bytes)\r\n"),
                kept, a->description, a->vendorId, gb,
                (unsigned long long)desc.DedicatedVideoMemory);
            kept++;
        }
        ad->Release();
    }
    factory->Release();

    if (kept < g_dxgiCount) g_dxgiCount = kept;  // tighten if some GetDesc failed

    // Match each MAHM GPU entry to a DXGI adapter by VendorId prefix.
    for (int i = 0; i < g_gpuCount; i++) {
        GpuEntry* entry = &g_gpus[i];
        // Parse "VEN_xxxx" from gpuId (ASCII in TCHAR buffer).
        UINT ven = 0;
        if (_sntscanf_s(entry->gpuId, _countof(entry->gpuId), TEXT("VEN_%x"), &ven) != 1) {
            Log(TEXT("[DxgiMatch] gpu[%d] '%s' has no VEN_ prefix, skip match\r\n"),
                i, entry->name);
            continue;
        }
        for (int j = 0; j < g_dxgiCount; j++) {
            if (g_dxgi[j].vendorId == ven && g_dxgi[j].dedicatedBytes > 0) {
                entry->vramTotalGB = (double)g_dxgi[j].dedicatedBytes
                                 / (1024.0 * 1024.0 * 1024.0);
                entry->vramFromDxgi = TRUE;
                Log(TEXT("[DxgiMatch] gpu[%d] '%s' VEN=0x%04X -> adapter[%d] vram=%.2fGB\r\n"),
                    i, entry->name, ven, j, entry->vramTotalGB);
                break;
            }
        }
        if (!entry->vramFromDxgi) {
            Log(TEXT("[DxgiMatch] gpu[%d] '%s' VEN=0x%04X: no DXGI adapter match (vram=0)\r\n"),
                i, entry->name, ven);
        }
    }
}

// Per-GPU showInPanel hot-reload from ini. Called from InitGpuSet AFTER the
// set is built — at LoadConfigFromIni time g_gpus is still NULL.
//
// Each g_gpus[i].showInPanel is loaded under the stable PCI-path-derived key
// ShowGpu_<gpuIdSafe>= in the [view] section. Missing keys default to TRUE
// (first run). This function does NOT write back — users can edit ini and
// the change is reflected within ≤1 s via the menu handler that calls
// WritePrivateProfileString directly. No hot-reload here on purpose: if a
// user edits the file we want the toggle to be a deliberate menu action,
// not a silent overwrite of their current toggle state on every sample tick.
static void LoadGpuShowFromIni()
{
    if (!g_gpus || g_gpuCount == 0) return;
    TCHAR iniPath[MAX_PATH]; GetIniPath(iniPath, MAX_PATH);
    for (int i = 0; i < g_gpuCount; i++) {
        TCHAR key[200];
        _sntprintf_s(key, _countof(key), _TRUNCATE,
                     TEXT("ShowGpu_%s"), g_gpus[i].gpuIdSafe);
        int v = GetPrivateProfileInt(TEXT("view"), key, 1, iniPath);
        g_gpus[i].showInPanel = (v != 0);
        Log(TEXT("[Ini] gpu[%d] '%s' showInPanel=%d (key=%s)\r\n"),
            i, g_gpus[i].name, (int)g_gpus[i].showInPanel, key);
    }
}

static void LoadConfigFromIni()
{
    TCHAR path[MAX_PATH]; GetIniPath(path, MAX_PATH);
    g_cfg.fontSize      = GetPrivateProfileInt(TEXT("view"), TEXT("FontSize"),   20, path);
    g_cfg.fgColor       = (COLORREF)GetPrivateProfileInt(TEXT("view"), TEXT("FgColor"),  (int)RGB(255,255,0), path);
    g_cfg.opacityPct    = GetPrivateProfileInt(TEXT("view"), TEXT("Opacity"),    100, path);
    g_cfg.topMost       = GetPrivateProfileInt(TEXT("view"), TEXT("TopMost"),    1, path);
    g_cfg.showCpu       = GetPrivateProfileInt(TEXT("view"), TEXT("ShowCpu"),    1, path);
    g_cfg.showGpu       = GetPrivateProfileInt(TEXT("view"), TEXT("ShowGpu"),    1, path);  // legacy master toggle
    g_cfg.showUsage     = GetPrivateProfileInt(TEXT("view"), TEXT("ShowUsage"),  0, path);
    g_cfg.showVram      = GetPrivateProfileInt(TEXT("view"), TEXT("ShowVram"),   1, path);
    g_cfg.showRam       = GetPrivateProfileInt(TEXT("view"), TEXT("ShowRam"),    1, path);
    g_cfg.transparentBg = GetPrivateProfileInt(TEXT("view"), TEXT("Transparent"), 1, path);

    g_cpuThreshold     = GetPrivateProfileInt(TEXT("threshold"), TEXT("Cpu"), 85, path);
    g_gpuThreshold     = GetPrivateProfileInt(TEXT("threshold"), TEXT("Gpu"), 85, path);
    if (g_cpuThreshold < 0)   g_cpuThreshold = 0;
    if (g_gpuThreshold < 0)   g_gpuThreshold = 0;
    if (g_cpuThreshold > 150) g_cpuThreshold = 150;
    if (g_gpuThreshold > 150) g_gpuThreshold = 150;
    Log(TEXT("[Ini] thresholds cpu=%d gpu=%d\r\n"), g_cpuThreshold, g_gpuThreshold);

    // NOTE: per-GPU ShowGpu_<gpuIdSafe> is loaded in LoadGpuShowFromIni(),
    // called from InitGpuSet() at WM_CREATE time — g_gpus doesn't exist yet
    // here. See that function for rationale.

    // Startup hardware-threshold logic — phase 1 (ini read):
    //   - If [threshold] section is missing entirely (fresh install), seed
    //     it via hardware match. We can't check [hardware] fingerprint yet
    //     because SampleMahm hasn't run to populate g_gpus[0].name — that
    //     check happens in phase 2 below.
    {
        TCHAR sectBuf[8] = {};
        DWORD sectLen = GetPrivateProfileSection(TEXT("threshold"), sectBuf,
                                                 _countof(sectBuf), path);
        if (sectLen == 0) {
            TCHAR curCpu[128] = {};
            ReadCpuNameFromRegistry(curCpu, _countof(curCpu));
            int cpuThr = 0, gpuThr = 0;
            MatchHardwareThresholds(&cpuThr, &gpuThr);
            g_cpuThreshold = cpuThr;
            g_gpuThreshold = gpuThr;
            WriteThresholdSection(cpuThr, gpuThr);
            // matchName picked by SampleMahm's phase 2 will be a dGPU name
            // when available; record the CPU and a placeholder here so the
            // next SampleMahm pass can compare and overwrite if needed.
            TCHAR placeholderGpu[64] = TEXT("(pending first sample)");
            WriteHardwareFingerprintSection(curCpu, placeholderGpu);
            Log(TEXT("[Ini] seeded [threshold] cpu=%d gpu=%d, [hardware] placeholder fp recorded\r\n"),
                cpuThr, gpuThr);
        }
    }

    g_cfg.fgAlpha   = (BYTE)(g_cfg.opacityPct * 255 / 100);
    g_cfg.bgAlpha   = (BYTE)(g_cfg.opacityPct * 220 / 100);
    g_cfg.fontSize  = ClampInt(g_cfg.fontSize, 8, 96);
    g_cfg.opacityPct= ClampInt(g_cfg.opacityPct, 20, 100);
}

static void SaveConfigToIni()
{
    TCHAR path[MAX_PATH]; GetIniPath(path, MAX_PATH);
    TCHAR buf[64];
    StringCchPrintf(buf, _countof(buf), TEXT("%d"), g_cfg.fontSize);
    WritePrivateProfileString(TEXT("view"), TEXT("FontSize"), buf, path);
    StringCchPrintf(buf, _countof(buf), TEXT("%d"), (int)g_cfg.fgColor);
    WritePrivateProfileString(TEXT("view"), TEXT("FgColor"), buf, path);
    StringCchPrintf(buf, _countof(buf), TEXT("%d"), g_cfg.opacityPct);
    WritePrivateProfileString(TEXT("view"), TEXT("Opacity"), buf, path);
    StringCchPrintf(buf, _countof(buf), TEXT("%d"), g_cfg.topMost ? 1 : 0);
    WritePrivateProfileString(TEXT("view"), TEXT("TopMost"), buf, path);
    StringCchPrintf(buf, _countof(buf), TEXT("%d"), g_cfg.showCpu ? 1 : 0);
    WritePrivateProfileString(TEXT("view"), TEXT("ShowCpu"), buf, path);
    StringCchPrintf(buf, _countof(buf), TEXT("%d"), g_cfg.showGpu ? 1 : 0);  // legacy master
    WritePrivateProfileString(TEXT("view"), TEXT("ShowGpu"), buf, path);
    StringCchPrintf(buf, _countof(buf), TEXT("%d"), g_cfg.showUsage ? 1 : 0);
    WritePrivateProfileString(TEXT("view"), TEXT("ShowUsage"), buf, path);
    StringCchPrintf(buf, _countof(buf), TEXT("%d"), g_cfg.showVram ? 1 : 0);
    WritePrivateProfileString(TEXT("view"), TEXT("ShowVram"), buf, path);
    StringCchPrintf(buf, _countof(buf), TEXT("%d"), g_cfg.showRam ? 1 : 0);
    WritePrivateProfileString(TEXT("view"), TEXT("ShowRam"), buf, path);
    StringCchPrintf(buf, _countof(buf), TEXT("%d"), g_cfg.transparentBg ? 1 : 0);
    WritePrivateProfileString(TEXT("view"), TEXT("Transparent"), buf, path);

    StringCchPrintf(buf, _countof(buf), TEXT("%d"), g_cpuThreshold);
    WritePrivateProfileString(TEXT("threshold"), TEXT("Cpu"), buf, path);
    StringCchPrintf(buf, _countof(buf), TEXT("%d"), g_gpuThreshold);
    WritePrivateProfileString(TEXT("threshold"), TEXT("Gpu"), buf, path);

    // Per-GPU showInPanel under stable PCI-path-derived keys. We DON'T write
    // these here — they're written immediately on each toggle (in
    // ShowContextMenu) so "保存配置" doesn't accidentally overwrite a fresh
    // unsaved toggle the user just made.
}
