// MultiPing - pings a list of addresses, one row per address, with live stats and a 60 s graph.
// Pure Win32 (user32/gdi32) + iphlpapi for ICMP + ws2_32 for name resolution. No comctl32, no MFC.
// Build: see build.bat (cl /MT ... statically linked CRT).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
// Opt into themed (visual style) standard controls. This is a manifest entry, not a link dependency.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------------------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------------------------

enum {
    WM_APP_ENTER = WM_APP + 1,  // lParam = edit hwnd
    WM_APP_APPLY,               // lParam = edit hwnd
    WM_APP_TAB,                 // wParam = shift pressed, lParam = edit hwnd
    WM_APP_DELETE,              // wParam = row id
};

enum { RES_NONE = -3, RES_NOHOST = -2, RES_TIMEOUT = -1 };

#define RING_N            60     // seconds of history shown in the graph
#define TIMER_TICK        1
#define PING_INTERVAL_MS  1000   // UI tick: commits last results and releases the next round of pings
#define PING_TIMEOUT_MS   900    // a little under the interval so every result is in before the next tick

// Fonts in points.
#define FONT_PT        14
#define FONT_SMALL_PT  9
#define FONT_LOGO_PT   24     // "MultiPing" title at the top, bold
#define LOGO_H         38     // height reserved for the title (96-dpi px)

// Layout metrics in 96-dpi pixels.
#define MARGIN       18     // outer padding around all content
#define GAP          6
#define ROW_H        56
#define HDR_H        30
#define CTRL_H       32
#define W_ADDR       290
#define W_CUR        110
#define W_STAT       92
#define W_GRAPH_MIN  240
#define GRAPH_GAP    28     // extra space between the Max column and the graph
#define W_DEL        24

enum { C_ADDR, C_CUR, C_MIN, C_MED, C_AVG, C_MAX, C_GRAPH, C_DEL, NCOL };
static const int g_colBase[NCOL] = { W_ADDR, W_CUR, W_STAT, W_STAT, W_STAT, W_STAT, W_GRAPH_MIN, W_DEL };
static const wchar_t* g_colNames[NCOL] = { L"Address", L"Current", L"Min", L"Median", L"Avg", L"Max", L"Last 60 s", L"" };

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
    bool          inflight;   // UI side: a `go` has been issued whose result is not yet taken
};

struct Row {
    int           id;
    std::wstring  addr;
    HWND          edit, cur, mn, med, avg, mx, graph, del;
    PingCtx*      ctx;
    std::vector<double> samples;    // all successful RTTs this session
    int           ring[RING_N];     // last RING_N results: >=0 rtt, -1 timeout
    int           ringCount, ringHead;
    LPARAM        last;
};

static HWND     g_hwnd;
static HFONT    g_font, g_fontSmall, g_fontLogo;
static UINT     g_dpi = 96;
static int      g_scrollY;
static int      g_nextRowId = 1;
static bool     g_closing;
static std::vector<Row*> g_rows;
static WNDPROC  g_oldEditProc;
static UINT (WINAPI* g_pGetDpiForWindow)(HWND);

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

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
        if (ok) result = (LPARAM)((qb.QuadPart - qa.QuadPart) * 1000000 / freq.QuadPart);
        InterlockedExchange(&c->result, (LONG)result);
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
    wcsncpy_s(c->host, r->addr.c_str(), _TRUNCATE);
    c->stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    c->go = CreateEvent(NULL, FALSE, FALSE, NULL);
    c->ready = 0;
    c->result = RES_NONE;
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
    char buf[96];
    int n = sprintf_s(buf, "%ld %ld %ld %ld\r\n", r.left, r.top, r.right - r.left, r.bottom - r.top);
    std::wstring path = ConfigDir(true) + L"\\window.txt";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote;
    WriteFile(f, buf, (DWORD)n, &wrote, NULL);
    CloseHandle(f);
}

// Loads the saved rectangle and makes it fit the work area of the monitor it is nearest to, so a
// changed screen layout or resolution never leaves the window off screen or larger than the screen.
static bool LoadWindowRect(RECT* out)
{
    std::wstring path = ConfigDir(false) + L"\\window.txt";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[128] = { 0 };
    DWORD got = 0;
    ReadFile(f, buf, sizeof buf - 1, &got, NULL);
    CloseHandle(f);
    long x, y, w, hgt;
    if (sscanf_s(buf, "%ld %ld %ld %ld", &x, &y, &w, &hgt) != 4) return false;
    if (w < 100 || hgt < 100 || w > 20000 || hgt > 20000) return false;

    RECT r = { x, y, x + w, y + hgt };
    MONITORINFO mi = { sizeof mi };
    if (!GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    const RECT& wa = mi.rcWork;
    long maxW = wa.right - wa.left, maxH = wa.bottom - wa.top;
    if (w > maxW) w = maxW;
    if (hgt > maxH) hgt = maxH;
    if (x + w > wa.right) x = wa.right - w;
    if (y + hgt > wa.bottom) y = wa.bottom - hgt;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    out->left = x; out->top = y; out->right = x + w; out->bottom = y + hgt;
    return true;
}

static std::wstring Trim(const std::wstring& s)
{
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

static bool LoadAddresses(std::vector<std::wstring>& out)
{
    std::wstring path = ConfigPath(false);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string bytes;
    char buf[4096]; DWORD got;
    while (ReadFile(f, buf, sizeof buf, &got, NULL) && got) bytes.append(buf, got);
    CloseHandle(f);

    size_t start = 0;
    while (start <= bytes.size()) {
        size_t nl = bytes.find('\n', start);
        if (nl == std::string::npos) nl = bytes.size();
        std::string line = bytes.substr(start, nl - start);
        start = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        int wn = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), (int)line.size(), NULL, 0);
        std::wstring w(wn, 0);
        MultiByteToWideChar(CP_UTF8, 0, line.c_str(), (int)line.size(), &w[0], wn);
        w = Trim(w);
        if (!w.empty()) out.push_back(w);
    }
    return true;
}

static void SaveAddresses()
{
    std::string bytes;
    for (Row* r : g_rows) {
        if (r->addr.empty()) continue;
        int n = WideCharToMultiByte(CP_UTF8, 0, r->addr.c_str(), (int)r->addr.size(), NULL, 0, NULL, NULL);
        std::string a(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, r->addr.c_str(), (int)r->addr.size(), &a[0], n, NULL, NULL);
        bytes += a; bytes += "\r\n";
    }
    std::wstring path = ConfigPath(true);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote;
    if (!bytes.empty()) WriteFile(f, bytes.data(), (DWORD)bytes.size(), &wrote, NULL);
    CloseHandle(f);
}

// ---------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------

struct Cols { int x[NCOL], w[NCOL]; };

static Cols ComputeCols(int clientW)
{
    Cols c;
    int fixed = 0;
    for (int i = 0; i < NCOL; i++) {
        c.w[i] = S(g_colBase[i]);
        if (i != C_GRAPH) fixed += c.w[i];
    }
    int avail = clientW - 2 * S(MARGIN) - (NCOL - 1) * S(GAP) - S(GRAPH_GAP) - fixed;
    if (avail > c.w[C_GRAPH]) c.w[C_GRAPH] = avail;
    c.x[0] = S(MARGIN);
    for (int i = 1; i < NCOL; i++) c.x[i] = c.x[i - 1] + c.w[i - 1] + S(GAP) + (i == C_GRAPH ? S(GRAPH_GAP) : 0);
    return c;
}

static int MinContentWidth()
{
    int w = 2 * S(MARGIN) + (NCOL - 1) * S(GAP) + S(GRAPH_GAP);
    for (int i = 0; i < NCOL; i++) w += S(g_colBase[i]);
    return w;
}

static int LogoTop()    { return S(MARGIN) / 2; }
static int HeaderTop()  { return S(MARGIN) + S(LOGO_H); }
static int RowsTop()    { return HeaderTop() + S(HDR_H) + S(MARGIN) / 2; }
static int RowTop(size_t i) { return RowsTop() + (int)i * S(ROW_H); }
static int ContentHeightFor(int rows) { return RowsTop() + rows * S(ROW_H) + S(MARGIN); }
static int ContentHeight() { return ContentHeightFor((int)g_rows.size()); }

static void Layout()
{
    RECT rc; GetClientRect(g_hwnd, &rc);
    Cols c = ComputeCols(rc.right);
    int ch = S(CTRL_H);
    HDWP dwp = BeginDeferWindowPos((int)g_rows.size() * 8);
    for (size_t i = 0; i < g_rows.size(); i++) {
        Row* r = g_rows[i];
        int y = RowTop(i) - g_scrollY;
        int yc = y + (S(ROW_H) - ch) / 2;
        HWND ctl[NCOL] = { r->edit, r->cur, r->mn, r->med, r->avg, r->mx, r->graph, r->del };
        for (int k = 0; k < NCOL; k++) {
            int cy = yc, chh = ch;
            if (k == C_GRAPH) { cy = y + S(3); chh = S(ROW_H) - S(6); }
            if (k == C_DEL)   { cy = y + (S(ROW_H) - c.w[C_DEL]) / 2; chh = c.w[C_DEL]; }
            dwp = DeferWindowPos(dwp, ctl[k], NULL, c.x[k], cy, c.w[k], chh, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    EndDeferWindowPos(dwp);
    InvalidateRect(g_hwnd, NULL, TRUE);
}

static void UpdateScroll()
{
    RECT rc; GetClientRect(g_hwnd, &rc);
    SCROLLINFO si = { sizeof si };
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = ContentHeight() - 1;
    si.nPage = rc.bottom;
    si.nPos = g_scrollY;
    SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);
    GetScrollInfo(g_hwnd, SB_VERT, &si);
    if (si.nPos != g_scrollY) { g_scrollY = si.nPos; Layout(); }
}

static void SetScrollY(int y)
{
    SCROLLINFO si = { sizeof si };
    si.fMask = SIF_POS;
    si.nPos = y;
    SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);
    GetScrollInfo(g_hwnd, SB_VERT, &si);
    if (si.nPos != g_scrollY) { g_scrollY = si.nPos; Layout(); }
}

static void EnsureRowVisible(size_t i)
{
    RECT rc; GetClientRect(g_hwnd, &rc);
    int top = RowTop(i), bottom = top + S(ROW_H);
    if (top - g_scrollY < 0) SetScrollY(i == 0 ? 0 : top - S(MARGIN));
    else if (bottom - g_scrollY > rc.bottom) SetScrollY(bottom + S(MARGIN) - rc.bottom);
}

// ---------------------------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------------------------

static Row* FindRowById(int id)
{
    for (Row* r : g_rows) if (r->id == id) return r;
    return NULL;
}
static size_t IndexOfEdit(HWND edit)
{
    for (size_t i = 0; i < g_rows.size(); i++) if (g_rows[i]->edit == edit) return i;
    return (size_t)-1;
}

static void ResetStats(Row* r)
{
    r->samples.clear();
    r->ringCount = r->ringHead = 0;
    r->last = RES_NONE;
}

static void PushRing(Row* r, int v)
{
    r->ring[r->ringHead] = v;
    r->ringHead = (r->ringHead + 1) % RING_N;
    if (r->ringCount < RING_N) r->ringCount++;
}

// Formats a millisecond value with precision that suits its magnitude: 0.37, 4.21, 23.5, 187.
static void FormatMs(double ms, wchar_t* out, size_t n)
{
    if (ms < 10.0)       swprintf_s(out, n, L"%.2f", ms);
    else if (ms < 100.0) swprintf_s(out, n, L"%.1f", ms);
    else                 swprintf_s(out, n, L"%.0f", ms);
}

static void UpdateLabels(Row* r)
{
    wchar_t b[64], vs[32];
    if (r->last == RES_NONE)         wcscpy_s(b, L"-");
    else if (r->last == RES_NOHOST)  wcscpy_s(b, L"no host");
    else if (r->last == RES_TIMEOUT) wcscpy_s(b, L"timeout");
    else { FormatMs((double)r->last / 1000.0, vs, 32); swprintf_s(b, L"%s ms", vs); }
    SetWindowTextW(r->cur, b);

    if (r->samples.empty()) {
        SetWindowTextW(r->mn, L"-"); SetWindowTextW(r->med, L"-");
        SetWindowTextW(r->avg, L"-"); SetWindowTextW(r->mx, L"-");
        return;
    }
    double mn = r->samples[0], mx = r->samples[0], sum = 0;
    for (double v : r->samples) { if (v < mn) mn = v; if (v > mx) mx = v; sum += v; }
    double avg = sum / (double)r->samples.size();
    std::vector<double> tmp(r->samples);
    size_t n = tmp.size();
    std::nth_element(tmp.begin(), tmp.begin() + n / 2, tmp.end());
    double med = tmp[n / 2];
    if ((n % 2) == 0) {
        double lo = *std::max_element(tmp.begin(), tmp.begin() + n / 2);
        med = (lo + med) / 2.0;
    }
    FormatMs(mn, b, 64);  SetWindowTextW(r->mn, b);
    FormatMs(med, b, 64); SetWindowTextW(r->med, b);
    FormatMs(avg, b, 64); SetWindowTextW(r->avg, b);
    FormatMs(mx, b, 64);  SetWindowTextW(r->mx, b);
}

// res: microseconds (>= 0) or a RES_* code. Samples are kept in ms, the ring in microseconds.
static void RecordResult(Row* r, LPARAM res)
{
    r->last = res;
    if (res >= 0) { r->samples.push_back((double)res / 1000.0); PushRing(r, (int)res); }
    else if (res == RES_TIMEOUT) PushRing(r, -1);
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

static Row* AddRow(const std::wstring& addr)
{
    Row* r = new Row;
    r->id = g_nextRowId++;
    r->addr = addr;
    r->ctx = NULL;
    ResetStats(r);
    HINSTANCE hi = GetModuleHandle(NULL);

    r->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", addr.c_str(),
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT,
                              0, 0, 10, 10, g_hwnd, NULL, hi, NULL);
    SendMessage(r->edit, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessage(r->edit, EM_SETCUEBANNER, FALSE, (LPARAM)L"host or IP address");
    WNDPROC old = (WNDPROC)SetWindowLongPtrW(r->edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
    if (!g_oldEditProc) g_oldEditProc = old;

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
    HWND ctl[NCOL] = { r->edit, r->cur, r->mn, r->med, r->avg, r->mx, r->graph, r->del };
    for (HWND h : ctl) DestroyWindow(h);
    g_rows.erase(std::find(g_rows.begin(), g_rows.end(), r));
    delete r;
}

// The list always ends with an empty row so it is obvious where a new address goes.
static void EnsureTrailingEmptyRow()
{
    if (g_rows.empty() || !g_rows.back()->addr.empty()) {
        AddRow(L"");
        UpdateScroll();
        Layout();
    }
}

// Reads the edit box into the row; restarts pinging if the address changed. Returns true if changed.
static bool ApplyEdit(Row* r)
{
    wchar_t buf[512];
    GetWindowTextW(r->edit, buf, 512);
    std::wstring a = Trim(buf);
    if (a == r->addr) return false;
    r->addr = a;
    StopPing(r);
    ResetStats(r);
    UpdateLabels(r);
    InvalidateRect(r->graph, NULL, FALSE);
    StartPing(r);
    SaveAddresses();
    return true;
}

static void FocusEdit(size_t i)
{
    if (i >= g_rows.size()) return;
    EnsureRowVisible(i);
    SetFocus(g_rows[i]->edit);
    SendMessage(g_rows[i]->edit, EM_SETSEL, 0, -1);
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
            size_t i = IndexOfEdit(h);
            if (i != (size_t)-1) SetWindowTextW(h, g_rows[i]->addr.c_str());
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

// Rounds a microsecond value up to a readable axis maximum (0.1 ms, 0.2, 0.5, 1, 2, 5, 10 ... 10 s).
static int NiceCeil(int v)
{
    static const int steps[] = { 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000,
                                 200000, 500000, 1000000, 2000000, 5000000, 10000000 };
    for (int s : steps) if (v <= s) return s;
    return ((v + 9999999) / 10000000) * 10000000;
}

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

    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(mdc, &rc, white);

    RECT plot = { 1, 1, W - 1, H - 1 };
    int pw = plot.right - plot.left, ph = plot.bottom - plot.top;

    int maxv = 0;
    for (int i = 0; i < r->ringCount; i++) {
        int idx = (r->ringHead + RING_N - r->ringCount + i) % RING_N;
        if (r->ring[idx] > maxv) maxv = r->ring[idx];
    }
    int nice = NiceCeil(maxv < 1 ? 1000 : maxv);   // 1 ms scale until there is data

    // Grid lines at 25 / 50 / 75 %.
    HPEN grid = CreatePen(PS_SOLID, 1, RGB(228, 228, 228));
    HGDIOBJ oldPen = SelectObject(mdc, grid);
    for (int q = 1; q < 4; q++) {
        int y = plot.bottom - ph * q / 4;
        MoveToEx(mdc, plot.left, y, NULL); LineTo(mdc, plot.right, y);
    }
    SelectObject(mdc, oldPen);
    DeleteObject(grid);

    HBRUSH blue = CreateSolidBrush(RGB(58, 128, 214));
    HBRUSH pink = CreateSolidBrush(RGB(255, 214, 214));
    HBRUSH red  = CreateSolidBrush(RGB(222, 54, 54));
    for (int i = 0; i < r->ringCount; i++) {
        int idx = (r->ringHead + RING_N - r->ringCount + i) % RING_N;
        int slot = RING_N - r->ringCount + i;             // newest sample at the right edge
        int x0 = plot.left + slot * pw / RING_N;
        int x1 = plot.left + (slot + 1) * pw / RING_N;
        if (x1 <= x0) x1 = x0 + 1;
        int v = r->ring[idx];
        RECT bar;
        if (v >= 0) {
            int y = plot.bottom - (int)((long long)v * ph / nice);
            if (y >= plot.bottom) y = plot.bottom - 1;
            if (y < plot.top) y = plot.top;
            bar.left = x0; bar.right = x1; bar.top = y; bar.bottom = plot.bottom;
            FillRect(mdc, &bar, blue);
        } else {
            bar.left = x0; bar.right = x1; bar.top = plot.top; bar.bottom = plot.bottom;
            FillRect(mdc, &bar, pink);
            bar.bottom = plot.top + S(3);
            FillRect(mdc, &bar, red);
        }
    }
    DeleteObject(blue); DeleteObject(pink); DeleteObject(red);

    // Scale label, top-left.
    wchar_t lbl[32], lv[32];
    FormatMs((double)nice / 1000.0, lv, 32);
    swprintf_s(lbl, L"%s ms", lv);
    HGDIOBJ oldFont = SelectObject(mdc, g_fontSmall);
    SetBkMode(mdc, OPAQUE);                 // white backing keeps the label legible over tall bars
    SetBkColor(mdc, RGB(255, 255, 255));
    SetTextColor(mdc, RGB(110, 110, 110));
    RECT lr = { plot.left + S(3), plot.top + S(1), plot.right, plot.bottom };
    DrawTextW(mdc, lbl, -1, &lr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(mdc, oldFont);

    HBRUSH border = CreateSolidBrush(RGB(160, 160, 160));
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
    FillRect(mdc, &rc, GetSysColorBrush(COLOR_BTNFACE));

    int d = (W < H ? W : H);
    int cx = W / 2, cy = H / 2, rad = d / 2;
    COLORREF col = st->down ? RGB(170, 30, 30) : (st->hover ? RGB(240, 70, 70) : RGB(214, 44, 44));
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
    MoveToEx(mdc, cx - ins, cy - ins, NULL); LineTo(mdc, cx + ins + 1, cy + ins + 1);
    MoveToEx(mdc, cx - ins, cy + ins, NULL); LineTo(mdc, cx + ins + 1, cy - ins - 1);
    SelectObject(mdc, oldP);
    DeleteObject(xp);

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
// Main window
// ---------------------------------------------------------------------------------------------

static void CreateFonts()
{
    if (g_font) DeleteObject(g_font);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    if (g_fontLogo) DeleteObject(g_fontLogo);
    g_fontLogo = CreateFontW(-MulDiv(FONT_LOGO_PT, (int)g_dpi, 72), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_font = CreateFontW(-MulDiv(FONT_PT, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_fontSmall = CreateFontW(-MulDiv(FONT_SMALL_PT, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static void ApplyFonts()
{
    for (Row* r : g_rows) {
        HWND ctl[6] = { r->edit, r->cur, r->mn, r->med, r->avg, r->mx };
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
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));

    // Title, large and bold, aligned with the Address column.
    HGDIOBJ oldFont = SelectObject(hdc, g_fontLogo);
    RECT lr = { c.x[0], LogoTop() - g_scrollY, rc.right, LogoTop() - g_scrollY + S(LOGO_H) };
    DrawTextW(hdc, L"MultiPing", -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(hdc, g_font);
    int y = HeaderTop() - g_scrollY;
    for (int k = 0; k < NCOL; k++) {
        RECT r = { c.x[k], y, c.x[k] + c.w[k], y + S(HDR_H) };
        UINT align = (k == C_ADDR || k == C_GRAPH) ? DT_LEFT : DT_RIGHT;
        DrawTextW(hdc, g_colNames[k], -1, &r, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(170, 170, 170));
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
    case SB_LINEUP:        pos -= S(ROW_H) / 2; break;
    case SB_LINEDOWN:      pos += S(ROW_H) / 2; break;
    case SB_PAGEUP:        pos -= (int)si.nPage; break;
    case SB_PAGEDOWN:      pos += (int)si.nPage; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: pos = si.nTrackPos; break;
    case SB_TOP:           pos = 0; break;
    case SB_BOTTOM:        pos = si.nMax; break;
    default: return;
    }
    SetScrollY(pos);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        g_hwnd = h;
        if (g_pGetDpiForWindow) g_dpi = g_pGetDpiForWindow(h);
        CreateFonts();
        std::vector<std::wstring> addrs;
        LoadAddresses(addrs);
        for (const std::wstring& a : addrs) AddRow(a);
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
        RECT r = { 0, 0, MinContentWidth() + GetSystemMetrics(SM_CXVSCROLL), S(ROW_H) * 2 + RowsTop() };
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        mmi->ptMinTrackSize.x = r.right - r.left;
        mmi->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_PAINT:
        PaintMain(h);
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)w, TRANSPARENT);
        SetTextColor((HDC)w, GetSysColor(COLOR_BTNTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_VSCROLL:
        OnVScroll(w);
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        SetScrollY(g_scrollY - delta * S(ROW_H) / WHEEL_DELTA);
        return 0;
    }
    case WM_SETFOCUS:
        if (!g_rows.empty()) SetFocus(g_rows[0]->edit);
        return 0;

    case WM_TIMER: {
        // One tick for every row: take last round's result, then release the next ping. All graphs
        // and labels therefore change in the same instant.
        if (g_closing || w != TIMER_TICK) return 0;
        for (Row* r : g_rows) {
            PingCtx* c = r->ctx;
            if (!c) continue;
            if (c->inflight) {
                if (InterlockedCompareExchange(&c->ready, 0, 1) != 1) continue;  // still waiting (rare)
                RecordResult(r, (LPARAM)c->result);
                c->inflight = false;
            }
            c->inflight = true;
            SetEvent(c->go);
        }
        return 0;
    }
    case WM_APP_APPLY: {
        if (g_closing) return 0;
        size_t i = IndexOfEdit((HWND)l);
        if (i != (size_t)-1 && ApplyEdit(g_rows[i])) EnsureTrailingEmptyRow();
        return 0;
    }
    case WM_APP_ENTER: {
        if (g_closing) return 0;
        size_t i = IndexOfEdit((HWND)l);
        if (i == (size_t)-1) return 0;
        ApplyEdit(g_rows[i]);
        EnsureTrailingEmptyRow();           // a filled-in last row gets a fresh empty row after it
        if (i + 1 < g_rows.size()) FocusEdit(i + 1);
        return 0;
    }
    case WM_APP_TAB: {
        if (g_closing) return 0;
        size_t i = IndexOfEdit((HWND)l);
        if (i == (size_t)-1 || g_rows.empty()) return 0;
        size_t n = g_rows.size();
        FocusEdit(w ? (i + n - 1) % n : (i + 1) % n);
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

    wc.style = CS_HREDRAW;
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = L"MultiPingMain";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    RECT r = { 0, 0, MinContentWidth() + S(95), ContentHeightFor(8) };   // 8 rows, graph about 335 px wide
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
