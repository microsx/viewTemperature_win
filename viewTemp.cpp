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

// Cached strings (formatted once per sample tick).
static TCHAR   g_cpuTempStr[32]   = TEXT("--");
static TCHAR   g_gpuTempStr[32]   = TEXT("--");
static TCHAR   g_cpuUsageStr[32]  = TEXT("--");
static TCHAR   g_gpuUsageStr[32]  = TEXT("--");
static TCHAR   g_vramStr[32]      = TEXT("--");     // "X.XG / YY.YG (NN%)"
static TCHAR   g_ramStr[32]       = TEXT("--");     // "X.XG / YY.YG (NN%)"
static TCHAR   g_gpuName[64]      = TEXT("GPU");
static TCHAR   g_cpuName[64]      = TEXT("CPU");
static BOOL    g_mahmAvailable    = FALSE;

// Temperature thresholds (°C). Read from [threshold] section of MdViewer.ini.
// While a sensor's last reported temperature exceeds its threshold, that row's
// text alternates between fgColor and warning red on a 360 ms tick.
static int      g_cpuThreshold     = 85;
static int      g_gpuThreshold     = 85;

// Flash state machine. Toggled by kTimerFlash (every 360 ms). When FALSE the
// row draws in fgColor; when TRUE the row draws in warning red. The state is
// pushed/popped by SampleMahm based on whether the latest reading crosses the
// threshold — hysteresis-free, but the 1 s SampleMahm cadence already filters
// out sub-second jitter.
static BOOL     g_flashCpuOn       = FALSE;
static BOOL     g_flashGpuOn       = FALSE;
static BOOL     g_flashTickOdd     = FALSE;  // toggled each kTimerFlash fire
static BOOL     g_hwFingerprintChecked = FALSE;  // one-shot after first SampleMahm

// VRAM total in bytes, fetched once at startup from DXGI (IDXGIAdapter::GetDesc).
// Used to fill in the "total" half of the "VRAM: X / Y (Z%)" display when MAHM
// can't supply it. 0 means "DXGI failed or returned 0" — caller falls back to
// the original MAHM-only display.
static UINT64   g_dxgiVramBytes    = 0;

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
static void  Dxgi_InitVramTotal();
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
        Dxgi_InitVramTotal();  // one-shot VRAM total via DXGI
        MapMahm();
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
    AppendMenu(hm, MF_STRING | (g_cfg.showGpu     ? MF_CHECKED : 0), 1003, LANG_IF("显示 GPU", "Show GPU"));
    AppendMenu(hm, MF_STRING | (g_cfg.showUsage   ? MF_CHECKED : 0), 1004, LANG_IF("显示使用率", "Show Usage"));
    AppendMenu(hm, MF_STRING | (g_cfg.showVram    ? MF_CHECKED : 0), 1005, LANG_IF("显示 VRAM", "Show VRAM"));
    AppendMenu(hm, MF_STRING | (g_cfg.showRam     ? MF_CHECKED : 0), 1006, LANG_IF("显示 RAM", "Show RAM"));
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
    case 1003: g_cfg.showGpu = !g_cfg.showGpu; break;
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
        TCHAR curCpu[128] = {};
        ReadCpuNameFromRegistry(curCpu, _countof(curCpu));
        WriteHardwareFingerprintSection(curCpu, g_gpuName);
        Log(TEXT("[Menu] auto-match thresholds -> cpu=%d gpu=%d\r\n"),
            cpuThr, gpuThr);
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
    if (g_cfg.showGpu) {
        StringCchPrintfW(lineBuf, 256, L"%s: %s", g_gpuName, g_gpuTempStr);
        int w = measure(lineBuf);
        if (w > widthPx) widthPx = w;
        heightPx += lh;
    }
    if (g_cfg.showVram) {
        StringCchPrintfW(lineBuf, 256, L"VRAM: %s", g_vramStr);
        int w = measure(lineBuf);
        if (w > widthPx) widthPx = w;
        heightPx += lh;
    }
    if (g_cfg.showRam) {
        StringCchPrintfW(lineBuf, 256, L"RAM: %s", g_ramStr);
        int w = measure(lineBuf);
        if (w > widthPx) widthPx = w;
        heightPx += lh;
    }
    if (g_cfg.showUsage) {
        StringCchPrintfW(lineBuf, 256, L"CPU %s / GPU %s", g_cpuUsageStr, g_gpuUsageStr);
        int w = measure(lineBuf);
        if (w > widthPx) widthPx = w;
        heightPx += lh;
    }
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
    if (g_cfg.showGpu) {
        StringCchPrintfW(lineBuf, 256, L"%s: %s", g_gpuName, g_gpuTempStr);
        g.DrawString(lineBuf, -1, &font, Gdiplus::PointF(10, (Gdiplus::REAL)yy), tempBrush(g_flashGpuOn));
        yy += lh;
    }
    if (g_cfg.showVram) {
        StringCchPrintfW(lineBuf, 256, L"VRAM: %s", g_vramStr);
        g.DrawString(lineBuf, -1, &font, Gdiplus::PointF(10, (Gdiplus::REAL)yy), &fg);
        yy += lh;
    }
    if (g_cfg.showRam) {
        StringCchPrintfW(lineBuf, 256, L"RAM: %s", g_ramStr);
        g.DrawString(lineBuf, -1, &font, Gdiplus::PointF(10, (Gdiplus::REAL)yy), &fg);
        yy += lh;
    }
    if (g_cfg.showUsage) {
        StringCchPrintfW(lineBuf, 256, L"CPU %s / GPU %s", g_cpuUsageStr, g_gpuUsageStr);
        g.DrawString(lineBuf, -1, &font, Gdiplus::PointF(10, (Gdiplus::REAL)yy), &fg);
        yy += lh;
    }

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
static BOOL GetGpuVramTotalKb(DWORD gpuIndex, DWORD* outKb)
{
    if (!g_pMahm || !outKb || gpuIndex >= g_gpuEntryCount) return FALSE;
    auto* h = (MAHM_SHARED_MEMORY_HEADER*)g_pMahm;
    // GPU entries follow the main entry array.
    BYTE* gpuBase = (BYTE*)g_pMahm + sizeof(MAHM_SHARED_MEMORY_HEADER)
                    + h->dwNumEntries * h->dwEntrySize;
    auto* g = (MAHM_SHARED_MEMORY_GPU_ENTRY*)(gpuBase + gpuIndex * h->dwGpuEntrySize);
    if (g->dwMemAmount == 0) return FALSE;
    *outKb = g->dwMemAmount;
    return TRUE;
}

// Get GPU device display name (e.g. "GeForce RTX 4090") if known.
static void GetGpuName(DWORD gpuIndex, TCHAR* out, size_t cch)
{
    if (!g_pMahm || gpuIndex >= g_gpuEntryCount || cch == 0) return;
    auto* h = (MAHM_SHARED_MEMORY_HEADER*)g_pMahm;
    BYTE* gpuBase = (BYTE*)g_pMahm + sizeof(MAHM_SHARED_MEMORY_HEADER)
                    + h->dwNumEntries * h->dwEntrySize;
    auto* g = (MAHM_SHARED_MEMORY_GPU_ENTRY*)(gpuBase + gpuIndex * h->dwGpuEntrySize);
    if (g->szDevice[0]) {
        // szDevice is char (multibyte). Convert to TCHAR (UTF-16) safely.
        MultiByteToWideChar(CP_ACP, 0, g->szDevice, -1, out, (int)cch);
        out[cch - 1] = 0;
    }
}

static BOOL SampleMahm()
{
    if (!g_pMahm && !MapMahm()) {
        StringCchCopy(g_cpuTempStr, _countof(g_cpuTempStr), TEXT("N/A"));
        StringCchCopy(g_gpuTempStr, _countof(g_gpuTempStr), TEXT("N/A"));
        StringCchCopy(g_vramStr,    _countof(g_vramStr),    TEXT("N/A"));
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
            StringCchCopy(g_gpuTempStr, _countof(g_gpuTempStr), TEXT("--"));
            StringCchCopy(g_vramStr,    _countof(g_vramStr),    TEXT("--"));
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
        // Some setups leave CPU temperature blank; fall back to first non-GPU entry
        // that reports a CPU temperature id (still global).
        StringCchCopy(g_cpuTempStr, _countof(g_cpuTempStr), TEXT("--"));
    } else {
        StringCchPrintfW(g_cpuTempStr, _countof(g_cpuTempStr), L"%.0f°C", cpuT);
    }
    float cpuU = GetSource(MAHM_GPU_INDEX_GLOBAL, MAHM_SRC_CPU_USAGE);
    if (cpuU == FLT_MAX) StringCchCopy(g_cpuUsageStr, _countof(g_cpuUsageStr), TEXT("--"));
    else                 StringCchPrintfW(g_cpuUsageStr, _countof(g_cpuUsageStr), L"%.0f%%", cpuU);

    // ---- GPU 0 ----
    float gpuT = GetSource(0, MAHM_SRC_GPU_TEMPERATURE);
    if (gpuT == FLT_MAX) StringCchCopy(g_gpuTempStr, _countof(g_gpuTempStr), TEXT("--"));
    else                 StringCchPrintfW(g_gpuTempStr, _countof(g_gpuTempStr), L"%.0f°C", gpuT);

    float gpuU = GetSource(0, MAHM_SRC_GPU_USAGE);
    if (gpuU == FLT_MAX) StringCchCopy(g_gpuUsageStr, _countof(g_gpuUsageStr), TEXT("--"));
    else                 StringCchPrintfW(g_gpuUsageStr, _countof(g_gpuUsageStr), L"%.0f%%", gpuU);

    // ---- VRAM ----
    // VRAM: only MAHM is available for percentage. NVAPI was removed because
    // NVIDIA's modern nvapi64.dll is COM-only (no flat exports — see NvApi.h
    // stub and continue.md "坑 2"). For the total, DXGI's
    // IDXGIAdapter::GetDesc was queried once at startup (Dxgi_InitVramTotal)
    // and cached in g_dxgiVramBytes — this works for AMD/Intel/NVIDIA alike.
    {
        float vramPct = GetSource(0, MAHM_SRC_MEMORY_USAGE);
        Log(TEXT("[Vram] MAHM raw pct=%.4f (interpreted percent*100), DXGI total=%llu bytes\r\n"),
            vramPct, (unsigned long long)g_dxgiVramBytes);
        if (vramPct == FLT_MAX) {
            StringCchCopy(g_vramStr, _countof(g_vramStr), TEXT("--"));
        } else {
            double pct    = (double)vramPct / 100.0;  // raw is "percent*100"
            double totalG = (double)g_dxgiVramBytes / (1024.0 * 1024.0 * 1024.0);
            double usedG  = totalG * pct / 100.0;
            StringCchPrintfW(g_vramStr, _countof(g_vramStr),
                              L"%.2fG / %.2fG (%.0f%%)", usedG, totalG, pct);
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

    // ---- Names ----
    if (g_gpuName[0] == TEXT('G') && g_gpuName[1] == TEXT('P') && g_gpuName[2] == TEXT('U'))
        GetGpuName(0, g_gpuName, _countof(g_gpuName));

    g_mahmAvailable = TRUE;
    Log(TEXT("[Sample-MAHM] CPU=%s GPU=%s VRAM=%s RAM=%s cpuU=%s gpuU=%s name=%s nvapi=stubbed\r\n"),
        g_cpuTempStr, g_gpuTempStr, g_vramStr, g_ramStr, g_cpuUsageStr, g_gpuUsageStr, g_gpuName);

    // ---- Threshold flash state machine ----
    // Push/pop each row's flash flag based on the latest cached temperature
    // string. We parse the "NN°C" form rather than the raw float so this
    // works uniformly across MAHM and NVAPI fallback paths. "--"/"N/A" are
    // treated as "no reading" and clear the flash state.
    {
        auto parseCelsius = [](const TCHAR* s) -> float {
            if (!s || !*s || s[0] == TEXT('-') || s[0] == TEXT('N')) return -FLT_MAX;
            return (float)_tcstod(s, nullptr);
        };
        float cpuT = parseCelsius(g_cpuTempStr);
        float gpuT = parseCelsius(g_gpuTempStr);
        BOOL  cpuHot = (cpuT > (float)g_cpuThreshold);
        BOOL  gpuHot = (gpuT > (float)g_gpuThreshold);
        if (cpuHot != g_flashCpuOn || gpuHot != g_flashGpuOn) {
            Log(TEXT("[Threshold] cpuT=%.1f cpuThr=%d -> flashCpu=%d  gpuT=%.1f gpuThr=%d -> flashGpu=%d\r\n"),
                cpuT, g_cpuThreshold, (int)cpuHot, gpuT, g_gpuThreshold, (int)gpuHot);
            g_flashCpuOn = cpuHot;
            g_flashGpuOn = gpuHot;
        }

        // Hot-reload thresholds from ini each tick so users can edit
        // MdViewer.ini and see the change within ≤1 s without restarting.
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

    Log(TEXT("[Sample-Final] CPU=%s GPU=%s VRAM=%s\r\n"),
        g_cpuTempStr, g_gpuTempStr, g_vramStr);

    // Hardware-fingerprint phase 2: now that g_gpuName has been refreshed
    // by SampleMahm, compare against [hardware] in the ini. A mismatch means
    // the user swapped hardware (or our database grew a new match for a
    // previously-unmatched model). Re-run the match and update ini.
    // Guard: skip until g_gpuName has been replaced from the "GPU" default.
    if (!g_hwFingerprintChecked && g_gpuName[0]
        && !(g_gpuName[0] == TEXT('G') && g_gpuName[1] == TEXT('P')
             && g_gpuName[2] == TEXT('U') && g_gpuName[3] == 0)) {
        g_hwFingerprintChecked = TRUE;
        TCHAR prevCpu[128] = {}, prevGpu[128] = {};
        if (LoadHardwareFingerprint(prevCpu, _countof(prevCpu),
                                    prevGpu, _countof(prevGpu))) {
            TCHAR curCpu[128] = {};
            ReadCpuNameFromRegistry(curCpu, _countof(curCpu));
            if (_tcscmp(prevCpu, curCpu) != 0 || _tcscmp(prevGpu, g_gpuName) != 0) {
                int cpuThr = 0, gpuThr = 0;
                MatchHardwareThresholds(&cpuThr, &gpuThr);
                if (cpuThr != g_cpuThreshold || gpuThr != g_gpuThreshold) {
                    g_cpuThreshold = cpuThr;
                    g_gpuThreshold = gpuThr;
                    WriteThresholdSection(cpuThr, gpuThr);
                    Log(TEXT("[Ini] hw changed -> [threshold] cpu=%d gpu=%d\r\n"),
                        cpuThr, gpuThr);
                }
                WriteHardwareFingerprintSection(curCpu, g_gpuName);
            }
        } else {
            // No fingerprint recorded yet but [threshold] exists — record
            // the fingerprint without changing thresholds. This handles the
            // upgrade path: existing users have [threshold] but no [hardware].
            TCHAR curCpu[128] = {};
            ReadCpuNameFromRegistry(curCpu, _countof(curCpu));
            WriteHardwareFingerprintSection(curCpu, g_gpuName);
            Log(TEXT("[Ini] recorded [hardware] fp for existing install: cpu='%s' gpu='%s'\r\n"),
                curCpu, g_gpuName);
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
// g_cpuName / g_gpuName in hardware_db.h. If either sensor fails to match
// the database, the corresponding default (85 °C) is returned and the
// "matched" flag in the output pair is FALSE.
static BOOL MatchHardwareThresholds(int* outCpu, int* outGpu)
{
    // Ensure we have a fresh CPU name (g_cpuName is "CPU" by default and is
    // never updated from MAHM, unlike g_gpuName which SampleMahm refreshes).
    TCHAR cpuName[128] = {};
    ReadCpuNameFromRegistry(cpuName, _countof(cpuName));
    const TCHAR* cpuLookup = (cpuName[0] ? cpuName : g_cpuName);
    const TCHAR* gpuLookup = g_gpuName;

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

// Read the GPU's DedicatedVideoMemory from DXGI (IDXGIAdapter::GetDesc) and
// stash it in g_dxgiVramBytes. Called once at startup, silent on failure.
// MAHM can't supply VRAM total in current RivaTuner driver builds (see坑 2),
// and the modern NVAPI path is COM-only — DXGI is the cheapest public-API
// alternative. Works on AMD/Intel/NVIDIA without per-vendor code.
static void Dxgi_InitVramTotal()
{
    if (g_dxgiVramBytes != 0) return;  // already populated

    IDXGIFactory* factory = NULL;
    HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory),
                                   reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        Log(TEXT("[Dxgi] CreateDXGIFactory FAILED hr=0x%08lx\r\n"), hr);
        return;
    }

    // Pick adapter index 0 (primary GPU). If the user has multiple GPUs the
    // MAHM-reported name still drives which adapter we want, but for the
    // VRAM total we just need *any* dedicated GPU memory — primary is good
    // enough for the single-row display.
    IDXGIAdapter* adapter = NULL;
    hr = factory->EnumAdapters(0, &adapter);
    if (FAILED(hr) || !adapter) {
        Log(TEXT("[Dxgi] EnumAdapters(0) FAILED hr=0x%08lx\r\n"), hr);
        factory->Release();
        return;
    }

    DXGI_ADAPTER_DESC desc = {};
    hr = adapter->GetDesc(&desc);
    if (SUCCEEDED(hr)) {
        g_dxgiVramBytes = desc.DedicatedVideoMemory;
        double gb = (double)g_dxgiVramBytes / (1024.0 * 1024.0 * 1024.0);
        Log(TEXT("[Dxgi] adapter='%ls' DedicatedVideoMemory=%.2fGB (%llu bytes)\r\n"),
            desc.Description, gb, (unsigned long long)g_dxgiVramBytes);
    } else {
        Log(TEXT("[Dxgi] GetDesc FAILED hr=0x%08lx\r\n"), hr);
    }

    adapter->Release();
    factory->Release();
}

static void LoadConfigFromIni()
{
    TCHAR path[MAX_PATH]; GetIniPath(path, MAX_PATH);
    g_cfg.fontSize      = GetPrivateProfileInt(TEXT("view"), TEXT("FontSize"),   20, path);
    g_cfg.fgColor       = (COLORREF)GetPrivateProfileInt(TEXT("view"), TEXT("FgColor"),  (int)RGB(255,255,0), path);
    g_cfg.opacityPct    = GetPrivateProfileInt(TEXT("view"), TEXT("Opacity"),    100, path);
    g_cfg.topMost       = GetPrivateProfileInt(TEXT("view"), TEXT("TopMost"),    1, path);
    g_cfg.showCpu       = GetPrivateProfileInt(TEXT("view"), TEXT("ShowCpu"),    1, path);
    g_cfg.showGpu       = GetPrivateProfileInt(TEXT("view"), TEXT("ShowGpu"),    1, path);
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

    // Startup hardware-threshold logic — phase 1 (ini read):
    //   - If [threshold] section is missing entirely (fresh install), seed
    //     it via hardware match. We can't check [hardware] fingerprint yet
    //     because SampleMahm hasn't run to populate g_gpuName — that check
    //     happens in phase 2 below.
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
            WriteHardwareFingerprintSection(curCpu, g_gpuName);
            Log(TEXT("[Ini] seeded [threshold] cpu=%d gpu=%d, [hardware] fp recorded\r\n"),
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
    StringCchPrintf(buf, _countof(buf), TEXT("%d"), g_cfg.showGpu ? 1 : 0);
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
}
