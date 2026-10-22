// MIT License
//
// MultiPing
// Copyright (c) 2026 Max Vilimpoc
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// MultiPing - pings a list of addresses, one row per address, with live stats and a 60 s graph.
// Pure Win32 (user32/gdi32) + iphlpapi for ICMP + ws2_32 for name resolution. No comctl32, no MFC.
// What the front ends share (timing, layout, statistics, the graph, the files) is in MultiPingCore.h.
// Build: see build.bat (cl /MT ... statically linked CRT).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS   // sscanf (MultiPingCore.h) and _vsnprintf (the debug log) are used on purpose
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601   // build_xp.bat overrides this with 0x0502
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <d2d1.h>      // only for interface declarations; d2d1.dll / dwrite.dll are loaded at runtime
#include <dwrite.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include "MultiPingCore.h"

using namespace mp;

// A small portable replacement for the MSVC-only secure-CRT wcscpy_s, so the same source builds
// with MSVC and with MinGW-w64 GCC. Always NUL-terminates.
static void CopyW(wchar_t* dst, size_t n, const wchar_t* src)
{
    size_t i = 0;
    for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
// Opt into themed (visual style) standard controls. This is a manifest entry, not a link dependency.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#else
// MinGW: the libraries are named on the g++ command line and the manifest is a resource (see MultiPing.rc).
#endif

// ---------------------------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------------------------

enum {
    WM_APP_ENTER = WM_APP + 1,  // lParam = edit hwnd
    WM_APP_APPLY,               // lParam = edit hwnd
    WM_APP_TAB,                 // wParam = shift pressed, lParam = edit hwnd
    WM_APP_DELETE,              // wParam = row id
};

#define TIMER_TICK        1
#define TIMER_DRAG        2   // scrolls the list while a row is dragged above or below the rows shown

// Git short revision, passed in by build.bat as /DGIT_REV="abc1234".
#ifndef GIT_REV
#define GIT_REV "unknown"
#endif

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501   // from commctrl.h, avoids including it
#endif
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

// ---------------------------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------------------------

// Shared between the UI thread and one worker. The UI owns `inflight`; the worker writes `result`
// and then sets `ready`; the UI clears `ready` when it takes the result. The worker frees the
// struct when it sees `stop`, and the UI drops its pointer at the moment it signals `stop`.
struct PingCtx {
    wchar_t       host[256];
    HANDLE        stop;       // manual-reset: worker exits
    HANDLE        go;         // auto-reset: worker sends one ping
    volatile LONG ready;      // 1 when `result` holds a fresh value
    volatile LONG result;     // microseconds (>= 0) or RES_* code
    volatile LONG seq;        // UI: sequence number of the ping most recently released with `go`
    volatile LONG resultSeq;  // worker: sequence number the current `result` answers
    bool          inflight;   // UI side: a `go` has been issued whose result is not yet taken
};

struct Row {
    int           id;
    std::wstring  addr, notes;
    HWND          grip, edit, notesEdit, cur, mn, med, avg, mx, graph, del;
    PingCtx*      ctx;
    Stats         stats;
};

// A row's windows in column order.
static void RowWindows(const Row* r, HWND ctl[NCOL])
{
    HWND all[NCOL] = { r->grip, r->edit, r->notesEdit, r->cur, r->mn, r->med, r->avg, r->mx, r->graph, r->del };
    for (int k = 0; k < NCOL; k++) ctl[k] = all[k];
}

static HWND     g_hwnd;
static HFONT    g_font, g_fontSmall, g_fontLogo, g_fontEmoji, g_fontInfo;
static HICON    g_titleIcon;      // app icon drawn beside the title where no emoji font exists (XP)

// Direct2D / DirectWrite, used only to draw the title emoji in color. Loaded on demand; when
// unavailable the emoji is drawn with GDI in monochrome instead.
static ID2D1Factory*        g_d2d;
static IDWriteFactory*      g_dw;
static ID2D1DCRenderTarget* g_rt;
static IDWriteTextFormat*   g_emojiFmt;
static bool                 g_d2dTried;
static bool InitD2D();
static bool DrawDeleteD2D(HDC hdc, const RECT& rc, COLORREF circle);

// UI face: Segoe UI where it exists (Vista and later), otherwise Tahoma (Windows XP).
static const wchar_t* g_uiFace = L"Segoe UI";
static bool           g_haveEmojiFont;   // Segoe UI Emoji present; without it the title emoji is skipped

static int CALLBACK FontFoundProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found)
{
    *(bool*)found = true;
    return 0;
}

static bool FontExists(const wchar_t* face)
{
    LOGFONTW lf; ZeroMemory(&lf, sizeof lf);
    lf.lfCharSet = DEFAULT_CHARSET;
    CopyW(lf.lfFaceName, LF_FACESIZE, face);
    bool found = false;
    HDC dc = GetDC(NULL);
    EnumFontFamiliesExW(dc, &lf, FontFoundProc, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

static void DetectFonts()
{
    if (!FontExists(L"Segoe UI")) g_uiFace = L"Tahoma";
    g_haveEmojiFont = FontExists(L"Segoe UI Emoji");
}

// Emoji appended to the title: U+1F44B (waving hand) as its UTF-16 surrogate pair. Drawn with
// Segoe UI Emoji, which GDI renders in monochrome outline form.
#ifndef TITLE_EMOJI
#define TITLE_EMOJI L"\xD83D\xDC4B"
#endif
static UINT     g_dpi = 96;
static int      g_firstRow;     // index of the first visible row (row-based scrolling)
static int      g_nextRowId = 1;
static bool     g_closing;
static std::vector<Row*> g_rows;
static Row*     g_dragRow;      // the row being moved by its handle, or NULL
static bool     g_dragMoved;    // and whether it has left its place
static int      g_dragY;        // the pointer's height in the main window while it is
static WNDPROC  g_oldEditProc;
static UINT (WINAPI* g_pGetDpiForWindow)(HWND);

static Metrics Mx() { return Metrics((int)g_dpi); }
static int S(int v) { return Mx().S(v); }
static COLORREF Colour(const Rgb& c) { return RGB(c.r, c.g, c.b); }

// ---------------------------------------------------------------------------------------------
// Light and dark
// ---------------------------------------------------------------------------------------------

// Windows 10 1809 and later have a dark app mode, which the classic controls ignore: in it the
// window paints its own background and text, and gives the address boxes and the scroll bar the
// system's dark control themes. It follows the app mode only where DWM can make the title bar dark
// too; elsewhere (XP to Windows 10 1803) it stays light. Advapi32, dwmapi and uxtheme are loaded
// at runtime, keeping them out of the import table.
static bool   g_dark;
static bool   g_darkEver;   // dark has been applied, so going light must undo the control themes
static HBRUSH g_darkFace, g_darkField;
static const Rgb kDarkFace = { 32, 32, 32 }, kDarkText = { 255, 255, 255 };   // Windows 11's own dark windows

static const Palette& Pal() { return PaletteFor(g_dark); }
static COLORREF Face()      { return g_dark ? Colour(kDarkFace) : GetSysColor(COLOR_BTNFACE); }
static COLORREF Ink()       { return g_dark ? Colour(kDarkText) : GetSysColor(COLOR_BTNTEXT); }
static HBRUSH   FaceBrush() { return g_dark ? g_darkFace : GetSysColorBrush(COLOR_BTNFACE); }

static bool AppsUseDarkTheme()
{
    typedef LONG (WINAPI* PFN_ROKE)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
    typedef LONG (WINAPI* PFN_RQVE)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
    typedef LONG (WINAPI* PFN_RCK)(HKEY);
    HMODULE adv = LoadLibraryW(L"advapi32.dll");
    PFN_ROKE open  = adv ? (PFN_ROKE)GetProcAddress(adv, "RegOpenKeyExW") : NULL;
    PFN_RQVE query = adv ? (PFN_RQVE)GetProcAddress(adv, "RegQueryValueExW") : NULL;
    PFN_RCK  close = adv ? (PFN_RCK)GetProcAddress(adv, "RegCloseKey") : NULL;
    HKEY key;
    if (!open || !query || !close ||
        open(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD light = 1, size = sizeof light;
    bool dark = query(key, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&light, &size) == ERROR_SUCCESS && light == 0;
    close(key);
    return dark;
}

// A window's control theme for the current mode: `dark` names the system's dark one. Untouched
// until dark mode has been seen.
static void ThemeControl(HWND h, const wchar_t* dark)
{
    if (!g_darkEver) return;
    typedef HRESULT (WINAPI* PFN_SWT)(HWND, LPCWSTR, LPCWSTR);
    static PFN_SWT setTheme;
    static bool tried;
    if (!tried) {
        tried = true;
        if (HMODULE ux = LoadLibraryW(L"uxtheme.dll")) setTheme = (PFN_SWT)GetProcAddress(ux, "SetWindowTheme");
    }
    if (setTheme) setTheme(h, g_dark ? dark : NULL, NULL);
}

// Reads the app mode and applies it to the title bar, the scroll bar and the address boxes; the
// rest picks it up as it paints.
static void ApplyTheme()
{
    typedef HRESULT (WINAPI* PFN_DWMSWA)(HWND, DWORD, LPCVOID, DWORD);
    static PFN_DWMSWA dwmSet;
    static bool tried;
    if (!tried) {
        tried = true;
        if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) dwmSet = (PFN_DWMSWA)GetProcAddress(dwm, "DwmSetWindowAttribute");
    }
    // DWMWA_USE_IMMERSIVE_DARK_MODE is 20, and was 19 before Windows 10 2004.
    BOOL want = AppsUseDarkTheme();
    bool set = dwmSet && (SUCCEEDED(dwmSet(g_hwnd, 20, &want, sizeof want)) || SUCCEEDED(dwmSet(g_hwnd, 19, &want, sizeof want)));
    g_dark = set && want;
    if (g_dark && !g_darkEver) {
        g_darkEver = true;
        g_darkFace = CreateSolidBrush(Colour(kDarkFace));
        g_darkField = CreateSolidBrush(Colour(kDark.graphBack));
    }
    ThemeControl(g_hwnd, L"DarkMode_Explorer");
    for (Row* r : g_rows) ThemeControl(r->edit, L"DarkMode_CFD");
}

// UTF-8 to UTF-16 and back, for the core's texts and the files.
static std::wstring Widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string Narrow(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, 0);
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

// ---------------------------------------------------------------------------------------------
// Ping worker thread
// ---------------------------------------------------------------------------------------------

static DWORD WINAPI PingThread(LPVOID p)
{
    PingCtx* c = (PingCtx*)p;

    char hostA[512];
    WideCharToMultiByte(CP_UTF8, 0, c->host, -1, hostA, sizeof hostA, NULL, NULL);

    HANDLE icmp = INVALID_HANDLE_VALUE;
    int family = 0;
    sockaddr_storage dst;
    ZeroMemory(&dst, sizeof dst);

    char data[32] = "MultiPing echo request";
    std::vector<BYTE> reply(sizeof(ICMP_ECHO_REPLY) + sizeof(ICMPV6_ECHO_REPLY) + sizeof data + 64);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); // less scheduling jitter in the timing

    for (;;) {
        // Wait for the UI tick to release this round's ping (or for shutdown).
        HANDLE waits[2] = { c->stop, c->go };
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
        LONG seq = c->seq;   // the round this ping answers; the UI discards results from older rounds

        if (icmp == INVALID_HANDLE_VALUE) {
            addrinfo hints; ZeroMemory(&hints, sizeof hints);
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* res = NULL;
            if (getaddrinfo(hostA, NULL, &hints, &res) == 0 && res) {
                // Prefer IPv4 if present, otherwise take the first IPv6 result.
                addrinfo* pick = NULL;
                for (addrinfo* a = res; a; a = a->ai_next)
                    if (a->ai_family == AF_INET) { pick = a; break; }
                if (!pick)
                    for (addrinfo* a = res; a; a = a->ai_next)
                        if (a->ai_family == AF_INET6) { pick = a; break; }
                if (pick) {
                    family = pick->ai_family;
                    memcpy(&dst, pick->ai_addr, pick->ai_addrlen);
                    icmp = (family == AF_INET) ? IcmpCreateFile() : Icmp6CreateFile();
                }
                freeaddrinfo(res);
            }
            if (icmp == INVALID_HANDLE_VALUE) {
                InterlockedExchange(&c->result, RES_NOHOST);
                InterlockedExchange(&c->resultSeq, seq);
                InterlockedExchange(&c->ready, 1);
                continue;   // retried on the next tick
            }
        }

        // The ICMP API only reports whole milliseconds, so time the round trip ourselves with the
        // high-resolution counter. Result is in microseconds.
        LPARAM result = RES_TIMEOUT;
        bool ok = false;
        LARGE_INTEGER qa, qb;
        if (family == AF_INET) {
            IPAddr ip = ((sockaddr_in*)&dst)->sin_addr.S_un.S_addr;
            QueryPerformanceCounter(&qa);
            DWORD n = IcmpSendEcho(icmp, ip, data, sizeof data, NULL,
                                   reply.data(), (DWORD)reply.size(), PING_TIMEOUT_MS);
            QueryPerformanceCounter(&qb);
            if (n > 0) ok = ((ICMP_ECHO_REPLY*)reply.data())->Status == IP_SUCCESS;
        } else {
            sockaddr_in6 src; ZeroMemory(&src, sizeof src);
            src.sin6_family = AF_INET6;
            QueryPerformanceCounter(&qa);
            DWORD n = Icmp6SendEcho2(icmp, NULL, NULL, NULL, &src, (sockaddr_in6*)&dst,
                                     data, sizeof data, NULL,
                                     reply.data(), (DWORD)reply.size(), PING_TIMEOUT_MS);
            QueryPerformanceCounter(&qb);
            if (n > 0) ok = ((ICMPV6_ECHO_REPLY*)reply.data())->Status == IP_SUCCESS;
        }
        if (ok) {
            // Clamp at zero: on some systems (XP in a VM, for one) the counter can run slightly
            // differently per CPU, and a negative delta must not be mistaken for a RES_* code.
            LONGLONG us = (qb.QuadPart - qa.QuadPart) * 1000000 / freq.QuadPart;
            result = us < 0 ? 0 : (LPARAM)us;
        }
        InterlockedExchange(&c->result, (LONG)result);
        InterlockedExchange(&c->resultSeq, seq);
        InterlockedExchange(&c->ready, 1);
    }

    if (icmp != INVALID_HANDLE_VALUE) IcmpCloseHandle(icmp);
    CloseHandle(c->stop);
    CloseHandle(c->go);
    delete c;
    return 0;
}

static void StopPing(Row* r)
{
    if (r->ctx) {
        SetEvent(r->ctx->stop);   // thread frees ctx on exit
        r->ctx = NULL;
    }
}

static void StartPing(Row* r)
{
    StopPing(r);
    if (r->addr.empty()) return;
    PingCtx* c = new PingCtx;
    CopyW(c->host, sizeof c->host / sizeof c->host[0], r->addr.c_str());
    c->stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    c->go = CreateEvent(NULL, FALSE, FALSE, NULL);
    c->ready = 0;
    c->result = RES_NONE;
    c->seq = 1;
    c->resultSeq = 0;
    c->inflight = false;
    HANDLE h = CreateThread(NULL, 0, PingThread, c, 0, NULL);
    if (!h) { CloseHandle(c->stop); CloseHandle(c->go); delete c; return; }
    CloseHandle(h);
    r->ctx = c;
    // First ping goes out right away; its result is committed on the next tick with everyone else's.
    c->inflight = true;
    SetEvent(c->go);
}

// ---------------------------------------------------------------------------------------------
// Persistence: %APPDATA%\MultiPing\addresses.txt (UTF-8, one address per line)
// ---------------------------------------------------------------------------------------------

static std::wstring ConfigDir(bool create)
{
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH);
    std::wstring dir;
    if (n == 0 || n >= MAX_PATH) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (slash) *slash = 0;
        dir = exe;
    } else {
        dir = base;
        dir += L"\\MultiPing";
        if (create) CreateDirectoryW(dir.c_str(), NULL);
    }
    return dir;
}

static std::wstring ConfigPath(bool create) { return ConfigDir(create) + L"\\addresses.txt"; }

static bool ReadWholeFile(const std::wstring& path, std::string* bytes)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[4096]; DWORD got;
    while (ReadFile(f, buf, sizeof buf, &got, NULL) && got) bytes->append(buf, got);
    CloseHandle(f);
    return true;
}

static void WriteWholeFile(const std::wstring& path, const std::string& bytes)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote;
    if (!bytes.empty()) WriteFile(f, bytes.data(), (DWORD)bytes.size(), &wrote, NULL);
    CloseHandle(f);
}

// Window placement: "left top width height" in screen pixels, in window.txt next to the addresses.

static void SaveWindowRect(HWND h)
{
    RECT r;
    if (IsIconic(h) || IsZoomed(h)) {
        // Use the restored-state rectangle. It is in workspace coordinates, i.e. relative to the
        // primary monitor's work area, so shift it back into screen coordinates.
        WINDOWPLACEMENT wp = { sizeof wp };
        GetWindowPlacement(h, &wp);
        r = wp.rcNormalPosition;
        POINT origin = { 0, 0 };
        MONITORINFO mi = { sizeof mi };
        GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
        OffsetRect(&r, mi.rcWork.left, mi.rcWork.top);
    } else {
        GetWindowRect(h, &r);
    }
    WindowRect wr = { r.left, r.top, r.right - r.left, r.bottom - r.top };
    WriteWholeFile(ConfigDir(true) + L"\\window.txt", FormatWindowRect(wr, "\r\n"));
}

// Loads the saved rectangle and makes it fit the work area of the monitor it is nearest to.
static bool LoadWindowRect(RECT* out)
{
    std::string text;
    WindowRect wr = { 0, 0, 0, 0 };
    if (!ReadWholeFile(ConfigDir(false) + L"\\window.txt", &text) || !ParseWindowRect(text, &wr)) return false;
    RECT minR = { 0, 0, Mx().MinContentWidth() + GetSystemMetrics(SM_CXVSCROLL), 100 };
    RECT defR = { 0, 0, Mx().DefaultWidth(), 100 };
    AdjustWindowRectEx(&minR, WS_OVERLAPPEDWINDOW, FALSE, 0);
    AdjustWindowRectEx(&defR, WS_OVERLAPPEDWINDOW, FALSE, 0);
    wr.w = FitSavedWidth(wr.w, minR.right - minR.left, defR.right - defR.left);
    RECT r = { wr.x, wr.y, wr.x + wr.w, wr.y + wr.h };
    MONITORINFO mi = { sizeof mi };
    if (!GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    const RECT& wa = mi.rcWork;
    FitWindowRect(&wr, wa.left, wa.top, wa.right - wa.left, wa.bottom - wa.top);
    out->left = wr.x; out->top = wr.y; out->right = wr.x + wr.w; out->bottom = wr.y + wr.h;
    return true;
}

static std::vector<Entry> LoadEntries()
{
    std::string bytes;
    ReadWholeFile(ConfigPath(false), &bytes);
    return ParseEntries(bytes);
}

static void SaveAddresses()
{
    std::vector<Entry> rows;
    for (Row* r : g_rows) rows.push_back(Entry{ Narrow(r->addr), Narrow(r->notes) });
    WriteWholeFile(ConfigPath(true), JoinEntries(rows, "\r\n"));
}

// ---------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------

static Cols ComputeCols(int clientW) { return Mx().Columns(clientW); }
static int MinContentWidth()            { return Mx().MinContentWidth(); }
static int LogoTop()                    { return Mx().LogoTop(); }
static int HeaderTop()                  { return Mx().HeaderTop(); }
static int ContentHeightFor(int rows)   { return Mx().ContentHeightFor(rows); }

// Scrolling is by whole rows: the title and header stay put, rows g_firstRow .. g_firstRow+fit-1
// are shown, and any row that would not fit completely is hidden rather than drawn partially.

static int RowsThatFit()
{
    RECT rc; GetClientRect(g_hwnd, &rc);
    return Mx().RowsThatFit(rc.bottom);
}

#ifdef MP_DEBUG_LOG
static void LogF(const char* fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0 || n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    va_end(ap);
    HANDLE f = CreateFileW(L"C:\\devsrc\\mp.log", FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(f, buf, (DWORD)n, &w, NULL); WriteFile(f, "\r\n", 2, &w, NULL);
    CloseHandle(f);
}
#define LOG(...) LogF(__VA_ARGS__)
#else
#define LOG(...) ((void)0)
#endif

static void Layout()
{
    RECT rc; GetClientRect(g_hwnd, &rc);
    Cols c = ComputeCols(rc.right);
    int fit = RowsThatFit();
    HDWP dwp = BeginDeferWindowPos((int)g_rows.size() * NCOL);
    LOG("Layout: rows=%d client=%dx%d fit=%d first=%d dpi=%u dwp=%p x0=%d w0=%d xg=%d wg=%d",
        (int)g_rows.size(), rc.right, rc.bottom, fit, g_firstRow, g_dpi, (void*)dwp, c.x[0], c.w[0], c.x[C_GRAPH], c.w[C_GRAPH]);
    for (size_t i = 0; i < g_rows.size(); i++) {
        Row* r = g_rows[i];
        bool shown = (int)i >= g_firstRow && (int)i < g_firstRow + fit;
        int y = Mx().RowTop((int)i - g_firstRow);
        HWND ctl[NCOL];
        RowWindows(r, ctl);
        for (int k = 0; k < NCOL; k++) {
            Rect cell = Mx().Cell(c, k, y);
            // Show/hide is done separately: DeferWindowPos may not move or size a window in the
            // same call that shows or hides it (Windows XP enforces this; later versions do not).
            // The empty row at the end has no handle: it stays last.
            bool show = shown && (k != C_GRIP || i + 1 < g_rows.size());
            if (show != (IsWindowVisible(ctl[k]) != FALSE))
                ShowWindow(ctl[k], show ? SW_SHOWNA : SW_HIDE);
            dwp = DeferWindowPos(dwp, ctl[k], NULL, cell.x, cell.y, cell.w, cell.h, SWP_NOZORDER | SWP_NOACTIVATE);
            if (!dwp) LOG("  DeferWindowPos failed row=%d col=%d hwnd=%p err=%lu", (int)i, k, (void*)ctl[k], GetLastError());
        }
    }
    if (!EndDeferWindowPos(dwp)) LOG("  EndDeferWindowPos failed err=%lu", GetLastError());
    InvalidateRect(g_hwnd, NULL, TRUE);
}

static void UpdateScroll()
{
    int fit = RowsThatFit(), n = (int)g_rows.size();
    g_firstRow = ClampFirstRow(g_firstRow, n, fit);
    SCROLLINFO si = { sizeof si };
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = n - 1;
    si.nPage = fit;
    si.nPos = g_firstRow;
    SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);   // hides the bar when everything fits
}

static void SetFirstRow(int first)
{
    first = ClampFirstRow(first, (int)g_rows.size(), RowsThatFit());
    if (first == g_firstRow) return;
    g_firstRow = first;
    UpdateScroll();
    Layout();
}

static void EnsureRowVisible(size_t i)
{
    int fit = RowsThatFit();
    if ((int)i < g_firstRow) SetFirstRow((int)i);
    else if ((int)i >= g_firstRow + fit) SetFirstRow((int)i - fit + 1);
}

// ---------------------------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------------------------

static Row* FindRowById(int id)
{
    for (Row* r : g_rows) if (r->id == id) return r;
    return NULL;
}
// The row an address or notes box belongs to, and which of the two it is.
static size_t IndexOfEdit(HWND edit, bool* notes = NULL)
{
    for (size_t i = 0; i < g_rows.size(); i++) {
        if (g_rows[i]->edit != edit && g_rows[i]->notesEdit != edit) continue;
        if (notes) *notes = g_rows[i]->notesEdit == edit;
        return i;
    }
    return (size_t)-1;
}

static void UpdateLabels(Row* r)
{
    std::string summary[4];
    r->stats.Summary(summary);
    SetWindowTextW(r->cur, Widen(r->stats.Current()).c_str());
    HWND ctl[4] = { r->mn, r->med, r->avg, r->mx };
    for (int i = 0; i < 4; i++) SetWindowTextW(ctl[i], Widen(summary[i]).c_str());
}

// res: microseconds (>= 0) or a RES_* code.
static void RecordResult(Row* r, int res)
{
    r->stats.Record(res);
    UpdateLabels(r);
    InvalidateRect(r->graph, NULL, FALSE);
}

static HWND MakeStatic(DWORD style)
{
    HWND h = CreateWindowExW(0, L"STATIC", L"-", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | style,
                             0, 0, 10, 10, g_hwnd, NULL, GetModuleHandle(NULL), NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)g_font, FALSE);
    return h;
}

static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM);

// An address or notes box: Enter, Tab and Escape handled by EditProc.
static HWND MakeEdit(const std::wstring& text)
{
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(),
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
                             0, 0, 10, 10, g_hwnd, NULL, GetModuleHandle(NULL), NULL);
    SendMessage(h, WM_SETFONT, (WPARAM)g_font, FALSE);
    WNDPROC old = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)EditProc);
    if (!g_oldEditProc) g_oldEditProc = old;
    ThemeControl(h, L"DarkMode_CFD");
    return h;
}

static Row* AddRow(const std::wstring& addr, const std::wstring& notes)
{
    Row* r = new Row;
    r->id = g_nextRowId++;
    r->addr = addr;
    r->notes = notes;
    r->ctx = NULL;
    HINSTANCE hi = GetModuleHandle(NULL);

    r->grip = CreateWindowExW(0, L"MultiPingGrip", NULL, WS_CHILD | WS_VISIBLE,
                              0, 0, 10, 10, g_hwnd, NULL, hi, r);
    r->edit = MakeEdit(addr);
    SendMessage(r->edit, EM_SETCUEBANNER, FALSE, (LPARAM)Widen(kCue).c_str());
    r->notesEdit = MakeEdit(notes);
    r->cur = MakeStatic(SS_RIGHT);
    r->mn  = MakeStatic(SS_RIGHT);
    r->med = MakeStatic(SS_RIGHT);
    r->avg = MakeStatic(SS_RIGHT);
    r->mx  = MakeStatic(SS_RIGHT);
    r->graph = CreateWindowExW(0, L"MultiPingGraph", NULL, WS_CHILD | WS_VISIBLE,
                               0, 0, 10, 10, g_hwnd, NULL, hi, r);
    r->del = CreateWindowExW(0, L"MultiPingDelete", NULL, WS_CHILD | WS_VISIBLE,
                             0, 0, 10, 10, g_hwnd, NULL, hi, r);

    g_rows.push_back(r);
    UpdateLabels(r);
    StartPing(r);
    return r;
}

static void DeleteRow(Row* r)
{
    StopPing(r);
    HWND ctl[NCOL];
    RowWindows(r, ctl);
    for (HWND h : ctl) DestroyWindow(h);
    g_rows.erase(std::find(g_rows.begin(), g_rows.end(), r));
    delete r;
}

// The list always ends with an empty row so it is obvious where a new address goes.
static void EnsureTrailingEmptyRow()
{
    if (g_rows.empty() || !g_rows.back()->addr.empty()) {
        AddRow(L"", L"");
        UpdateScroll();
        Layout();
    }
}

// Reads the edit box into the row; restarts pinging if the address changed. Returns true if changed.
static bool ApplyEdit(Row* r)
{
    wchar_t buf[512];
    GetWindowTextW(r->edit, buf, 512);
    std::wstring a = Widen(Trim(Narrow(buf)));
    if (a == r->addr) return false;
    r->addr = a;
    StopPing(r);
    r->stats.Reset();
    UpdateLabels(r);
    InvalidateRect(r->graph, NULL, FALSE);
    StartPing(r);
    SaveAddresses();
    return true;
}

// Reads the notes box into the row, and saves when they changed. Returns true if they did.
static bool ApplyNotes(Row* r)
{
    wchar_t buf[512];
    GetWindowTextW(r->notesEdit, buf, 512);
    std::wstring n = Widen(OneLine(Narrow(buf)));
    if (n == r->notes) return false;
    r->notes = n;
    SaveAddresses();
    return true;
}

// Puts the caret in row i's address box, or its notes box, with the text selected.
static void FocusEdit(size_t i, bool notes = false)
{
    if (i >= g_rows.size()) return;
    EnsureRowVisible(i);
    HWND h = notes ? g_rows[i]->notesEdit : g_rows[i]->edit;
    SetFocus(h);
    SendMessage(h, EM_SETSEL, 0, -1);
}

// ---------------------------------------------------------------------------------------------
// Edit subclass: Enter / Tab / Escape handling, apply on focus loss
// ---------------------------------------------------------------------------------------------

static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_RETURN) { SendMessage(g_hwnd, WM_APP_ENTER, 0, (LPARAM)h); return 0; }
        if (w == VK_TAB)    { SendMessage(g_hwnd, WM_APP_TAB, GetKeyState(VK_SHIFT) < 0, (LPARAM)h); return 0; }
        if (w == VK_ESCAPE) {
            bool notes = false;
            size_t i = IndexOfEdit(h, &notes);
            if (i != (size_t)-1) SetWindowTextW(h, (notes ? g_rows[i]->notes : g_rows[i]->addr).c_str());
            return 0;
        }
        break;
    case WM_CHAR:
        if (w == L'\r' || w == L'\n' || w == L'\t' || w == 27) return 0;
        break;
    case WM_KILLFOCUS: {
        LRESULT r = CallWindowProc(g_oldEditProc, h, m, w, l);
        SendMessage(g_hwnd, WM_APP_APPLY, 0, (LPARAM)h);
        return r;
    }
    }
    return CallWindowProc(g_oldEditProc, h, m, w, l);
}

// ---------------------------------------------------------------------------------------------
// Graph control
// ---------------------------------------------------------------------------------------------

static void PaintGraph(HWND h, Row* r)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) { EndPaint(h, &ps); return; }

    HDC mdc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ oldBmp = SelectObject(mdc, bmp);

    const Palette& P = Pal();
    HBRUSH back = CreateSolidBrush(Colour(P.graphBack));
    FillRect(mdc, &rc, back);
    DeleteObject(back);

    Graph g = LayoutGraph(r->stats, W, H);
    int bottom = g.plot.y + g.plot.h;

    // Grid lines at 25 / 50 / 75 %.
    HPEN grid = CreatePen(PS_SOLID, 1, Colour(P.graphGrid));
    HGDIOBJ oldPen = SelectObject(mdc, grid);
    for (int y : g.gridY) {
        MoveToEx(mdc, g.plot.x, y, NULL); LineTo(mdc, g.plot.x + g.plot.w, y);
    }
    SelectObject(mdc, oldPen);
    DeleteObject(grid);

    HBRUSH blue = CreateSolidBrush(Colour(P.graphBar));
    HBRUSH pink = CreateSolidBrush(Colour(P.graphTimeout));
    HBRUSH red  = CreateSolidBrush(Colour(P.graphTimeoutTop));
    for (const GraphBar& b : g.bars) {
        RECT bar = { b.x0, b.top, b.x1, bottom };
        if (!b.timeout) {
            FillRect(mdc, &bar, blue);
        } else {
            FillRect(mdc, &bar, pink);
            bar.bottom = b.top + S(3);
            FillRect(mdc, &bar, red);
        }
    }
    DeleteObject(blue); DeleteObject(pink); DeleteObject(red);

    // Scale label, top-left.
    HGDIOBJ oldFont = SelectObject(mdc, g_fontSmall);
    SetBkMode(mdc, OPAQUE);                 // the graph's colour behind it keeps the label legible over tall bars
    SetBkColor(mdc, Colour(P.graphBack));
    SetTextColor(mdc, Colour(P.graphLabel));
    RECT lr = { g.plot.x + S(3), g.plot.y + S(1), g.plot.x + g.plot.w, bottom };
    DrawTextW(mdc, Widen(g.scaleLabel).c_str(), -1, &lr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(mdc, oldFont);

    HBRUSH border = CreateSolidBrush(Colour(P.graphBorder));
    FrameRect(mdc, &rc, border);
    DeleteObject(border);

    BitBlt(hdc, 0, 0, W, H, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mdc);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK GraphProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCT*)l)->lpCreateParams);
        return TRUE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        Row* r = (Row*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (r) PaintGraph(h, r);
        else { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); }
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------------------------
// Delete button control: red filled circle with a white X
// ---------------------------------------------------------------------------------------------

struct DelState { Row* row; bool hover; bool down; };

static void PaintDelete(HWND h, DelState* st)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);
    int W = rc.right, H = rc.bottom;
    if (W <= 0 || H <= 0) { EndPaint(h, &ps); return; }

    HDC mdc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ oldBmp = SelectObject(mdc, bmp);
    FillRect(mdc, &rc, FaceBrush());

    const Palette& P = Pal();
    COLORREF col = Colour(st->down ? P.delDown : st->hover ? P.delHover : P.del);
    if (!DrawDeleteD2D(mdc, rc, col)) {
        // GDI fallback (not antialiased).
        int d = (W < H ? W : H);
        int cx = W / 2, cy = H / 2, rad = d / 2;
        HBRUSH fill = CreateSolidBrush(col);
        HPEN edge = CreatePen(PS_SOLID, 1, col);
        HGDIOBJ oldB = SelectObject(mdc, fill), oldP = SelectObject(mdc, edge);
        Ellipse(mdc, cx - rad, cy - rad, cx + rad, cy + rad);
        SelectObject(mdc, oldB); SelectObject(mdc, oldP);
        DeleteObject(fill); DeleteObject(edge);

        int thick = d / 9; if (thick < 2) thick = 2;
        int ins = (int)(rad * 0.45);
        HPEN xp = CreatePen(PS_SOLID, thick, RGB(255, 255, 255));
        oldP = SelectObject(mdc, xp);
        MoveToEx(mdc, cx - ins, cy - ins, NULL); LineTo(mdc, cx + ins, cy + ins);
        MoveToEx(mdc, cx - ins, cy + ins, NULL); LineTo(mdc, cx + ins, cy - ins);
        SelectObject(mdc, oldP);
        DeleteObject(xp);
    }

    BitBlt(hdc, 0, 0, W, H, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mdc);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK DeleteProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    DelState* st = (DelState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_NCCREATE: {
        st = new DelState;
        st->row = (Row*)((CREATESTRUCT*)l)->lpCreateParams;
        st->hover = st->down = false;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
        return TRUE;
    }
    case WM_NCDESTROY:
        delete st;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (st) PaintDelete(h, st);
        return 0;
    case WM_MOUSEMOVE:
        if (st && !st->hover) {
            st->hover = true;
            TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (st) { st->hover = false; InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_LBUTTONDOWN:
        if (st) { st->down = true; SetCapture(h); InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_LBUTTONUP:
        if (st && st->down) {
            st->down = false;
            ReleaseCapture();
            InvalidateRect(h, NULL, FALSE);
            RECT rc; GetClientRect(h, &rc);
            POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            if (PtInRect(&rc, pt)) PostMessage(g_hwnd, WM_APP_DELETE, (WPARAM)st->row->id, 0);
        }
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------------------------
// Handle control: six dots at a row's left; dragged up or down, it moves the row
// ---------------------------------------------------------------------------------------------

// Moves the row being dragged to where the pointer is, scrolling the list a row when `scroll` is
// set (the timer's tick), and keeps the timer running while the pointer is above or below the
// rows shown.
static void DragTo(bool scroll)
{
    if (!g_dragRow) return;
    int n = (int)g_rows.size(), fit = RowsThatFit(), first = g_firstRow;
    size_t from = std::find(g_rows.begin(), g_rows.end(), g_dragRow) - g_rows.begin();
    int to = DragTarget(Mx(), g_dragY, &first, fit, n, n - 1, scroll);
    bool moved = to != (int)from, scrolled = first != g_firstRow;
    if (moved) { MoveItem(g_rows, from, (size_t)to); g_dragMoved = true; }
    if (scrolled) { g_firstRow = first; UpdateScroll(); }
    if (moved || scrolled) Layout();
    if (DragOutside(Mx(), g_dragY, fit)) SetTimer(g_hwnd, TIMER_DRAG, DRAG_SCROLL_MS, NULL);
    else KillTimer(g_hwnd, TIMER_DRAG);
}

static void EndDrag()
{
    if (!g_dragRow) return;
    g_dragRow = NULL;
    KillTimer(g_hwnd, TIMER_DRAG);
    for (Row* r : g_rows) InvalidateRect(r->grip, NULL, FALSE);
    if (g_dragMoved) SaveAddresses();
}

struct GripState { Row* row; bool tracking; };

// A mouse message's pointer height, from the handle's client area to the main window's.
static int MainY(HWND grip, LPARAM l)
{
    POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    MapWindowPoints(grip, g_hwnd, &pt, 1);
    return pt.y;
}

static void PaintGrip(HWND h, GripState* st)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);
    FillRect(hdc, &rc, FaceBrush());
    POINT pt; GetCursorPos(&pt);
    ScreenToClient(h, &pt);
    bool active = g_dragRow ? g_dragRow == st->row : PtInRect(&rc, pt) != FALSE;
    HBRUSH dots = CreateSolidBrush(Colour(active ? Pal().gripActive : Pal().grip));
    Rect box = { 0, 0, rc.right, rc.bottom };
    GripDots g = LayoutGrip(Mx(), box);
    for (const Rect& d : g.dot) {
        RECT dr = { d.x, d.y, d.x + d.w, d.y + d.h };
        FillRect(hdc, &dr, dots);
    }
    DeleteObject(dots);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK GripProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    GripState* st = (GripState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_NCCREATE: {
        st = new GripState;
        st->row = (Row*)((CREATESTRUCT*)l)->lpCreateParams;
        st->tracking = false;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)st);
        return TRUE;
    }
    case WM_NCDESTROY:
        delete st;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (st) PaintGrip(h, st);
        else { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); }
        return 0;
    case WM_MOUSEMOVE:
        if (st && !st->tracking) {
            st->tracking = true;
            TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(h, NULL, FALSE);
        }
        if (g_dragRow) { g_dragY = MainY(h, l); DragTo(false); }
        return 0;
    case WM_MOUSELEAVE:
        if (st) { st->tracking = false; InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_LBUTTONDOWN:
        if (st && !g_closing) {
            g_dragRow = st->row;
            g_dragMoved = false;
            g_dragY = MainY(h, l);
            SetCapture(h);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        // The release can bring the pointer's last move with it.
        if (g_dragRow) { g_dragY = MainY(h, l); DragTo(false); }
        if (GetCapture() == h) ReleaseCapture();   // ends the drag through WM_CAPTURECHANGED
        EndDrag();
        return 0;
    case WM_CAPTURECHANGED:
        EndDrag();
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_SIZENS));
        return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------------------------
// Main window
// ---------------------------------------------------------------------------------------------

static bool InitD2D()
{
    if (g_d2dTried) return g_rt != NULL;
    g_d2dTried = true;
    HMODULE hd2d = LoadLibraryW(L"d2d1.dll");
    HMODULE hdw  = LoadLibraryW(L"dwrite.dll");
    if (!hd2d || !hdw) return false;
    typedef HRESULT (WINAPI* PFN_D2D)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
    typedef HRESULT (WINAPI* PFN_DW)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
    PFN_D2D pD2D = (PFN_D2D)GetProcAddress(hd2d, "D2D1CreateFactory");
    PFN_DW  pDW  = (PFN_DW)GetProcAddress(hdw, "DWriteCreateFactory");
    if (!pD2D || !pDW) return false;
    static const IID iidD2D1Factory  = { 0x06152247, 0x6f50, 0x465a, { 0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07 } };
    static const IID iidDWriteFactory = { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };
    if (FAILED(pD2D(D2D1_FACTORY_TYPE_SINGLE_THREADED, iidD2D1Factory, NULL, (void**)&g_d2d))) return false;
    if (FAILED(pDW(DWRITE_FACTORY_TYPE_SHARED, iidDWriteFactory, (IUnknown**)&g_dw))) return false;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        0, 0, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    if (FAILED(g_d2d->CreateDCRenderTarget(&props, &g_rt))) { g_rt = NULL; return false; }
    return true;
}

// Creates the DirectWrite format for the emoji at the title's pixel size (the DC render target
// works at 96 DPI, so DIPs are pixels here).
static void CreateEmojiFormat()
{
    if (g_emojiFmt) { g_emojiFmt->Release(); g_emojiFmt = NULL; }
    if (!InitD2D()) return;
    g_dw->CreateTextFormat(L"Segoe UI Emoji", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                           DWRITE_FONT_STRETCH_NORMAL, (float)MulDiv(FONT_LOGO_PT, (int)g_dpi, 72),
                           L"en-us", &g_emojiFmt);
}

// Draws the title emoji in color into `rc` of `hdc`. Returns false if the color path is unavailable.
static bool DrawColorEmoji(HDC hdc, const RECT& rc)
{
    if (!InitD2D() || !g_emojiFmt) return false;
    if (FAILED(g_rt->BindDC(hdc, &rc))) return false;
    g_rt->BeginDraw();
    COLORREF bg = Face(), ink = Ink();
    g_rt->Clear(D2D1::ColorF(GetRValue(bg) / 255.0f, GetGValue(bg) / 255.0f, GetBValue(bg) / 255.0f));
    ID2D1SolidColorBrush* brush = NULL;
    g_rt->CreateSolidColorBrush(D2D1::ColorF(GetRValue(ink) / 255.0f, GetGValue(ink) / 255.0f, GetBValue(ink) / 255.0f), &brush);
    D2D1_RECT_F lr = D2D1::RectF(0, 0, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
    if (brush) {
        // D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT (= 4); spelled numerically because the Windows
        // 7.1A SDK used for the XP build predates it. Ignored by Direct2D versions without color fonts.
        g_rt->DrawText(TITLE_EMOJI, (UINT32)wcslen(TITLE_EMOJI), g_emojiFmt, lr, brush,
                       (D2D1_DRAW_TEXT_OPTIONS)4);
        brush->Release();
    }
    HRESULT hr = g_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        g_rt->Release(); g_rt = NULL;
        g_d2dTried = false;             // rebuilt on the next paint
    }
    return SUCCEEDED(hr) && brush != NULL;
}

// Antialiased delete glyph: filled circle with a white X, drawn via Direct2D into `rc` of `hdc`.
static bool DrawDeleteD2D(HDC hdc, const RECT& rc, COLORREF circle)
{
    if (!InitD2D()) return false;
    if (FAILED(g_rt->BindDC(hdc, &rc))) return false;
    DeleteGlyph d = LayoutDelete((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
    float cx = d.cx, cy = d.cy, ins = d.inset;

    g_rt->BeginDraw();
    COLORREF bg = Face();
    g_rt->Clear(D2D1::ColorF(GetRValue(bg) / 255.0f, GetGValue(bg) / 255.0f, GetBValue(bg) / 255.0f));
    ID2D1SolidColorBrush* red = NULL; ID2D1SolidColorBrush* white = NULL;
    g_rt->CreateSolidColorBrush(D2D1::ColorF(GetRValue(circle) / 255.0f, GetGValue(circle) / 255.0f, GetBValue(circle) / 255.0f), &red);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &white);
    if (red && white) {
        g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), d.radius, d.radius), red);
        g_rt->DrawLine(D2D1::Point2F(cx - ins, cy - ins), D2D1::Point2F(cx + ins, cy + ins), white, d.stroke);
        g_rt->DrawLine(D2D1::Point2F(cx - ins, cy + ins), D2D1::Point2F(cx + ins, cy - ins), white, d.stroke);
    }
    if (red) red->Release();
    if (white) white->Release();
    HRESULT hr = g_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) { g_rt->Release(); g_rt = NULL; g_d2dTried = false; }
    return SUCCEEDED(hr) && red && white;
}

static const wchar_t* BuildInfoText()
{
    static std::wstring text;
    if (text.empty()) text = Widen(BuildInfo(__DATE__, GIT_REV));
    return text.c_str();
}

static void CreateFonts()
{
    if (g_font) DeleteObject(g_font);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    if (g_fontLogo) DeleteObject(g_fontLogo);
    if (g_fontEmoji) DeleteObject(g_fontEmoji);
    if (g_fontInfo) DeleteObject(g_fontInfo);
    g_fontInfo = CreateFontW(-MulDiv(FONT_INFO_PT, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, g_uiFace);
    CreateEmojiFormat();

    if (g_titleIcon) DestroyIcon(g_titleIcon);
    g_titleIcon = (HICON)LoadImageW(GetModuleHandle(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                    S(TITLE_ICON_PX), S(TITLE_ICON_PX), LR_DEFAULTCOLOR);
    g_fontLogo = CreateFontW(-MulDiv(FONT_LOGO_PT, (int)g_dpi, 72), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, g_uiFace);
    g_fontEmoji = CreateFontW(-MulDiv(FONT_LOGO_PT, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Emoji");
    g_font = CreateFontW(-MulDiv(FONT_PT, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, g_uiFace);
    g_fontSmall = CreateFontW(-MulDiv(FONT_SMALL_PT, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, g_uiFace);
}

static void ApplyFonts()
{
    for (Row* r : g_rows) {
        HWND ctl[7] = { r->edit, r->notesEdit, r->cur, r->mn, r->med, r->avg, r->mx };
        for (HWND h : ctl) SendMessage(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
}

static void PaintMain(HWND h)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);
    Cols c = ComputeCols(rc.right);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, Ink());

    // Title, large and bold, aligned with the Address column.
    HGDIOBJ oldFont = SelectObject(hdc, g_fontLogo);
    RECT lr = { c.x[0], LogoTop(), rc.right, LogoTop() + S(LOGO_H) };
    std::wstring title = Widen(kTitle);
    DrawTextW(hdc, title.c_str(), -1, &lr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
    SIZE ts; GetTextExtentPoint32W(hdc, title.c_str(), (int)title.size(), &ts);
    lr.left += ts.cx + S(10);
    RECT er = { lr.left, lr.top, lr.left + S(LOGO_H) * 3 / 2, lr.bottom };
    if (!g_haveEmojiFont) {
        // No emoji font on this system (e.g. Windows XP): draw the app icon in its place instead.
        if (g_titleIcon) {
            int sz = S(TITLE_ICON_PX);
            DrawIconEx(hdc, lr.left, LogoTop() + (S(LOGO_H) - sz) / 2, g_titleIcon, sz, sz, 0, NULL, DI_NORMAL);
            er.right = lr.left + sz;
        } else {
            er.right = lr.left;
        }
    } else if (!DrawColorEmoji(hdc, er)) {
        SelectObject(hdc, g_fontEmoji);   // monochrome fallback
        DrawTextW(hdc, TITLE_EMOJI, -1, &lr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
    }

    // Copyright and revision, stacked, right-aligned to the window margin beside the title.
    SelectObject(hdc, g_fontInfo);
    SetTextColor(hdc, Colour(Pal().infoText));
    TEXTMETRICW tm; GetTextMetricsW(hdc, &tm);
    int iy = LogoTop() + (S(LOGO_H) - 2 * tm.tmHeight) / 2;
    RECT ir = { er.right, iy, rc.right - S(MARGIN), iy + tm.tmHeight };
    DrawTextW(hdc, Widen(kCopyright).c_str(), -1, &ir, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    ir.top += tm.tmHeight; ir.bottom += tm.tmHeight;
    DrawTextW(hdc, BuildInfoText(), -1, &ir, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(hdc, Ink());

    SelectObject(hdc, g_font);
    int y = HeaderTop();
    for (int k = 0; k < NCOL; k++) {
        RECT r = { c.x[k], y, c.x[k] + c.w[k], y + S(HDR_H) };
        UINT align = LeftAligned(k) ? DT_LEFT : DT_RIGHT;
        DrawTextW(hdc, Widen(kColNames[k]).c_str(), -1, &r, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    HPEN pen = CreatePen(PS_SOLID, 1, Colour(Pal().headerLine));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    int ly = y + S(HDR_H);
    MoveToEx(hdc, c.x[0], ly, NULL);
    LineTo(hdc, c.x[NCOL - 1] + c.w[NCOL - 1], ly);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    SelectObject(hdc, oldFont);
    EndPaint(h, &ps);
}

static void OnVScroll(WPARAM w)
{
    SCROLLINFO si = { sizeof si };
    si.fMask = SIF_ALL;
    GetScrollInfo(g_hwnd, SB_VERT, &si);
    int pos = si.nPos;
    switch (LOWORD(w)) {
    case SB_LINEUP:        pos -= 1; break;
    case SB_LINEDOWN:      pos += 1; break;
    case SB_PAGEUP:        pos -= (int)si.nPage; break;
    case SB_PAGEDOWN:      pos += (int)si.nPage; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: pos = si.nTrackPos; break;
    case SB_TOP:           pos = 0; break;
    case SB_BOTTOM:        pos = si.nMax; break;
    default: return;
    }
    SetFirstRow(pos);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        g_hwnd = h;
        if (g_pGetDpiForWindow) g_dpi = g_pGetDpiForWindow(h);
        ApplyTheme();
        CreateFonts();
        for (const Entry& e : LoadEntries()) AddRow(Widen(e.addr), Widen(e.notes));
        EnsureTrailingEmptyRow();
        SetTimer(h, TIMER_TICK, PING_INTERVAL_MS, NULL);
        return 0;
    }
    case WM_SIZE:
        UpdateScroll();
        Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)l;
        RECT r = { 0, 0, MinContentWidth() + GetSystemMetrics(SM_CXVSCROLL), ContentHeightFor(MIN_ROWS) };
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        mmi->ptMinTrackSize.x = r.right - r.left;
        mmi->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_PAINT:
        PaintMain(h);
        return 0;
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, FaceBrush());
        return 1;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)w, TRANSPARENT);
        SetTextColor((HDC)w, Ink());
        return (LRESULT)FaceBrush();
    case WM_CTLCOLOREDIT:
        if (!g_dark) break;
        SetTextColor((HDC)w, Ink());
        SetBkColor((HDC)w, Colour(kDark.graphBack));
        return (LRESULT)g_darkField;
    case WM_SETTINGCHANGE:
        // The app mode changed in Settings.
        if (l && lstrcmpiW((const wchar_t*)l, L"ImmersiveColorSet") == 0) {
            ApplyTheme();
            RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        }
        break;
    case WM_VSCROLL:
        OnVScroll(w);
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        SetFirstRow(g_firstRow - delta / WHEEL_DELTA);
        return 0;
    }
    case WM_SETFOCUS:
        if (!g_rows.empty()) SetFocus(g_rows[0]->edit);
        return 0;

    case WM_TIMER: {
        if (w == TIMER_DRAG) { DragTo(true); return 0; }
        // One tick for every row: take last round's result, then release the next ping. All graphs
        // and labels therefore change in the same instant.
        if (g_closing || w != TIMER_TICK) return 0;
        for (Row* r : g_rows) {
            PingCtx* c = r->ctx;
            if (!c) continue;
            if (c->inflight) {
                // The tick is authoritative: a result that is not back yet (or answers an older
                // round) counts as a timeout, so every row commits exactly one sample per tick
                // whatever the ICMP stack's timing. Windows XP, for one, lets IcmpSendEcho run
                // past its nominal timeout.
                bool fresh = InterlockedCompareExchange(&c->ready, 0, 1) == 1 && c->resultSeq == c->seq;
                RecordResult(r, fresh ? (int)c->result : (int)RES_TIMEOUT);
            }
            c->seq++;
            c->inflight = true;
            SetEvent(c->go);   // if the worker is still busy, the auto-reset event holds the request
        }
        return 0;
    }
    case WM_APP_APPLY: {
        if (g_closing) return 0;
        bool notes = false;
        size_t i = IndexOfEdit((HWND)l, &notes);
        if (i == (size_t)-1) return 0;
        if (notes) ApplyNotes(g_rows[i]);
        else if (ApplyEdit(g_rows[i])) EnsureTrailingEmptyRow();
        return 0;
    }
    case WM_APP_ENTER: {
        // From either box, on to the next row's address.
        if (g_closing) return 0;
        bool notes = false;
        size_t i = IndexOfEdit((HWND)l, &notes);
        if (i == (size_t)-1) return 0;
        if (notes) ApplyNotes(g_rows[i]);
        else ApplyEdit(g_rows[i]);
        EnsureTrailingEmptyRow();           // a filled-in last row gets a fresh empty row after it
        if (i + 1 < g_rows.size()) FocusEdit(i + 1);
        return 0;
    }
    case WM_APP_TAB: {
        // The boxes in reading order: each row's address, then its notes.
        if (g_closing) return 0;
        bool notes = false;
        size_t i = IndexOfEdit((HWND)l, &notes);
        if (i == (size_t)-1 || g_rows.empty()) return 0;
        size_t n = 2 * g_rows.size(), f = 2 * i + (notes ? 1 : 0);
        f = w ? (f + n - 1) % n : (f + 1) % n;
        FocusEdit(f / 2, f % 2 == 1);
        return 0;
    }
    case WM_APP_DELETE: {
        if (g_closing) return 0;
        Row* r = FindRowById((int)w);
        if (!r) return 0;
        size_t idx = std::find(g_rows.begin(), g_rows.end(), r) - g_rows.begin();
        DeleteRow(r);
        EnsureTrailingEmptyRow();
        SaveAddresses();
        UpdateScroll();
        Layout();
        FocusEdit(idx < g_rows.size() ? idx : g_rows.size() - 1);
        return 0;
    }

    case WM_DPICHANGED: {
        g_dpi = HIWORD(w);
        CreateFonts();
        ApplyFonts();
        RECT* r = (RECT*)l;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateScroll();
        Layout();
        return 0;
    }
    case WM_DESTROY:
        g_closing = true;
        KillTimer(h, TIMER_TICK);
        KillTimer(h, TIMER_DRAG);
        for (Row* r : g_rows) {   // what the boxes hold, edited or not
            wchar_t buf[512];
            GetWindowTextW(r->edit, buf, 512);
            r->addr = Widen(Trim(Narrow(buf)));
            GetWindowTextW(r->notesEdit, buf, 512);
            r->notes = Widen(OneLine(Narrow(buf)));
        }
        SaveWindowRect(h);
        SaveAddresses();
        for (Row* r : g_rows) StopPing(r);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void InitDpi()
{
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI* PFN_SPDAC)(HANDLE);
    typedef BOOL (WINAPI* PFN_SPDA)();
    PFN_SPDAC spdac = (PFN_SPDAC)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (spdac) spdac((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
    else {
        PFN_SPDA spda = (PFN_SPDA)GetProcAddress(u, "SetProcessDPIAware");
        if (spda) spda();
    }
    g_pGetDpiForWindow = (UINT (WINAPI*)(HWND))GetProcAddress(u, "GetDpiForWindow");
    HDC dc = GetDC(NULL);
    g_dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int nShow)
{
    InitDpi();
    DetectFonts();

    // With the common-controls v6 manifest, the standard EDIT/STATIC classes come from comctl32 v6.
    // Windows XP only registers them once comctl32 is loaded and initialized (later versions do it
    // implicitly), so do that here; resolved dynamically to keep comctl32 out of the import table.
    if (HMODULE cc = LoadLibraryW(L"comctl32.dll")) {
        struct { DWORD dwSize, dwICC; } icc = { 8, 0x00004000 /* ICC_STANDARD_CLASSES */ };
        typedef BOOL (WINAPI* PFN_ICCE)(const void*);
        PFN_ICCE icce = (PFN_ICCE)GetProcAddress(cc, "InitCommonControlsEx");
        if (icce) icce(&icc);
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    WNDCLASSEXW wc = { sizeof wc };
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    wc.lpfnWndProc = GraphProc;
    wc.lpszClassName = L"MultiPingGraph";
    RegisterClassExW(&wc);

    wc.lpfnWndProc = DeleteProc;
    wc.lpszClassName = L"MultiPingDelete";
    RegisterClassExW(&wc);

    wc.lpfnWndProc = GripProc;
    wc.lpszClassName = L"MultiPingGrip";
    RegisterClassExW(&wc);

    wc.style = CS_HREDRAW;
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = L"MultiPingMain";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    // Icon resource 1 from MultiPing.rc; fall back to the stock icon if the resource is missing.
    wc.hIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    RECT r = { 0, 0, Mx().DefaultWidth(), ContentHeightFor(DEFAULT_ROWS) };
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = r.right - r.left, hgt = r.bottom - r.top;
    RECT saved;
    if (LoadWindowRect(&saved)) {
        x = saved.left; y = saved.top;
        w = saved.right - saved.left; hgt = saved.bottom - saved.top;
    }
    HWND hwnd = CreateWindowExW(0, L"MultiPingMain", L"MultiPing",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL,
                                x, y, w, hgt, NULL, NULL, hi, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);
    if (!g_rows.empty()) SetFocus(g_rows[0]->edit);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    WSACleanup();
    return (int)msg.wParam;
}
