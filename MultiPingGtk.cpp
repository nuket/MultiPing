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

// MultiPing for Linux with GTK 3 (build_linux.sh makes multiping-gtk). The window is the Windows
// one: its layout, look and behaviour come from MultiPingCore.h, the pinging and the files from
// MultiPingPosix.h. One drawing area paints everything but the address boxes and the scrollbar,
// which an overlay places on top of it exactly where the layout puts them.

#include "MultiPingPosix.h"

#include <glib-unix.h>
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#include <math.h>

using namespace mp;

// Git short revision, passed in by build_linux.sh as -DGIT_REV="abc1234".
#ifndef GIT_REV
#define GIT_REV "unknown"
#endif

// GTK's pixels are device-independent, the 96-dpi pixels of the core's metrics: GTK scales them
// for the screen the way Windows scales by its dpi.
static const Metrics M(96);

struct Row {
    int id;
    std::string addr;
    GtkWidget* entry;
    std::unique_ptr<Pinger> pinger;
    Stats stats;
};

static GtkWidget* g_window;
static GtkWidget* g_overlay;
static GtkWidget* g_area;
static GtkWidget* g_bar;
static GtkAdjustment* g_adjust;
static std::vector<std::unique_ptr<Row>> g_rows;
static int g_nextId = 1;
static int g_first;           // index of the first visible row (row-based scrolling)
static int g_hover = -1;      // the row whose delete button the pointer is over
static int g_down = -1;       // the row whose delete button is pressed
static double g_wheel;
static bool g_closing;
static bool g_sized;          // the window's content made the size asked for, once
static int g_wantW, g_wantH;  // that size
static int g_allocW, g_allocH;  // the drawing area's size when last laid out
static std::string g_uiFamily;
static cairo_surface_t* g_emoji;   // the title emoji, cut out; NULL without an emoji font
static GdkPixbuf* g_icon;

// ---------------------------------------------------------------------------------------------
// Fonts and text
// ---------------------------------------------------------------------------------------------

// The desktop's UI face at the Windows app's size: points at 96 dpi as pixels, whatever font
// scaling the desktop asks for, so the window matches the Windows one pixel for pixel.
static PangoFontDescription* UiFont(int pt, bool bold = false)
{
    PangoFontDescription* f = pango_font_description_new();
    pango_font_description_set_family(f, g_uiFamily.c_str());
    pango_font_description_set_absolute_size(f, M.Pt(pt) * PANGO_SCALE);
    if (bold) pango_font_description_set_weight(f, PANGO_WEIGHT_BOLD);
    return f;
}

// The title emoji as the font draws it, large, cut to its visible pixels, so the window can scale
// and place it the way Windows does whatever size and height the font gives it. NULL when the font
// draws nothing.
static cairo_surface_t* CroppedEmoji(const std::string& family)
{
    const int size = 256;
    cairo_surface_t* big = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t* cr = cairo_create(big);
    PangoFontDescription* f = pango_font_description_new();
    pango_font_description_set_family(f, family.c_str());
    pango_font_description_set_absolute_size(f, 128 * PANGO_SCALE);
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, f);
    pango_layout_set_text(layout, kTitleEmoji, -1);
    cairo_move_to(cr, 32, 32);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    pango_font_description_free(f);
    cairo_destroy(cr);
    cairo_surface_flush(big);

    const unsigned char* data = cairo_image_surface_get_data(big);
    int stride = cairo_image_surface_get_stride(big);
    int x0 = size, y0 = size, x1 = -1, y1 = -1;
    for (int y = 0; y < size; y++) {
        const uint32_t* line = (const uint32_t*)(data + y * stride);
        for (int x = 0; x < size; x++) {
            if ((line[x] >> 24) < 16) continue;   // alpha, the top byte of cairo's ARGB32
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    cairo_surface_t* cut = NULL;
    if (x1 >= 0) {
        cut = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, x1 - x0 + 1, y1 - y0 + 1);
        cairo_t* c = cairo_create(cut);
        cairo_set_source_surface(c, big, -x0, -y0);
        cairo_paint(c);
        cairo_destroy(c);
    }
    cairo_surface_destroy(big);
    return cut;
}

static void FindFonts()
{
    gchar* name = NULL;
    g_object_get(gtk_settings_get_default(), "gtk-font-name", &name, NULL);
    PangoFontDescription* d = pango_font_description_from_string(name ? name : "Sans");
    const char* family = pango_font_description_get_family(d);
    g_uiFamily = family ? family : "Sans";
    pango_font_description_free(d);
    g_free(name);

    // A colour emoji font, where the system has one; without, the app icon stands in for the emoji.
    PangoFontFamily** families = NULL;
    int n = 0;
    std::string emoji;
    pango_font_map_list_families(pango_cairo_font_map_get_default(), &families, &n);
    for (const char* want : { "Noto Color Emoji", "Twemoji", "JoyPixels" })
        for (int i = 0; i < n && emoji.empty(); i++)
            if (strcmp(pango_font_family_get_name(families[i]), want) == 0) emoji = want;
    for (int i = 0; i < n && emoji.empty(); i++)
        if (strstr(pango_font_family_get_name(families[i]), "Emoji")) emoji = pango_font_family_get_name(families[i]);
    g_free(families);
    if (!emoji.empty()) g_emoji = CroppedEmoji(emoji);
}

static void SetColour(cairo_t* cr, const Rgb& c) { cairo_set_source_rgb(cr, c.r / 255.0, c.g / 255.0, c.b / 255.0); }

// Text in `font` drawn into r: left or right aligned, at its top or centred vertically. Its width.
static int DrawText(cairo_t* cr, PangoFontDescription* font, const char* text, const Rect& r, bool right, bool centred)
{
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text, -1);
    int w, h;
    pango_layout_get_pixel_size(layout, &w, &h);
    int x = right ? r.x + r.w - w : r.x;
    int y = centred ? r.y + (r.h - h) / 2 : r.y;
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    return w;
}

static int TextWidth(cairo_t* cr, PangoFontDescription* font, const char* text, int* height = NULL)
{
    PangoLayout* layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, text, -1);
    int w, h;
    pango_layout_get_pixel_size(layout, &w, &h);
    g_object_unref(layout);
    if (height) *height = h;
    return w;
}

// ---------------------------------------------------------------------------------------------
// Layout and scrolling
// ---------------------------------------------------------------------------------------------

static int BarWidth()
{
    if (!gtk_widget_get_visible(g_bar)) return 0;
    int min = 0, nat = 0;
    gtk_widget_get_preferred_width(g_bar, &min, &nat);
    return nat;
}
static int ClientWidth() { return gtk_widget_get_allocated_width(g_area) - BarWidth(); }
static int ClientHeight() { return gtk_widget_get_allocated_height(g_area); }
static int Fit() { return M.RowsThatFit(ClientHeight()); }

static void Relayout()
{
    gtk_widget_queue_resize(g_overlay);   // the overlay asks where its children go again
    gtk_widget_queue_draw(g_area);
}

// Scrolling is by whole rows: the title and header stay put, rows g_first .. g_first+fit-1 are
// shown, and any row that would not fit completely is hidden rather than drawn partially.
// relayout: false when called while GTK lays the window out, which places the boxes anyway.
static void UpdateScroll(bool relayout = true)
{
    int fit = Fit(), n = (int)g_rows.size();
    g_first = ClampFirstRow(g_first, n, fit);
    bool need = n > fit;
    if (need != (bool)gtk_widget_get_visible(g_bar)) gtk_widget_set_visible(g_bar, need);
    gtk_adjustment_configure(g_adjust, g_first, 0, n, 1, fit, fit);
    for (size_t i = 0; i < g_rows.size(); i++) {
        bool shown = (int)i >= g_first && (int)i < g_first + fit;
        if (shown != (bool)gtk_widget_get_child_visible(g_rows[i]->entry)) gtk_widget_set_child_visible(g_rows[i]->entry, shown);
    }
    if (relayout) Relayout();
}

static void SetFirstRow(int first)
{
    first = ClampFirstRow(first, (int)g_rows.size(), Fit());
    if (first == g_first) return;
    g_first = first;
    UpdateScroll();
}

static void EnsureRowVisible(int i)
{
    int fit = Fit();
    if (i < g_first) SetFirstRow(i);
    else if (i >= g_first + fit) SetFirstRow(i - fit + 1);
}

// Where the overlay puts its children: each address box in its row's cell, the scrollbar along
// the right edge.
static gboolean OnChildPosition(GtkOverlay*, GtkWidget* child, GdkRectangle* a, gpointer)
{
    if (child == g_bar) {
        a->width = BarWidth();
        a->x = gtk_widget_get_allocated_width(g_area) - a->width;
        a->y = 0;
        a->height = ClientHeight();
        return TRUE;
    }
    Cols c = M.Columns(ClientWidth());
    for (size_t i = 0; i < g_rows.size(); i++) {
        if (g_rows[i]->entry != child) continue;
        int slot = (int)i - g_first;
        if (slot < 0 || slot >= Fit()) slot = 0;   // hidden; anywhere will do
        Rect r = M.Cell(c, C_ADDR, M.RowTop(slot));
        a->x = r.x; a->y = r.y; a->width = r.w; a->height = r.h;
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------------------------

static Row* FindRow(int id)
{
    for (auto& r : g_rows) if (r->id == id) return r.get();
    return NULL;
}
static size_t IndexOf(GtkWidget* entry)
{
    for (size_t i = 0; i < g_rows.size(); i++) if (g_rows[i]->entry == entry) return i;
    return (size_t)-1;
}

static void Save()
{
    std::vector<std::string> addrs;
    for (auto& r : g_rows) addrs.push_back(r->addr);
    SaveAddresses(addrs);
}

static void FocusEdit(size_t i)
{
    if (i >= g_rows.size()) return;
    EnsureRowVisible((int)i);
    gtk_widget_grab_focus(g_rows[i]->entry);
    gtk_editable_select_region(GTK_EDITABLE(g_rows[i]->entry), 0, -1);
}

// Reads the box into the row; restarts pinging if the address changed. True if it did.
static bool ApplyEdit(Row* r)
{
    std::string a = Trim(gtk_entry_get_text(GTK_ENTRY(r->entry)));
    if (a == r->addr) return false;
    r->addr = a;
    r->pinger.reset(a.empty() ? NULL : new Pinger(a));
    r->stats.Reset();
    Save();
    gtk_widget_queue_draw(g_area);
    return true;
}

static void EnsureTrailingEmptyRow();

static gboolean OnEntryKey(GtkWidget* entry, GdkEventKey* e, gpointer)
{
    size_t i = IndexOf(entry);
    if (i == (size_t)-1 || g_closing) return FALSE;
    switch (e->keyval) {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        ApplyEdit(g_rows[i].get());
        EnsureTrailingEmptyRow();              // a filled-in last row gets a fresh empty row after it
        if (i + 1 < g_rows.size()) FocusEdit(i + 1);
        return TRUE;
    case GDK_KEY_Tab:
    case GDK_KEY_KP_Tab:
    case GDK_KEY_ISO_Left_Tab: {
        bool back = e->keyval == GDK_KEY_ISO_Left_Tab || (e->state & GDK_SHIFT_MASK);
        FocusEdit(back ? (i + g_rows.size() - 1) % g_rows.size() : (i + 1) % g_rows.size());
        return TRUE;
    }
    case GDK_KEY_Escape:
        gtk_entry_set_text(GTK_ENTRY(entry), g_rows[i]->addr.c_str());
        return TRUE;
    }
    return FALSE;
}

static gboolean ApplyLater(gpointer id)
{
    Row* r = FindRow(GPOINTER_TO_INT(id));
    if (r && !g_closing && ApplyEdit(r)) EnsureTrailingEmptyRow();
    return G_SOURCE_REMOVE;
}

static gboolean OnEntryFocusOut(GtkWidget* entry, GdkEvent*, gpointer)
{
    size_t i = IndexOf(entry);
    if (i != (size_t)-1 && !g_closing) g_idle_add(ApplyLater, GINT_TO_POINTER(g_rows[i]->id));
    return FALSE;
}

static Row* AddRow(const std::string& addr)
{
    std::unique_ptr<Row> r(new Row);
    r->id = g_nextId++;
    r->addr = addr;
    r->entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(r->entry), addr.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(r->entry), kCue);
    gtk_style_context_add_class(gtk_widget_get_style_context(r->entry), "multiping");
    g_signal_connect(r->entry, "key-press-event", G_CALLBACK(OnEntryKey), NULL);
    g_signal_connect(r->entry, "focus-out-event", G_CALLBACK(OnEntryFocusOut), NULL);
    gtk_overlay_add_overlay(GTK_OVERLAY(g_overlay), r->entry);
    gtk_widget_show(r->entry);
    if (!addr.empty()) r->pinger.reset(new Pinger(addr));
    g_rows.push_back(std::move(r));
    return g_rows.back().get();
}

// The list always ends with an empty row so it is obvious where a new address goes.
static void EnsureTrailingEmptyRow()
{
    if (g_rows.empty() || !g_rows.back()->addr.empty()) {
        AddRow("");
        UpdateScroll();
    }
}

static gboolean DeleteLater(gpointer id)
{
    if (g_closing) return G_SOURCE_REMOVE;
    size_t i = 0;
    while (i < g_rows.size() && g_rows[i]->id != GPOINTER_TO_INT(id)) i++;
    if (i == g_rows.size()) return G_SOURCE_REMOVE;
    GtkWidget* entry = g_rows[i]->entry;
    g_signal_handlers_disconnect_by_func(entry, (gpointer)OnEntryFocusOut, NULL);   // its focus loss is no edit
    g_rows.erase(g_rows.begin() + (long)i);
    gtk_widget_destroy(entry);
    g_hover = -1;
    EnsureTrailingEmptyRow();
    Save();
    UpdateScroll();
    FocusEdit(i < g_rows.size() ? i : g_rows.size() - 1);
    return G_SOURCE_REMOVE;
}

// One tick for every row: take last round's result, then release the next ping. All graphs and
// labels therefore change in the same instant.
static gboolean OnTick(gpointer)
{
    if (g_closing) return G_SOURCE_REMOVE;
    for (auto& r : g_rows) {
        int res;
        if (r->pinger && r->pinger->Tick(&res)) r->stats.Record(res);
    }
    gtk_widget_queue_draw(g_area);
    return G_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------------

static void PaintGraph(cairo_t* cr, const Rect& box, const Stats& s, const Palette& P)
{
    cairo_save(cr);
    cairo_translate(cr, box.x, box.y);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    SetColour(cr, P.graphBack);
    cairo_rectangle(cr, 0, 0, box.w, box.h);
    cairo_fill(cr);
    Graph g = LayoutGraph(s, box.w, box.h);
    int bottom = g.plot.y + g.plot.h;

    // Grid lines at 25 / 50 / 75 %, one pixel each.
    SetColour(cr, P.graphGrid);
    for (int gy : g.gridY) cairo_rectangle(cr, g.plot.x, gy, g.plot.w, 1);
    cairo_fill(cr);

    for (const GraphBar& b : g.bars) {
        SetColour(cr, b.timeout ? P.graphTimeout : P.graphBar);
        cairo_rectangle(cr, b.x0, b.top, b.x1 - b.x0, bottom - b.top);
        cairo_fill(cr);
        if (b.timeout) {
            SetColour(cr, P.graphTimeoutTop);
            cairo_rectangle(cr, b.x0, b.top, b.x1 - b.x0, M.S(3));
            cairo_fill(cr);
        }
    }

    // Scale label, top-left, on a backing of the graph's own colour that keeps it legible over tall bars.
    PangoFontDescription* small = UiFont(FONT_SMALL_PT);
    int lh, lw = TextWidth(cr, small, g.scaleLabel.c_str(), &lh);
    Rect lr = { g.plot.x + M.S(3), g.plot.y + M.S(1), lw, lh };
    SetColour(cr, P.graphBack);
    cairo_rectangle(cr, lr.x, lr.y, lr.w, lr.h);
    cairo_fill(cr);
    SetColour(cr, P.graphLabel);
    DrawText(cr, small, g.scaleLabel.c_str(), lr, false, false);
    pango_font_description_free(small);

    SetColour(cr, P.graphBorder);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 0.5, box.w - 1, box.h - 1);
    cairo_stroke(cr);
    cairo_restore(cr);
}

// Red filled circle with a white X, antialiased.
static void PaintDelete(cairo_t* cr, const Rect& box, const Rgb& colour)
{
    DeleteGlyph d = LayoutDelete((float)box.w, (float)box.h);
    cairo_save(cr);
    cairo_translate(cr, box.x, box.y);
    SetColour(cr, colour);
    cairo_arc(cr, d.cx, d.cy, d.radius, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, d.stroke);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    cairo_move_to(cr, d.cx - d.inset, d.cy - d.inset);
    cairo_line_to(cr, d.cx + d.inset, d.cy + d.inset);
    cairo_move_to(cr, d.cx - d.inset, d.cy + d.inset);
    cairo_line_to(cr, d.cx + d.inset, d.cy - d.inset);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static gboolean OnDraw(GtkWidget* area, cairo_t* cr, gpointer)
{
    GtkStyleContext* style = gtk_widget_get_style_context(area);
    GdkRGBA fg;
    gtk_style_context_get_color(style, gtk_style_context_get_state(style), &fg);
    const Palette& P = PaletteFor(0.3 * fg.red + 0.59 * fg.green + 0.11 * fg.blue > 0.5);   // light text: a dark theme
    int clientW = ClientWidth();
    Cols c = M.Columns(clientW);

    // Title, large and bold, aligned with the Address column.
    PangoFontDescription* logo = UiFont(FONT_LOGO_PT, true);
    gdk_cairo_set_source_rgba(cr, &fg);
    Rect lr = { c.x[0], M.LogoTop(), clientW - c.x[0], M.S(LOGO_H) };
    int titleEnd = c.x[0] + DrawText(cr, logo, kTitle, lr, false, false);
    int x = titleEnd + M.S(10);
    int afterEmoji = x;
    if (g_emoji) {
        // As tall as Windows draws it, its middle a pixel above the middle of the capitals.
        PangoLayout* m = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(m, logo);
        pango_layout_set_text(m, "M", -1);
        PangoRectangle ink;
        pango_layout_get_pixel_extents(m, &ink, NULL);
        g_object_unref(m);
        int capsMiddle = M.LogoTop() + ink.y + ink.height / 2;
        int cw = cairo_image_surface_get_width(g_emoji), ch = cairo_image_surface_get_height(g_emoji);
        int h = M.S(TITLE_EMOJI_PX), w = h * cw / ch;
        int ex = titleEnd + M.S(TITLE_EMOJI_GAP), ey = capsMiddle - M.S(1) - h / 2;
        cairo_save(cr);
        cairo_translate(cr, ex, ey);
        cairo_scale(cr, (double)w / cw, (double)h / ch);
        cairo_set_source_surface(cr, g_emoji, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
        cairo_paint(cr);
        cairo_restore(cr);
        afterEmoji = ex + w;
    } else if (g_icon) {
        // No emoji font on this system: the app icon in its place.
        int sz = M.S(TITLE_ICON_PX);
        GdkPixbuf* small = gdk_pixbuf_scale_simple(g_icon, sz, sz, GDK_INTERP_HYPER);
        gdk_cairo_set_source_pixbuf(cr, small, x, M.LogoTop() + (M.S(LOGO_H) - sz) / 2);
        cairo_paint(cr);
        g_object_unref(small);
        afterEmoji = x + sz;
    }
    pango_font_description_free(logo);

    // Copyright and revision, stacked, right-aligned to the window margin beside the title.
    PangoFontDescription* info = UiFont(FONT_INFO_PT);
    int lh;
    TextWidth(cr, info, kCopyright, &lh);
    SetColour(cr, P.infoText);
    Rect ir = { afterEmoji, M.LogoTop() + (M.S(LOGO_H) - 2 * lh) / 2, clientW - M.S(MARGIN) - afterEmoji, lh };
    DrawText(cr, info, kCopyright, ir, true, false);
    ir.y += lh;
    DrawText(cr, info, BuildInfo(__DATE__, GIT_REV).c_str(), ir, true, false);
    pango_font_description_free(info);

    // Column headers and the line under them.
    PangoFontDescription* font = UiFont(FONT_PT);
    gdk_cairo_set_source_rgba(cr, &fg);
    int y = M.HeaderTop();
    for (int k = 0; k < NCOL; k++) {
        Rect r = { c.x[k], y, c.w[k], M.S(HDR_H) };
        DrawText(cr, font, kColNames[k], r, !LeftAligned(k), true);
    }
    SetColour(cr, P.headerLine);
    cairo_rectangle(cr, c.x[0], y + M.S(HDR_H), c.x[NCOL - 1] + c.w[NCOL - 1] - c.x[0], 1);
    cairo_fill(cr);

    // The visible rows: labels, graph, delete button; the address boxes are widgets of their own.
    int fit = Fit();
    for (int slot = 0; slot < fit && g_first + slot < (int)g_rows.size(); slot++) {
        const Row& r = *g_rows[(size_t)(g_first + slot)];
        int top = M.RowTop(slot);
        std::string labels[5];
        labels[0] = r.stats.Current();
        r.stats.Summary(labels + 1);
        gdk_cairo_set_source_rgba(cr, &fg);
        for (int k = C_CUR; k <= C_MAX; k++) DrawText(cr, font, labels[k - C_CUR].c_str(), M.Cell(c, k, top), true, true);
        PaintGraph(cr, M.Cell(c, C_GRAPH, top), r.stats, P);
        int i = g_first + slot;
        PaintDelete(cr, M.Cell(c, C_DEL, top), i == g_down ? P.delDown : i == g_hover ? P.delHover : P.del);
    }
    pango_font_description_free(font);
    return FALSE;
}

// ---------------------------------------------------------------------------------------------
// Pointer
// ---------------------------------------------------------------------------------------------

// The visible row whose delete button is at (x, y), or -1.
static int DeleteAt(double x, double y)
{
    Cols c = M.Columns(ClientWidth());
    for (int slot = 0; slot < Fit() && g_first + slot < (int)g_rows.size(); slot++) {
        Rect d = M.Cell(c, C_DEL, M.RowTop(slot));
        if (x >= d.x && x < d.x + d.w && y >= d.y && y < d.y + d.h) return g_first + slot;
    }
    return -1;
}

static void SetHover(int over)
{
    if (over == g_hover) return;
    g_hover = over;
    GdkWindow* w = gtk_widget_get_window(g_area);
    GdkCursor* hand = over >= 0 ? gdk_cursor_new_from_name(gdk_window_get_display(w), "pointer") : NULL;
    gdk_window_set_cursor(w, hand);
    if (hand) g_object_unref(hand);
    gtk_widget_queue_draw(g_area);
}

static gboolean OnMotion(GtkWidget*, GdkEventMotion* e, gpointer) { SetHover(DeleteAt(e->x, e->y)); return TRUE; }
static gboolean OnLeave(GtkWidget*, GdkEventCrossing*, gpointer) { SetHover(-1); return TRUE; }

// GTK under Wayland can hand a press and a release over twice: a delete happens only on the
// release that ends the press of the same button.
static gboolean OnPress(GtkWidget*, GdkEventButton* e, gpointer)
{
    if (e->type != GDK_BUTTON_PRESS || e->button != 1) return FALSE;
    g_down = DeleteAt(e->x, e->y);
    if (g_down >= 0) gtk_widget_queue_draw(g_area);
    return g_down >= 0;
}

static gboolean OnRelease(GtkWidget*, GdkEventButton* e, gpointer)
{
    if (e->button != 1 || g_down < 0) return FALSE;
    int pressed = g_down;
    g_down = -1;
    gtk_widget_queue_draw(g_area);
    if (DeleteAt(e->x, e->y) == pressed) g_idle_add(DeleteLater, GINT_TO_POINTER(g_rows[(size_t)pressed]->id));
    return TRUE;
}

// One row per notch. A wheel's notch comes as a smooth step and again as an emulated discrete one,
// which is passed over; a touchpad's small steps add up to notches.
static gboolean OnScroll(GtkWidget*, GdkEventScroll* e, gpointer)
{
    int notches = 0;
    if (e->direction == GDK_SCROLL_SMOOTH) {
        g_wheel += e->delta_y;
        notches = (int)g_wheel;
        g_wheel -= notches;
    } else if (!gdk_event_get_pointer_emulated((GdkEvent*)e)) {
        if (e->direction == GDK_SCROLL_DOWN) notches = 1;
        if (e->direction == GDK_SCROLL_UP) notches = -1;
    }
    if (notches) SetFirstRow(g_first + notches);
    return TRUE;
}

static void OnBarValue(GtkAdjustment* a, gpointer) { SetFirstRow((int)lround(gtk_adjustment_get_value(a))); }

// ---------------------------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------------------------

// The window's content, once there is some, made the size asked for: the size GTK's window takes
// includes its title bar where GTK draws one itself, and the difference shows only here.
static void OnAreaAllocate(GtkWidget*, GdkRectangle* a, gpointer)
{
    if (!g_sized && g_wantW > 0) {
        g_sized = true;
        int dw = g_wantW - a->width, dh = g_wantH - a->height;
        if (dw || dh) {
            int w, h;
            gtk_window_get_size(GTK_WINDOW(g_window), &w, &h);
            gtk_window_resize(GTK_WINDOW(g_window), w + dw, h + dh);
        }
    }
    if (a->width == g_allocW && a->height == g_allocH) return;
    g_allocW = a->width;
    g_allocH = a->height;
    UpdateScroll(false);
}

static gboolean OnDelete(GtkWidget*, GdkEvent*, gpointer)
{
    g_closing = true;
    int x = 0, y = 0;
    gtk_window_get_position(GTK_WINDOW(g_window), &x, &y);   // 0, 0 under Wayland, which never says
    WindowRect r = { x, y, gtk_widget_get_allocated_width(g_area), gtk_widget_get_allocated_height(g_area) };
    SaveWindowRect(r);
    Save();
    g_rows.clear();   // the pingers stop
    return FALSE;
}

static gboolean OnQuitSignal(gint fd, GIOCondition, gpointer)
{
    char b;
    if (read(fd, &b, 1) > 0 && !g_closing) gtk_window_close(GTK_WINDOW(g_window));
    return G_SOURCE_CONTINUE;
}

int main(int argc, char** argv)
{
    gtk_init(&argc, &argv);
    FindFonts();

    std::string png = IconPng();
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new_with_type("png", NULL);
    if (loader && gdk_pixbuf_loader_write(loader, (const guchar*)png.data(), png.size(), NULL) && gdk_pixbuf_loader_close(loader, NULL)) {
        g_icon = gdk_pixbuf_loader_get_pixbuf(loader);
        g_object_ref(g_icon);
    }
    if (loader) g_object_unref(loader);

    // The address boxes at the app's font size, and short enough for their rows.
    GtkCssProvider* css = gtk_css_provider_new();
    char rules[256];
    snprintf(rules, sizeof rules, "entry.multiping { font-size: %dpx; min-height: 0; padding-top: 0; padding-bottom: 0; }", M.Pt(FONT_PT));
    gtk_css_provider_load_from_data(css, rules, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    g_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_window), kTitle);
    if (g_icon) gtk_window_set_icon(GTK_WINDOW(g_window), g_icon);
    g_signal_connect(g_window, "delete-event", G_CALLBACK(OnDelete), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    g_overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(g_window), g_overlay);
    g_signal_connect(g_overlay, "get-child-position", G_CALLBACK(OnChildPosition), NULL);

    g_area = gtk_drawing_area_new();
    gtk_widget_set_can_focus(g_area, FALSE);
    gtk_widget_add_events(g_area, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                  GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    g_signal_connect(g_area, "draw", G_CALLBACK(OnDraw), NULL);
    g_signal_connect(g_area, "size-allocate", G_CALLBACK(OnAreaAllocate), NULL);
    g_signal_connect(g_area, "motion-notify-event", G_CALLBACK(OnMotion), NULL);
    g_signal_connect(g_area, "leave-notify-event", G_CALLBACK(OnLeave), NULL);
    g_signal_connect(g_area, "button-press-event", G_CALLBACK(OnPress), NULL);
    g_signal_connect(g_area, "button-release-event", G_CALLBACK(OnRelease), NULL);
    g_signal_connect(g_area, "scroll-event", G_CALLBACK(OnScroll), NULL);
    gtk_container_add(GTK_CONTAINER(g_overlay), g_area);

    g_adjust = gtk_adjustment_new(0, 0, 1, 1, 1, 1);
    g_signal_connect(g_adjust, "value-changed", G_CALLBACK(OnBarValue), NULL);
    g_bar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, g_adjust);
    gtk_overlay_add_overlay(GTK_OVERLAY(g_overlay), g_bar);

    for (const std::string& a : LoadAddresses()) AddRow(a);
    EnsureTrailingEmptyRow();

    int barMin = 0, barNat = 0;
    gtk_widget_get_preferred_width(g_bar, &barMin, &barNat);
    gtk_widget_set_size_request(g_area, M.MinContentWidth() + barNat, M.ContentHeightFor(MIN_ROWS));

    // The saved size (and place, where the window system lets a window choose: not under Wayland),
    // made to fit the screen; else the size for eight rows.
    WindowRect r;
    g_wantW = M.DefaultWidth();
    g_wantH = M.ContentHeightFor(DEFAULT_ROWS);
    bool placed = false;
    if (LoadWindowRect(&r)) {
        GdkDisplay* display = gdk_display_get_default();
        GdkMonitor* monitor = gdk_display_get_monitor_at_point(display, (int)(r.x + r.w / 2), (int)(r.y + r.h / 2));
        if (!monitor) monitor = gdk_display_get_primary_monitor(display);
        if (monitor) {
            GdkRectangle wa;
            gdk_monitor_get_workarea(monitor, &wa);
            FitWindowRect(&r, wa.x, wa.y, wa.width, wa.height);
        }
        g_wantW = (int)r.w;
        g_wantH = (int)r.h;
#ifdef GDK_WINDOWING_WAYLAND
        placed = !GDK_IS_WAYLAND_DISPLAY(display);
#else
        placed = true;
#endif
    }
    gtk_window_set_default_size(GTK_WINDOW(g_window), g_wantW, g_wantH);
    if (placed) gtk_window_move(GTK_WINDOW(g_window), (int)r.x, (int)r.y);

    int quitFd = WatchQuitSignals();
    if (quitFd >= 0) g_unix_fd_add(quitFd, G_IO_IN, OnQuitSignal, NULL);
    g_timeout_add(PING_INTERVAL_MS, OnTick, NULL);

    gtk_widget_show_all(g_window);
    gtk_widget_hide(g_bar);
    UpdateScroll();
    if (!g_rows.empty()) {
        // At the start, as on Windows: the caret at the start of the first address, nothing selected.
        gtk_widget_grab_focus(g_rows[0]->entry);
        gtk_editable_select_region(GTK_EDITABLE(g_rows[0]->entry), 0, 0);
    }
    gtk_main();
    return 0;
}
