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

// MultiPingCore.h - what every MultiPing front end shares: the timing, the layout, the statistics
// and how they read, the charts' geometry, and the files the addresses and the window's place are
// kept in. No toolkit and no system calls here: the Win32, GTK, Qt and Cocoa front ends draw and
// ping, this decides what and where. Header-only C++14, so each front end stays one source file
// and the XP toolsets build it too.

#ifndef MULTIPING_CORE_H
#define MULTIPING_CORE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

namespace mp {

// ---------------------------------------------------------------------------------------------
// Timing and results
// ---------------------------------------------------------------------------------------------

// A ping's result is its round trip in microseconds (>= 0) or one of these.
enum { RES_NONE = -3, RES_NOHOST = -2, RES_TIMEOUT = -1 };

enum {
    RING_N           = 60,     // seconds of history shown in the graph
    PING_INTERVAL_MS = 1000,   // UI tick: commits last results and releases the next round of pings
    PING_TIMEOUT_MS  = 800,    // under the interval; a result not back by the tick counts as a timeout
    DRAG_SCROLL_MS   = 100,    // a row dragged above or below the rows shown scrolls the list a row this often
};

// ---------------------------------------------------------------------------------------------
// Look: fonts, metrics, colours, texts
// ---------------------------------------------------------------------------------------------

// Fonts in points, at 96 dpi (a point is 4/3 of a 96-dpi pixel).
enum {
    FONT_PT       = 14,
    FONT_SMALL_PT = 9,     // the graph's scale label
    FONT_LOGO_PT  = 24,    // "MultiPing" title at the top, bold
    FONT_INFO_PT  = 10,    // copyright / revision block beside the title (under half the title size)
};

// Layout metrics in 96-dpi pixels.
enum {
    MARGIN        = 18,    // outer padding around all content
    GAP           = 6,
    ROW_H         = 56,
    HDR_H         = 30,
    CTRL_H        = 32,
    LOGO_H        = 44,    // height reserved for the title
    TITLE_ICON_PX = 36,    // the app icon drawn beside the title where no emoji font exists
    TITLE_EMOJI_PX  = 35,  // the title emoji's height, as Windows draws it beside the 24 pt title,
    TITLE_EMOJI_GAP = 14,  // from the title's end, its middle a pixel above the capitals' middle
    W_GRIP        = 16,    // the handle that moves a row up and down
    W_ADDR        = 193,
    W_NOTES       = 180,
    W_CUR         = 110,
    W_STAT        = 92,
    W_CHART_MIN   = 97,    // each of the two charts; with the gap between them, 200 together
    GRAPH_GAP     = 28,    // extra space between the Max column and the charts
    DEL_GAP       = 10,    // extra space between the charts and the delete button
    W_DEL         = 24,
    W_EXTRA       = 135,   // the first window's width beyond the minimum: two charts about 165 px wide
    GRIP_DOT      = 3,     // the handle's six dots, two across and three down,
    GRIP_PITCH    = 6,     // this far apart
    DEFAULT_ROWS  = 8,     // the first window fits this many rows
    MIN_ROWS      = 2,     // the window is never made shorter than this many
};

enum { C_GRIP, C_ADDR, C_NOTES, C_CUR, C_MIN, C_MED, C_AVG, C_MAX, C_GRAPH, C_HIST, C_DEL, NCOL };
static const int kColBase[NCOL] = { W_GRIP, W_ADDR, W_NOTES, W_CUR, W_STAT, W_STAT, W_STAT, W_STAT, W_CHART_MIN, W_CHART_MIN, W_DEL };
static const char* const kColNames[NCOL] = { "", "Address", "Notes", "Current", "Min", "Median", "Avg", "Max", "Last 60 s", "Histogram", "" };
// Columns whose header and values are left aligned; the rest are right aligned.
inline bool LeftAligned(int col) { return col == C_ADDR || col == C_NOTES || col == C_GRAPH || col == C_HIST; }

struct Rgb { unsigned char r, g, b; };

// The colours the front ends draw with themselves, one set for a light system theme and one for a
// dark one. The window, its text and the address boxes keep the platform's own colours (Windows,
// whose classic controls have no dark theme, sets them in MultiPing.cpp). The graph is as light as
// the address boxes in the light theme and as dark as them in the dark one.
struct Palette {
    Rgb graphBack;
    Rgb graphGrid;
    Rgb graphBar;
    Rgb graphTimeout;      // a timed-out second, the whole height
    Rgb graphTimeoutTop;   // and the strip along its top
    Rgb graphLabel;        // the scale label, on a backing of graphBack
    Rgb graphBorder;
    Rgb headerLine;
    Rgb infoText;
    Rgb del, delHover, delDown;
    Rgb grip, gripActive;  // a row's handle, and the one under the pointer or being dragged
};
static const Palette kLight = {
    { 255, 255, 255 }, { 228, 228, 228 }, { 58, 128, 214 }, { 255, 214, 214 }, { 222, 54, 54 },
    { 110, 110, 110 }, { 160, 160, 160 }, { 170, 170, 170 }, { 80, 80, 80 },
    { 214, 44, 44 }, { 240, 70, 70 }, { 170, 30, 30 },
    { 168, 168, 168 }, { 96, 96, 96 },
};
static const Palette kDark = {
    { 20, 20, 20 }, { 48, 48, 48 }, { 58, 128, 214 }, { 84, 30, 32 }, { 222, 54, 54 },
    { 150, 150, 150 }, { 96, 96, 96 }, { 84, 84, 84 }, { 175, 175, 175 },
    { 214, 44, 44 }, { 240, 70, 70 }, { 170, 30, 30 },
    { 100, 100, 100 }, { 196, 196, 196 },
};
inline const Palette& PaletteFor(bool dark) { return dark ? kDark : kLight; }

// Texts in UTF-8.
static const char* const kTitle      = "MultiPing";
static const char* const kTitleEmoji = "\xF0\x9F\x91\x8B";   // U+1F44B waving hand
static const char* const kCopyright  = "Copyright \xC2\xA9 2026 Max Vilimpoc";
static const char* const kCue        = "host or IP address";

// "Build 2026-10-17 from git abc1234", with the compiler's "Mmm dd yyyy" __DATE__ turned into ISO form.
inline std::string BuildInfo(const char* date, const char* rev)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int mon = 0;
    for (int i = 0; i < 12; i++) if (strncmp(months + 3 * i, date, 3) == 0) { mon = i + 1; break; }
    int day = (date[4] == ' ' ? 0 : (date[4] - '0') * 10) + (date[5] - '0');
    int year = atoi(date + 7);
    char text[96];
    snprintf(text, sizeof text, "Build %04d-%02d-%02d from git %s", year, mon, day, rev);
    return text;
}

// ---------------------------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------------------------

// Formats a millisecond value with precision that suits its magnitude: 0.37, 4.21, 23.5, 187.
inline std::string FormatMs(double ms)
{
    char b[32];
    if (ms < 10.0)       snprintf(b, sizeof b, "%.2f", ms);
    else if (ms < 100.0) snprintf(b, sizeof b, "%.1f", ms);
    else                 snprintf(b, sizeof b, "%.0f", ms);
    return b;
}

// The round trips are counted in bins, one for each value the columns can show (0.01 ms steps
// under 10 ms, 0.1 ms under 100 ms, 1 ms under a second, and one bin for anything longer), each
// holding the replies that show as that value, so a session takes the same memory however long
// it runs, and the median reads as it would from every reply (give or take a unit of the last
// digit when there are two middle replies to average).
enum { NBINS = 1000 + 900 + 900 + 1 };

// The bin that counts a round trip of `us` microseconds.
inline int BinOf(int us)
{
    if (us < 9995)   return (us + 5) / 10;
    if (us < 99950)  return 1000 + (us - 10000 + 50) / 100;
    if (us < 999500) return 1900 + (us - 100000 + 500) / 1000;
    return NBINS - 1;
}

// The round trip bin i stands for, in ms: what its replies show as.
inline double BinMs(int i)
{
    if (i < 1000) return i * 0.01;
    if (i < 1900) return 10.0 + (i - 1000) * 0.1;
    return 100.0 + (i - 1900);
}

// The shortest round trip bin i counts, in microseconds; the bin ends where BinLow(i + 1) begins
// (the last one, counting every reply of a second or more, is taken as a millisecond wide).
inline int BinLow(int i)
{
    if (i <= 0)    return 0;
    if (i < 1000)  return i * 10 - 5;
    if (i == 1000) return 9995;
    if (i < 1900)  return 10000 + (i - 1000) * 100 - 50;
    if (i == 1900) return 99950;
    return 100000 + (i - 1900) * 1000 - 500;
}

// One address's session: every reply's round trip counted in its bin, for the Median column and
// the histogram, the running Min, Avg and Max, and the last RING_N results for the graph.
struct Stats {
    unsigned bins[NBINS];           // the replies this session, counted by round trip
    unsigned count;                 // all of them
    int minUs, maxUs;
    long long sumUs;
    int ring[RING_N];               // last RING_N results: >= 0 round trip in us, -1 timeout
    int ringCount, ringHead;
    int last;                       // the latest result: us or RES_*

    Stats() { Reset(); }

    void Reset()
    {
        memset(bins, 0, sizeof bins);
        count = 0;
        minUs = maxUs = 0;
        sumUs = 0;
        ringCount = ringHead = 0;
        last = RES_NONE;
    }

    // res: microseconds (>= 0) or a RES_* code. Not finding the host leaves the graph alone.
    void Record(int res)
    {
        last = res;
        if (res >= 0) {
            if (count == 0 || res < minUs) minUs = res;
            if (count == 0 || res > maxUs) maxUs = res;
            sumUs += res;
            count++;
            bins[BinOf(res)]++;
            Push(res);
        } else if (res == RES_TIMEOUT) Push(-1);
    }

    // The i-th of the ringCount results in the graph, oldest first.
    int At(int i) const { return ring[(ringHead + RING_N - ringCount + i) % RING_N]; }

    // The bin holding the k-th shortest reply, k from 0 and below count.
    int BinAtRank(unsigned k) const
    {
        unsigned seen = 0;
        for (int i = 0; i < NBINS; i++) { seen += bins[i]; if (seen > k) return i; }
        return NBINS - 1;
    }

    // The Current column.
    std::string Current() const
    {
        if (last == RES_NONE)    return "-";
        if (last == RES_NOHOST)  return "no host";
        if (last == RES_TIMEOUT) return "timeout";
        return FormatMs((double)last / 1000.0) + " ms";
    }

    // The Min, Median, Avg and Max columns, "-" before the first reply.
    void Summary(std::string out[4]) const
    {
        if (count == 0) { for (int i = 0; i < 4; i++) out[i] = "-"; return; }
        double med = BinMs(BinAtRank(count / 2));
        if ((count % 2) == 0) med = (BinMs(BinAtRank(count / 2 - 1)) + med) / 2.0;
        out[0] = FormatMs((double)minUs / 1000.0);
        out[1] = FormatMs(med);
        out[2] = FormatMs((double)sumUs / (double)count / 1000.0);
        out[3] = FormatMs((double)maxUs / 1000.0);
    }

private:
    void Push(int v)
    {
        ring[ringHead] = v;
        ringHead = (ringHead + 1) % RING_N;
        if (ringCount < RING_N) ringCount++;
    }
};

// ---------------------------------------------------------------------------------------------
// Layout, in pixels at the given dpi (96 where the toolkit scales by itself)
// ---------------------------------------------------------------------------------------------

struct Rect { int x, y, w, h; };
struct Cols { int x[NCOL], w[NCOL]; };

struct Metrics {
    int dpi;
    explicit Metrics(int dpi_ = 96) : dpi(dpi_) {}

    // A 96-dpi length at this dpi, rounded to the nearest pixel (as Win32's MulDiv does).
    int S(int v) const { return (int)(((long long)v * dpi + (v >= 0 ? 48 : -48)) / 96); }
    // A size in points as pixels at this dpi.
    int Pt(int pt) const { return (int)(((long long)pt * dpi + 36) / 72); }

    // The columns across a client area clientW wide: all fixed but the two charts, which share
    // the rest equally.
    Cols Columns(int clientW) const
    {
        Cols c;
        int fixed = 0;
        for (int i = 0; i < NCOL; i++) {
            c.w[i] = S(kColBase[i]);
            if (i != C_GRAPH && i != C_HIST) fixed += c.w[i];
        }
        int avail = clientW - 2 * S(MARGIN) - (NCOL - 1) * S(GAP) - S(GRAPH_GAP) - S(DEL_GAP) - fixed;
        if (avail > c.w[C_GRAPH] + c.w[C_HIST]) { c.w[C_GRAPH] = avail / 2; c.w[C_HIST] = avail - avail / 2; }
        c.x[0] = S(MARGIN);
        for (int i = 1; i < NCOL; i++)
            c.x[i] = c.x[i - 1] + c.w[i - 1] + S(GAP) + (i == C_GRAPH ? S(GRAPH_GAP) : i == C_DEL ? S(DEL_GAP) : 0);
        return c;
    }

    int MinContentWidth() const
    {
        int w = 2 * S(MARGIN) + (NCOL - 1) * S(GAP) + S(GRAPH_GAP) + S(DEL_GAP);
        for (int i = 0; i < NCOL; i++) w += S(kColBase[i]);
        return w;
    }
    int DefaultWidth() const { return MinContentWidth() + S(W_EXTRA); }

    int LogoTop() const    { return S(MARGIN) / 2; }
    int HeaderTop() const  { return LogoTop() + S(LOGO_H) + S(2); }
    int RowsTop() const    { return HeaderTop() + S(HDR_H) + S(MARGIN) / 2; }
    int ContentHeightFor(int rows) const { return RowsTop() + rows * S(ROW_H) + S(MARGIN); }

    // Scrolling is by whole rows: the title and header stay put, and only the rows that fit
    // completely in a client area clientH high are shown, at least one.
    int RowsThatFit(int clientH) const
    {
        int fit = (clientH - RowsTop()) / S(ROW_H);
        return fit < 1 ? 1 : fit;
    }
    // The top of the row shown `slot` rows below the first one shown.
    int RowTop(int slot) const { return RowsTop() + slot * S(ROW_H); }

    // Where column `col` of the row whose top is rowTop goes: the controls centred in the row,
    // the charts nearly its full height, the delete button a square.
    Rect Cell(const Cols& c, int col, int rowTop) const
    {
        Rect r = { c.x[col], rowTop + (S(ROW_H) - S(CTRL_H)) / 2, c.w[col], S(CTRL_H) };
        if (col == C_GRAPH || col == C_HIST) { r.y = rowTop + S(3); r.h = S(ROW_H) - S(6); }
        if (col == C_DEL)   { r.y = rowTop + (S(ROW_H) - c.w[C_DEL]) / 2; r.h = c.w[C_DEL]; }
        return r;
    }
};

// The first row shown, kept so that the last row is at the bottom when everything after it fits.
inline int ClampFirstRow(int first, int rows, int fit)
{
    int maxFirst = rows > fit ? rows - fit : 0;
    if (first > maxFirst) first = maxFirst;
    if (first < 0) first = 0;
    return first;
}

// ---------------------------------------------------------------------------------------------
// Moving a row by its handle
// ---------------------------------------------------------------------------------------------

// While a row is dragged by its handle it moves, as the pointer goes, to where the row under the
// pointer is: DragTarget is that row's index for a pointer at y, among the first `movable` of the
// `rows` rows (the empty row at the end stays last). Above or below the rows shown, it is the
// first or the last row shown, and with `scroll` set the list also moves a row towards the
// pointer: the front ends set it on a timer's tick while the pointer stays out there.
inline bool DragOutside(const Metrics& M, int y, int fit)
{
    return y < M.RowsTop() || y >= M.RowTop(fit);
}

inline int DragTarget(const Metrics& M, int y, int* first, int fit, int rows, int movable, bool scroll)
{
    int t;
    if (y < M.RowsTop()) {
        if (scroll && *first > 0) (*first)--;
        t = *first;
    } else if (y >= M.RowTop(fit)) {
        if (scroll) *first = ClampFirstRow(*first + 1, rows, fit);
        t = *first + fit - 1;
    } else {
        t = *first + (y - M.RowsTop()) / M.S(ROW_H);
    }
    return t < movable - 1 ? t : movable - 1;
}

// Moves v[from] to index `to`; the items between close up behind it.
template <class T> void MoveItem(std::vector<T>& v, size_t from, size_t to)
{
    if (from < to) std::rotate(v.begin() + (long)from, v.begin() + (long)from + 1, v.begin() + (long)to + 1);
    else if (to < from) std::rotate(v.begin() + (long)to, v.begin() + (long)from, v.begin() + (long)from + 1);
}

// ---------------------------------------------------------------------------------------------
// The charts: the graph of the last 60 s and the histogram of the session
// ---------------------------------------------------------------------------------------------

// Rounds a microsecond value up to a readable axis maximum (0.1 ms, 0.2, 0.5, 1, 2, 5, 10 ... 10 s).
inline int NiceCeil(int v)
{
    static const int steps[] = { 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000,
                                 200000, 500000, 1000000, 2000000, 5000000, 10000000 };
    for (int s : steps) if (v <= s) return s;
    return ((v + 9999999) / 10000000) * 10000000;
}

// A bar from `top` down to the plot's bottom: in the graph a reply's second, or a timeout, the
// plot's whole height with a strip of the timeout colour along its top; in the histogram the
// replies of one round trip length.
struct GraphBar { int x0, x1, top; bool timeout; };

// A chart in a W by H box, inside its one-pixel border: the plot, three grid lines a pixel wide
// across it at a quarter, half and three quarters (horizontal in the graph, vertical in the
// histogram), the bars, and the scale at the top, "2.00 ms", in the left corner of the graph
// (its height) and the right corner of the histogram (its width).
struct Graph {
    Rect plot;
    Rect grid[3];
    int scaleUs;
    std::string scaleLabel;
    bool labelRight;
    std::vector<GraphBar> bars;
};

// The last RING_N results, one bar per second with the newest at the right edge, against a scale
// that holds the longest of them.
inline Graph LayoutGraph(const Stats& s, int W, int H)
{
    Graph g;
    g.plot.x = 1; g.plot.y = 1; g.plot.w = W - 2; g.plot.h = H - 2;
    int left = g.plot.x, top = g.plot.y, pw = g.plot.w, ph = g.plot.h, bottom = top + ph;
    int maxv = 0;
    for (int i = 0; i < s.ringCount; i++) if (s.At(i) > maxv) maxv = s.At(i);
    g.scaleUs = NiceCeil(maxv < 1 ? 1000 : maxv);   // 1 ms scale until there is data
    g.scaleLabel = FormatMs((double)g.scaleUs / 1000.0) + " ms";
    g.labelRight = false;
    for (int q = 1; q < 4; q++) { Rect r = { left, bottom - ph * q / 4, pw, 1 }; g.grid[q - 1] = r; }
    for (int i = 0; i < s.ringCount; i++) {
        int slot = RING_N - s.ringCount + i;                 // newest sample at the right edge
        GraphBar b;
        b.x0 = left + slot * pw / RING_N;
        b.x1 = left + (slot + 1) * pw / RING_N;
        if (b.x1 <= b.x0) b.x1 = b.x0 + 1;
        int v = s.At(i);
        b.timeout = v < 0;
        if (b.timeout) b.top = top;
        else {
            b.top = bottom - (int)((long long)v * ph / g.scaleUs);
            if (b.top >= bottom) b.top = bottom - 1;
            if (b.top < top) b.top = top;
        }
        g.bars.push_back(b);
    }
    return g;
}

// The session's replies by round trip, from 0 at the left edge to the scale at the right: a
// column of pixels for each stretch of the scale, as tall as the replies in it, the tallest the
// plot's height and any with a reply at least a pixel. The scale holds all but the longest
// hundredth of the replies, so that a few slow ones do not squash the rest into the first
// pixels; those beyond it are counted in the last column.
inline Graph LayoutHistogram(const Stats& s, int W, int H)
{
    Graph g;
    g.plot.x = 1; g.plot.y = 1; g.plot.w = W - 2; g.plot.h = H - 2;
    int left = g.plot.x, top = g.plot.y, pw = g.plot.w, ph = g.plot.h, bottom = top + ph;
    int longest = s.count ? BinLow(s.BinAtRank(s.count - 1 - s.count / 100) + 1) : 1000;
    g.scaleUs = NiceCeil(longest);
    g.scaleLabel = FormatMs((double)g.scaleUs / 1000.0) + " ms";
    g.labelRight = true;
    for (int q = 1; q < 4; q++) { Rect r = { left + pw * q / 4, top, 1, ph }; g.grid[q - 1] = r; }
    if (pw < 1 || ph < 1) return g;

    // The replies in each pixel column; a bin wider than a column is spread over those it covers.
    std::vector<double> cols((size_t)pw, 0.0);
    for (int i = 0; i < NBINS; i++) {
        if (!s.bins[i]) continue;
        double n = (double)s.bins[i];
        double xa = (double)BinLow(i) * pw / g.scaleUs, xb = (double)BinLow(i + 1) * pw / g.scaleUs;
        if (xa >= pw) { cols[(size_t)pw - 1] += n; continue; }
        for (int x = (int)xa; x < pw && x < xb; x++) {
            double lo = x > xa ? x : xa, hi = x + 1 < xb ? x + 1 : xb;
            cols[(size_t)x] += n * (hi - lo) / (xb - xa);
        }
        if (xb > pw) cols[(size_t)pw - 1] += n * (xb - pw) / (xb - xa);
    }
    double tallest = 0;
    for (double c : cols) if (c > tallest) tallest = c;
    for (int x = 0; x < pw; x++) {
        if (cols[(size_t)x] <= 0) continue;
        int t = bottom - (int)(cols[(size_t)x] * ph / tallest);
        if (t >= bottom) t = bottom - 1;
        if (t < top) t = top;
        if (!g.bars.empty() && g.bars.back().x1 == left + x && g.bars.back().top == t) { g.bars.back().x1++; continue; }
        GraphBar b = { left + x, left + x + 1, t, false };
        g.bars.push_back(b);
    }
    return g;
}

// The chart in column col: the graph in C_GRAPH, the histogram in C_HIST.
inline Graph LayoutChart(const Stats& s, int col, int W, int H)
{
    return col == C_HIST ? LayoutHistogram(s, W, H) : LayoutGraph(s, W, H);
}

// The delete button's glyph in a W by H box: a filled circle and a white X across it.
struct DeleteGlyph { float cx, cy, radius, inset, stroke; };

inline DeleteGlyph LayoutDelete(float W, float H)
{
    DeleteGlyph d;
    float side = W < H ? W : H;
    d.cx = W / 2.0f; d.cy = H / 2.0f;
    d.radius = side / 2.0f - 0.5f;
    d.stroke = side / 9.0f; if (d.stroke < 2.0f) d.stroke = 2.0f;
    d.inset = d.radius * 0.45f;   // the X runs from centre - inset to centre + inset on both axes
    return d;
}

// The handle's six square dots, two across and three down, centred in its box.
struct GripDots { Rect dot[6]; };

inline GripDots LayoutGrip(const Metrics& M, const Rect& box)
{
    GripDots g;
    int d = M.S(GRIP_DOT), pitch = M.S(GRIP_PITCH);
    int x0 = box.x + (box.w - pitch - d) / 2, y0 = box.y + (box.h - 2 * pitch - d) / 2;
    for (int i = 0; i < 6; i++) {
        Rect r = { x0 + (i % 2) * pitch, y0 + (i / 2) * pitch, d, d };
        g.dot[i] = r;
    }
    return g;
}

// ---------------------------------------------------------------------------------------------
// The files: addresses.txt (UTF-8, a row per line: the address, and after a tab the row's notes)
// and window.txt ("left top width height")
// ---------------------------------------------------------------------------------------------

inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

inline std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && IsSpace(s[a])) a++;
    while (b > a && IsSpace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

// A row as addresses.txt keeps it.
struct Entry { std::string addr, notes; };

// Text as a line of the file can hold it: tabs and line breaks become spaces, and the space at
// either end goes.
inline std::string OneLine(std::string s)
{
    for (char& c : s) if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    return Trim(s);
}

// The rows in a file's bytes, a line each: the address, then a tab and the notes when there are
// any (a file from before the notes has addresses alone). Blank lines are left out.
inline std::vector<Entry> ParseEntries(const std::string& bytes)
{
    std::vector<Entry> out;
    size_t start = 0;
    while (start <= bytes.size()) {
        size_t nl = bytes.find('\n', start);
        if (nl == std::string::npos) nl = bytes.size();
        std::string line = bytes.substr(start, nl - start);
        start = nl + 1;
        size_t tab = line.find('\t');
        Entry e = { Trim(line.substr(0, tab)), tab == std::string::npos ? std::string() : OneLine(line.substr(tab + 1)) };
        if (!e.addr.empty() || !e.notes.empty()) out.push_back(e);
    }
    return out;
}

// A file's bytes for the rows, each line followed by eol ("\r\n" on Windows, "\n" elsewhere);
// rows with neither an address nor notes are left out.
inline std::string JoinEntries(const std::vector<Entry>& rows, const char* eol)
{
    std::string bytes;
    for (const Entry& e : rows) {
        std::string notes = OneLine(e.notes);
        if (e.addr.empty() && notes.empty()) continue;
        bytes += e.addr;
        if (!notes.empty()) { bytes += '\t'; bytes += notes; }
        bytes += eol;
    }
    return bytes;
}

struct WindowRect { long x, y, w, h; };

// window.txt's rectangle; false when the file says nothing sensible.
inline bool ParseWindowRect(const std::string& text, WindowRect* r)
{
    long x, y, w, h;
    if (sscanf(text.c_str(), "%ld %ld %ld %ld", &x, &y, &w, &h) != 4) return false;
    if (w < 100 || h < 100 || w > 20000 || h > 20000) return false;
    r->x = x; r->y = y; r->w = w; r->h = h;
    return true;
}

inline std::string FormatWindowRect(const WindowRect& r, const char* eol)
{
    char b[96];
    snprintf(b, sizeof b, "%ld %ld %ld %ld%s", r.x, r.y, r.w, r.h, eol);
    return b;
}

// A saved width too narrow for the columns, saved before one was added, gives way to the default.
inline long FitSavedWidth(long w, long minW, long defaultW) { return w < minW ? defaultW : w; }

// Makes the rectangle fit a work area, so that a changed screen layout or resolution never leaves
// the window off screen or larger than the screen.
inline void FitWindowRect(WindowRect* r, long left, long top, long width, long height)
{
    if (r->w > width) r->w = width;
    if (r->h > height) r->h = height;
    if (r->x + r->w > left + width) r->x = left + width - r->w;
    if (r->y + r->h > top + height) r->y = top + height - r->h;
    if (r->x < left) r->x = left;
    if (r->y < top) r->y = top;
}

} // namespace mp

#endif
