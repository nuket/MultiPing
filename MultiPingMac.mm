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

// MultiPing for macOS with AppKit (build_macos.sh makes MultiPing.app). The window is the Windows
// one: its layout, look and behaviour come from MultiPingCore.h, the pinging and the files from
// MultiPingPosix.h. One flipped view draws everything but the address boxes and the scrollbar.

#import <Cocoa/Cocoa.h>

#include "MultiPingPosix.h"

using namespace mp;

// Git short revision, passed in by build_macos.sh as -DGIT_REV="abc1234".
#ifndef GIT_REV
#define GIT_REV "unknown"
#endif

// AppKit's points are device-independent pixels, the 96-dpi pixels of the core's metrics: AppKit
// scales them for the screen the way Windows scales by its dpi. Fonts too are sized in them, so a
// point at 96 dpi becomes M.Pt() of them, and the window matches the Windows one pixel for pixel.
static const Metrics M(96);

static NSString* NS(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }
static NSString* NS(const char* s) { return [NSString stringWithUTF8String:s]; }
static NSColor* Colour(const Rgb& c) { return [NSColor colorWithSRGBRed:c.r / 255.0 green:c.g / 255.0 blue:c.b / 255.0 alpha:1]; }
static NSRect R(const mp::Rect& r) { return NSMakeRect(r.x, r.y, r.w, r.h); }
static NSFont* UiFont(int pt, bool bold = false)
{
    return bold ? [NSFont boldSystemFontOfSize:M.Pt(pt)] : [NSFont systemFontOfSize:M.Pt(pt)];
}

// Text drawn into r: left or right aligned, at its top or centred vertically. Its width.
static CGFloat DrawText(NSString* s, NSFont* font, NSColor* colour, const mp::Rect& r, bool right, bool centred)
{
    NSDictionary* attrs = @{ NSFontAttributeName: font, NSForegroundColorAttributeName: colour };
    NSSize size = [s sizeWithAttributes:attrs];
    CGFloat x = right ? r.x + r.w - size.width : r.x;
    CGFloat y = centred ? r.y + (r.h - size.height) / 2 : r.y;
    [s drawAtPoint:NSMakePoint(round(x), round(y)) withAttributes:attrs];
    return size.width;
}

// The title emoji as Apple Color Emoji draws it, large, cut to its visible pixels, so the window
// can scale and place it the way Windows does. Nil when nothing is drawn.
static NSImage* CroppedEmoji()
{
    NSFont* font = [NSFont fontWithName:@"Apple Color Emoji" size:128];
    if (!font) return nil;
    const int size = 256;
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:size pixelsHigh:size
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:rep]];
    [NS(kTitleEmoji) drawAtPoint:NSMakePoint(32, 32) withAttributes:@{ NSFontAttributeName: font }];
    [NSGraphicsContext restoreGraphicsState];
    const unsigned char* data = rep.bitmapData;
    NSInteger stride = rep.bytesPerRow;
    int x0 = size, y0 = size, x1 = -1, y1 = -1;
    for (int y = 0; y < size; y++)             // row 0 is the top
        for (int x = 0; x < size; x++) {
            if (data[y * stride + x * 4 + 3] < 16) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    if (x1 < 0) return nil;
    CGImageRef cut = CGImageCreateWithImageInRect(rep.CGImage, CGRectMake(x0, y0, x1 - x0 + 1, y1 - y0 + 1));
    NSImage* image = [[NSImage alloc] initWithCGImage:cut size:NSMakeSize(x1 - x0 + 1, y1 - y0 + 1)];
    CGImageRelease(cut);
    return image;
}

struct Row {
    int id;
    std::string addr, notes;
    NSTextField* field;        // the address box, tagged with the row's id
    NSTextField* notesField;   // the notes box, tagged with minus the row's id
    std::unique_ptr<Pinger> pinger;
    Stats stats;
};

// ---------------------------------------------------------------------------------------------
// The view
// ---------------------------------------------------------------------------------------------

@interface MPView : NSView <NSTextFieldDelegate>
- (void)updateScroll;
- (void)focusFirst;
- (void)takeBoxes;
- (void)save;
- (void)stop;
@end

@implementation MPView {
    std::vector<std::unique_ptr<Row>> _rows;
    int _nextId;
    int _first;       // index of the first visible row (row-based scrolling)
    int _hover;       // the row whose delete button the pointer is over
    int _down;        // the row whose delete button is pressed
    int _gripHover;   // the row whose handle the pointer is over
    Row* _dragRow;    // the row being moved by its handle
    BOOL _dragMoved;  // and whether it has left its place
    int _dragY;       // the pointer's height while it is
    NSTimer* _dragTimer;   // scrolls the list while the pointer is above or below the rows shown
    double _wheel;
    BOOL _closing;
    NSScroller* _bar;
    NSImage* _emoji;
    NSTimer* _tick;
}

- (instancetype)initWithFrame:(NSRect)frame
{
    if (!(self = [super initWithFrame:frame])) return nil;
    _nextId = 1;
    _hover = _down = _gripHover = -1;
    _emoji = CroppedEmoji();
    _bar = [[NSScroller alloc] initWithFrame:NSMakeRect(0, 0, [self barWidth], 100)];
    _bar.scrollerStyle = NSScrollerStyleLegacy;   // always there, as on Windows, not an overlay
    _bar.enabled = YES;
    _bar.target = self;
    _bar.action = @selector(scrolled:);
    _bar.hidden = YES;
    [self addSubview:_bar];
    for (const Entry& e : LoadEntries()) [self addRow:e.addr notes:e.notes];
    [self ensureTrailingEmptyRow];
    _tick = [NSTimer timerWithTimeInterval:PING_INTERVAL_MS / 1000.0 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    _tick.tolerance = 0.005;
    [[NSRunLoop mainRunLoop] addTimer:_tick forMode:NSRunLoopCommonModes];   // also while the scrollbar is dragged
    return self;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { return YES; }

- (CGFloat)barWidth { return [NSScroller scrollerWidthForControlSize:NSControlSizeRegular scrollerStyle:NSScrollerStyleLegacy]; }
- (int)clientWidth { return (int)self.bounds.size.width - (_bar.hidden ? 0 : (int)[self barWidth]); }
- (int)fit { return M.RowsThatFit((int)self.bounds.size.height); }

- (Row*)findRow:(int)rid
{
    for (auto& r : _rows) if (r->id == rid) return r.get();
    return NULL;
}
// The row an address or notes box belongs to, and which of the two it is.
- (size_t)indexOfField:(NSControl*)field notes:(BOOL*)notes
{
    for (size_t i = 0; i < _rows.size(); i++) {
        if (_rows[i]->field != field && _rows[i]->notesField != field) continue;
        if (notes) *notes = _rows[i]->notesField == field;
        return i;
    }
    return (size_t)-1;
}

// Takes what the boxes hold, edited or not, as the rows' addresses and notes: for quitting.
- (void)takeBoxes
{
    for (auto& r : _rows) {
        NSTextField* fields[2] = { r->field, r->notesField };
        std::string text[2];
        for (int k = 0; k < 2; k++) {
            NSText* editor = [fields[k] currentEditor];
            NSString* s = editor ? editor.string : fields[k].stringValue;
            text[k] = s.UTF8String ? s.UTF8String : "";
        }
        r->addr = Trim(text[0]);
        r->notes = OneLine(text[1]);
    }
}

- (void)save
{
    std::vector<Entry> entries;
    for (auto& r : _rows) entries.push_back(Entry{ r->addr, r->notes });
    SaveEntries(entries);
}

- (void)stop
{
    _closing = YES;
    [_tick invalidate];
    [_dragTimer invalidate];
    _rows.clear();   // the pingers stop
}

// An address or notes box: Enter, Tab and Escape handled by the delegate methods below.
- (NSTextField*)makeField:(const std::string&)text tag:(int)tag
{
    NSTextField* f = [[NSTextField alloc] initWithFrame:NSZeroRect];
    f.stringValue = NS(text);
    f.font = UiFont(FONT_PT);
    f.bezeled = YES;
    f.bezelStyle = NSTextFieldSquareBezel;
    f.editable = YES;
    f.usesSingleLineMode = YES;
    f.cell.scrollable = YES;
    f.tag = tag;
    f.delegate = self;
    f.hidden = YES;
    [self addSubview:f];
    return f;
}

- (Row*)addRow:(const std::string&)addr notes:(const std::string&)notes
{
    std::unique_ptr<Row> r(new Row);
    r->id = _nextId++;
    r->addr = addr;
    r->notes = notes;
    r->field = [self makeField:addr tag:r->id];
    r->field.placeholderString = NS(kCue);
    r->notesField = [self makeField:notes tag:-r->id];
    if (!addr.empty()) r->pinger.reset(new Pinger(addr));
    _rows.push_back(std::move(r));
    return _rows.back().get();
}

// The list always ends with an empty row so it is obvious where a new address goes.
- (void)ensureTrailingEmptyRow
{
    if (_rows.empty() || !_rows.back()->addr.empty()) {
        [self addRow:"" notes:""];
        [self updateScroll];
    }
}

// Reads the box into the row; restarts pinging if the address changed. YES if it did.
- (BOOL)applyEdit:(Row*)r
{
    NSText* editor = [r->field currentEditor];
    NSString* text = editor ? editor.string : r->field.stringValue;
    std::string a = Trim(text.UTF8String ? text.UTF8String : "");
    if (a == r->addr) return NO;
    r->addr = a;
    r->pinger.reset(a.empty() ? NULL : new Pinger(a));
    r->stats.Reset();
    [self save];
    self.needsDisplay = YES;
    return YES;
}

// Reads the notes box into the row, and saves when they changed. YES if they did.
- (BOOL)applyNotes:(Row*)r
{
    NSText* editor = [r->notesField currentEditor];
    NSString* text = editor ? editor.string : r->notesField.stringValue;
    std::string n = OneLine(text.UTF8String ? text.UTF8String : "");
    if (n == r->notes) return NO;
    r->notes = n;
    [self save];
    return YES;
}

- (void)deleteRow:(int)rid
{
    if (_closing || _dragRow) return;
    size_t i = 0;
    while (i < _rows.size() && _rows[i]->id != rid) i++;
    if (i == _rows.size()) return;
    for (NSTextField* f in @[ _rows[i]->field, _rows[i]->notesField ]) {
        f.delegate = nil;            // its end of editing is no edit to apply
        [f abortEditing];
        [f removeFromSuperview];
    }
    _rows.erase(_rows.begin() + (long)i);
    _hover = _gripHover = -1;
    [self ensureTrailingEmptyRow];
    [self save];
    [self updateScroll];
    [self focusEdit:i < _rows.size() ? i : _rows.size() - 1];
}

- (void)focusEdit:(size_t)i { [self focusEdit:i notes:NO]; }

// Puts the caret in row i's address box, or its notes box, with the text selected.
- (void)focusEdit:(size_t)i notes:(BOOL)notes
{
    if (i >= _rows.size()) return;
    [self ensureRowVisible:(int)i];
    NSTextField* f = notes ? _rows[i]->notesField : _rows[i]->field;
    [self.window makeFirstResponder:f];
    [[f currentEditor] selectAll:nil];
}

// At the start, as on Windows: the caret at the start of the first address, nothing selected.
- (void)focusFirst
{
    if (_rows.empty()) return;
    [self.window makeFirstResponder:_rows[0]->field];
    [(NSTextView*)[_rows[0]->field currentEditor] setSelectedRange:NSMakeRange(0, 0)];
}

// One tick for every row: take last round's result, then release the next ping. All graphs and
// labels therefore change in the same instant.
- (void)tick:(NSTimer*)t
{
    if (_closing) return;
    for (auto& r : _rows) {
        int res;
        if (r->pinger && r->pinger->Tick(&res)) r->stats.Record(res);
    }
    self.needsDisplay = YES;
}

// --- the address and notes boxes: Enter, Tab, Shift+Tab and Escape, and applying when editing
// ends --------------------------------------------------------------------------------------------

- (BOOL)control:(NSControl*)control textView:(NSTextView*)tv doCommandBySelector:(SEL)sel
{
    BOOL notes = NO;
    size_t i = [self indexOfField:control notes:&notes];
    if (i == (size_t)-1 || _closing) return NO;
    size_t n = 2 * _rows.size(), f = 2 * i + (notes ? 1 : 0);   // the boxes in reading order
    if (sel == @selector(insertNewline:)) {
        // From either box, on to the next row's address.
        if (notes) [self applyNotes:_rows[i].get()];
        else [self applyEdit:_rows[i].get()];
        [self ensureTrailingEmptyRow];       // a filled-in last row gets a fresh empty row after it
        if (i + 1 < _rows.size()) [self focusEdit:i + 1];
        return YES;
    }
    if (sel == @selector(insertTab:)) { f = (f + 1) % n; [self focusEdit:f / 2 notes:f % 2 == 1]; return YES; }
    if (sel == @selector(insertBacktab:)) { f = (f + n - 1) % n; [self focusEdit:f / 2 notes:f % 2 == 1]; return YES; }
    if (sel == @selector(cancelOperation:)) { tv.string = NS(notes ? _rows[i]->notes : _rows[i]->addr); return YES; }
    return NO;
}

- (void)controlTextDidEndEditing:(NSNotification*)n
{
    int tag = (int)((NSControl*)n.object).tag;
    dispatch_async(dispatch_get_main_queue(), ^{
        Row* r = [self findRow:tag < 0 ? -tag : tag];
        if (!r || self->_closing) return;
        if (tag < 0) [self applyNotes:r];
        else if ([self applyEdit:r]) [self ensureTrailingEmptyRow];
    });
}

// --- scrolling: by whole rows; the title and header stay put, rows _first .. _first+fit-1 are
// shown, and any row that would not fit completely is hidden rather than drawn partially --------

- (void)updateScroll
{
    int fit = [self fit], n = (int)_rows.size();
    _first = ClampFirstRow(_first, n, fit);
    BOOL need = n > fit;
    _bar.hidden = !need;
    _bar.knobProportion = n > 0 ? (CGFloat)fit / n : 1;
    _bar.doubleValue = n > fit ? (double)_first / (n - fit) : 0;
    CGFloat w = [self barWidth];
    _bar.frame = NSMakeRect(self.bounds.size.width - w, 0, w, self.bounds.size.height);
    [self layoutRows];
}

- (void)setFirstRow:(int)first
{
    first = ClampFirstRow(first, (int)_rows.size(), [self fit]);
    if (first == _first) return;
    _first = first;
    [self updateScroll];
}

- (void)ensureRowVisible:(int)i
{
    int fit = [self fit];
    if (i < _first) [self setFirstRow:i];
    else if (i >= _first + fit) [self setFirstRow:i - fit + 1];
}

- (void)scrolled:(NSScroller*)bar
{
    int fit = [self fit], n = (int)_rows.size();
    switch (bar.hitPart) {   // no arrows on a Mac's scrollbar: pages and the knob
    case NSScrollerDecrementPage: [self setFirstRow:_first - fit]; break;
    case NSScrollerIncrementPage: [self setFirstRow:_first + fit]; break;
    default: [self setFirstRow:(int)lround(bar.doubleValue * (n > fit ? n - fit : 0))]; break;
    }
}

// One row per notch; a trackpad's fine steps add up to rows.
- (void)scrollWheel:(NSEvent*)e
{
    double dy = e.hasPreciseScrollingDeltas ? e.scrollingDeltaY / M.S(ROW_H) : e.scrollingDeltaY;
    _wheel += dy;
    int rows = (int)_wheel;
    _wheel -= rows;
    if (rows) [self setFirstRow:_first - rows];
}

- (void)layoutRows
{
    Cols c = M.Columns([self clientWidth]);
    int fit = [self fit];
    for (size_t i = 0; i < _rows.size(); i++) {
        BOOL shown = (int)i >= _first && (int)i < _first + fit;
        NSTextField* fields[2] = { _rows[i]->field, _rows[i]->notesField };
        for (int k = 0; k < 2; k++) {
            if (shown) fields[k].frame = R(M.Cell(c, k ? C_NOTES : C_ADDR, M.RowTop((int)i - _first)));
            if (shown == fields[k].hidden) fields[k].hidden = !shown;
        }
    }
    [self.window invalidateCursorRectsForView:self];
    self.needsDisplay = YES;
}

- (void)resizeSubviewsWithOldSize:(NSSize)old { [self updateScroll]; }

// --- the delete buttons ----------------------------------------------------------------------

// The visible row whose delete button is at p, or -1.
- (int)deleteAt:(NSPoint)p
{
    Cols c = M.Columns([self clientWidth]);
    for (int slot = 0; slot < [self fit] && _first + slot < (int)_rows.size(); slot++)
        if (NSPointInRect(p, R(M.Cell(c, C_DEL, M.RowTop(slot))))) return _first + slot;
    return -1;
}

- (void)updateTrackingAreas
{
    for (NSTrackingArea* a in self.trackingAreas) [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
        options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
        owner:self userInfo:nil]];
    [super updateTrackingAreas];
}

// The visible row whose handle is at p, or -1. The empty row at the end has none.
- (int)gripAt:(NSPoint)p
{
    Cols c = M.Columns([self clientWidth]);
    for (int slot = 0; slot < [self fit] && _first + slot + 1 < (int)_rows.size(); slot++)
        if (NSPointInRect(p, R(M.Cell(c, C_GRIP, M.RowTop(slot))))) return _first + slot;
    return -1;
}

- (void)resetCursorRects
{
    Cols c = M.Columns([self clientWidth]);
    for (int slot = 0; slot < [self fit] && _first + slot < (int)_rows.size(); slot++) {
        [self addCursorRect:R(M.Cell(c, C_DEL, M.RowTop(slot))) cursor:[NSCursor pointingHandCursor]];
        if (_first + slot + 1 < (int)_rows.size())
            [self addCursorRect:R(M.Cell(c, C_GRIP, M.RowTop(slot))) cursor:[NSCursor openHandCursor]];
    }
}

- (void)setHover:(int)over grip:(int)grip
{
    if (over == _hover && grip == _gripHover) return;
    _hover = over;
    _gripHover = grip;
    self.needsDisplay = YES;
}
- (void)mouseMoved:(NSEvent*)e
{
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    [self setHover:[self deleteAt:p] grip:[self gripAt:p]];
}
- (void)mouseExited:(NSEvent*)e { if (!_dragRow) [self setHover:-1 grip:-1]; }

// Moves the row being dragged to where the pointer is, scrolling the list a row when `scroll` is
// set (the timer's tick), and keeps the timer running while the pointer is above or below the
// rows shown.
- (void)dragTo:(BOOL)scroll
{
    if (!_dragRow) return;
    int n = (int)_rows.size(), fit = [self fit], first = _first;
    size_t from = 0;
    while (_rows[from].get() != _dragRow) from++;
    int to = DragTarget(M, _dragY, &first, fit, n, n - 1, scroll);
    bool moved = to != (int)from, scrolled = first != _first;
    if (moved) { MoveItem(_rows, from, (size_t)to); _dragMoved = YES; }
    if (scrolled) _first = first;
    if (moved || scrolled) [self updateScroll];
    if (!DragOutside(M, _dragY, fit)) {
        [_dragTimer invalidate];
        _dragTimer = nil;
    } else if (!_dragTimer) {
        _dragTimer = [NSTimer timerWithTimeInterval:DRAG_SCROLL_MS / 1000.0 target:self selector:@selector(dragTick:) userInfo:nil repeats:YES];
        [[NSRunLoop mainRunLoop] addTimer:_dragTimer forMode:NSRunLoopCommonModes];
    }
}
- (void)dragTick:(NSTimer*)t { [self dragTo:YES]; }

- (void)mouseDown:(NSEvent*)e
{
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    int grip = [self gripAt:p];
    if (grip >= 0) {
        // A row's handle: the row follows the pointer up and down until the button goes up.
        _dragRow = _rows[(size_t)grip].get();
        _dragMoved = NO;
        _dragY = (int)p.y;
        [self.window disableCursorRects];
        [[NSCursor closedHandCursor] set];
        self.needsDisplay = YES;
        return;
    }
    _down = [self deleteAt:p];
    if (_down >= 0) self.needsDisplay = YES;
}
- (void)mouseDragged:(NSEvent*)e
{
    if (!_dragRow) return;
    _dragY = (int)[self convertPoint:e.locationInWindow fromView:nil].y;
    [self dragTo:NO];
}
- (void)mouseUp:(NSEvent*)e
{
    if (_dragRow) {
        _dragY = (int)[self convertPoint:e.locationInWindow fromView:nil].y;   // the release can bring the pointer's last move with it
        [self dragTo:NO];
        _dragRow = NULL;
        [_dragTimer invalidate];
        _dragTimer = nil;
        [self.window enableCursorRects];
        [self.window invalidateCursorRectsForView:self];
        NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
        [([self gripAt:p] >= 0 ? [NSCursor openHandCursor] : [NSCursor arrowCursor]) set];
        _hover = _gripHover = -2;   // so that setHover: draws again
        [self setHover:[self deleteAt:p] grip:[self gripAt:p]];
        if (_dragMoved) [self save];
        return;
    }
    if (_down < 0) return;
    int pressed = _down;
    _down = -1;
    self.needsDisplay = YES;
    if ([self deleteAt:[self convertPoint:e.locationInWindow fromView:nil]] == pressed) {
        int rid = _rows[(size_t)pressed]->id;
        dispatch_async(dispatch_get_main_queue(), ^{ [self deleteRow:rid]; });
    }
}

// --- painting --------------------------------------------------------------------------------

// The palette follows the system's appearance, also when it changes while the window is open.
- (void)viewDidChangeEffectiveAppearance
{
    [super viewDidChangeEffectiveAppearance];
    self.needsDisplay = YES;
}

- (BOOL)dark
{
    if (@available(macOS 10.14, *))
        return [[self.effectiveAppearance bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]]
                   isEqualToString:NSAppearanceNameDarkAqua];
    return NO;
}

- (void)drawRect:(NSRect)dirty
{
    [[NSColor windowBackgroundColor] setFill];
    NSRectFill(dirty);
    NSColor* text = [NSColor labelColor];
    const Palette& P = PaletteFor([self dark]);
    int clientW = [self clientWidth];
    Cols c = M.Columns(clientW);

    // Title, large and bold, aligned with the Address column.
    NSFont* logo = UiFont(FONT_LOGO_PT, true);
    mp::Rect lr = { c.x[0], M.LogoTop(), clientW - c.x[0], M.S(LOGO_H) };
    int titleEnd = c.x[0] + (int)ceil(DrawText(NS(kTitle), logo, text, lr, false, false));
    int afterEmoji = titleEnd + M.S(10);
    if (_emoji) {
        // As tall as Windows draws it, its middle a pixel above the middle of the capitals.
        int h = M.S(TITLE_EMOJI_PX), w = (int)lround(h * _emoji.size.width / _emoji.size.height);
        int capsMiddle = M.LogoTop() + (int)lround(logo.ascender - logo.capHeight / 2);
        NSRect er = NSMakeRect(titleEnd + M.S(TITLE_EMOJI_GAP), capsMiddle - M.S(1) - h / 2, w, h);
        [_emoji drawInRect:er fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES
                     hints:@{ NSImageHintInterpolation: @(NSImageInterpolationHigh) }];
        afterEmoji = (int)NSMaxX(er);
    }

    // Copyright and revision, stacked, right-aligned to the window margin beside the title.
    NSFont* info = UiFont(FONT_INFO_PT);
    NSColor* grey = Colour(P.infoText);
    int lh = (int)ceil([@"Mg" sizeWithAttributes:@{ NSFontAttributeName: info }].height);
    mp::Rect ir = { afterEmoji, M.LogoTop() + (M.S(LOGO_H) - 2 * lh) / 2, clientW - M.S(MARGIN) - afterEmoji, lh };
    DrawText(NS(kCopyright), info, grey, ir, true, false);
    ir.y += lh;
    DrawText(NS(BuildInfo(__DATE__, GIT_REV)), info, grey, ir, true, false);

    // Column headers and the line under them.
    NSFont* font = UiFont(FONT_PT);
    int y = M.HeaderTop();
    for (int k = 0; k < NCOL; k++) {
        mp::Rect r = { c.x[k], y, c.w[k], M.S(HDR_H) };
        DrawText(NS(kColNames[k]), font, text, r, !LeftAligned(k), true);
    }
    [Colour(P.headerLine) setFill];
    NSRectFill(NSMakeRect(c.x[0], y + M.S(HDR_H), c.x[NCOL - 1] + c.w[NCOL - 1] - c.x[0], 1));

    // The visible rows: handle, labels, graph, delete button; the boxes are views of their own.
    int fit = [self fit];
    for (int slot = 0; slot < fit && _first + slot < (int)_rows.size(); slot++) {
        const Row& r = *_rows[(size_t)(_first + slot)];
        int top = M.RowTop(slot);
        std::string labels[5];
        labels[0] = r.stats.Current();
        r.stats.Summary(labels + 1);
        for (int k = C_CUR; k <= C_MAX; k++) DrawText(NS(labels[k - C_CUR]), font, text, M.Cell(c, k, top), true, true);
        [self paintGraph:M.Cell(c, C_GRAPH, top) stats:r.stats palette:P];
        int i = _first + slot;
        if (i + 1 < (int)_rows.size()) {   // the empty row at the end has no handle: it stays last
            bool active = _dragRow ? _dragRow == _rows[(size_t)i].get() : i == _gripHover;
            [self paintGrip:M.Cell(c, C_GRIP, top) colour:active ? P.gripActive : P.grip];
        }
        [self paintDelete:M.Cell(c, C_DEL, top) colour:i == _down ? P.delDown : i == _hover ? P.delHover : P.del];
    }
}

- (void)paintGraph:(const mp::Rect&)box stats:(const Stats&)s palette:(const Palette&)P
{
    NSGraphicsContext* gc = [NSGraphicsContext currentContext];
    [gc saveGraphicsState];
    gc.shouldAntialias = NO;
    [Colour(P.graphBack) setFill];
    NSRectFill(R(box));
    Graph g = LayoutGraph(s, box.w, box.h);
    int bottom = g.plot.y + g.plot.h;

    // Grid lines at 25 / 50 / 75 %.
    [Colour(P.graphGrid) setFill];
    for (int gy : g.gridY) NSRectFill(NSMakeRect(box.x + g.plot.x, box.y + gy, g.plot.w, 1));

    for (const GraphBar& b : g.bars) {
        [Colour(b.timeout ? P.graphTimeout : P.graphBar) setFill];
        NSRectFill(NSMakeRect(box.x + b.x0, box.y + b.top, b.x1 - b.x0, bottom - b.top));
        if (b.timeout) {
            [Colour(P.graphTimeoutTop) setFill];
            NSRectFill(NSMakeRect(box.x + b.x0, box.y + b.top, b.x1 - b.x0, M.S(3)));
        }
    }

    // Scale label, top-left, on a backing of the graph's own colour that keeps it legible over tall bars.
    NSFont* small = UiFont(FONT_SMALL_PT);
    NSString* label = NS(g.scaleLabel);
    NSSize ls = [label sizeWithAttributes:@{ NSFontAttributeName: small }];
    mp::Rect lr = { box.x + g.plot.x + M.S(3), box.y + g.plot.y + M.S(1), (int)ceil(ls.width), (int)ceil(ls.height) };
    [Colour(P.graphBack) setFill];
    NSRectFill(R(lr));
    gc.shouldAntialias = YES;
    DrawText(label, small, Colour(P.graphLabel), lr, false, false);

    [Colour(P.graphBorder) setFill];
    NSFrameRect(R(box));
    [gc restoreGraphicsState];
}

// The handle's six dots.
- (void)paintGrip:(const mp::Rect&)box colour:(const Rgb&)colour
{
    GripDots g = LayoutGrip(M, box);
    [Colour(colour) setFill];
    for (const mp::Rect& d : g.dot) NSRectFill(R(d));
}

// Red filled circle with a white X, antialiased.
- (void)paintDelete:(const mp::Rect&)box colour:(const Rgb&)colour
{
    DeleteGlyph d = LayoutDelete((float)box.w, (float)box.h);
    CGFloat cx = box.x + d.cx, cy = box.y + d.cy;
    [Colour(colour) setFill];
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(cx - d.radius, cy - d.radius, 2 * d.radius, 2 * d.radius)] fill];
    NSBezierPath* x = [NSBezierPath bezierPath];
    x.lineWidth = d.stroke;
    x.lineCapStyle = NSLineCapStyleButt;
    [x moveToPoint:NSMakePoint(cx - d.inset, cy - d.inset)];
    [x lineToPoint:NSMakePoint(cx + d.inset, cy + d.inset)];
    [x moveToPoint:NSMakePoint(cx - d.inset, cy + d.inset)];
    [x lineToPoint:NSMakePoint(cx + d.inset, cy - d.inset)];
    [[NSColor whiteColor] setStroke];
    [x stroke];
}

@end

// ---------------------------------------------------------------------------------------------
// The application
// ---------------------------------------------------------------------------------------------

@interface MPAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation MPAppDelegate {
    NSWindow* _window;
    MPView* _view;
    dispatch_source_t _quit;
    BOOL _saved;
}

static NSMenuItem* Item(NSString* title, SEL action, NSString* key)
{
    return [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
}

// The menu bar: the app's menu, and Edit, without which copy and paste do nothing in the boxes.
- (void)makeMenus
{
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    NSMenu* app = [[NSMenu alloc] initWithTitle:@"MultiPing"];
    [app addItem:Item(@"About MultiPing", @selector(orderFrontStandardAboutPanel:), @"")];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItem:Item(@"Hide MultiPing", @selector(hide:), @"h")];
    NSMenuItem* others = Item(@"Hide Others", @selector(hideOtherApplications:), @"h");
    others.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [app addItem:others];
    [app addItem:Item(@"Show All", @selector(unhideAllApplications:), @"")];
    [app addItem:[NSMenuItem separatorItem]];
    [app addItem:Item(@"Quit MultiPing", @selector(terminate:), @"q")];
    appItem.submenu = app;
    [bar addItem:appItem];

    NSMenuItem* editItem = [[NSMenuItem alloc] init];
    NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    [edit addItem:Item(@"Undo", NSSelectorFromString(@"undo:"), @"z")];
    [edit addItem:Item(@"Redo", NSSelectorFromString(@"redo:"), @"Z")];
    [edit addItem:[NSMenuItem separatorItem]];
    [edit addItem:Item(@"Cut", @selector(cut:), @"x")];
    [edit addItem:Item(@"Copy", @selector(copy:), @"c")];
    [edit addItem:Item(@"Paste", @selector(paste:), @"v")];
    [edit addItem:Item(@"Select All", @selector(selectAll:), @"a")];
    editItem.submenu = edit;
    [bar addItem:editItem];

    NSMenuItem* windowItem = [[NSMenuItem alloc] init];
    NSMenu* window = [[NSMenu alloc] initWithTitle:@"Window"];
    [window addItem:Item(@"Minimize", @selector(performMiniaturize:), @"m")];
    [window addItem:Item(@"Close", @selector(performClose:), @"w")];
    windowItem.submenu = window;
    [bar addItem:windowItem];
    NSApp.mainMenu = bar;
    NSApp.windowsMenu = window;
}

- (void)applicationDidFinishLaunching:(NSNotification*)n
{
    [self makeMenus];
    NSRect content = NSMakeRect(0, 0, M.DefaultWidth(), M.ContentHeightFor(DEFAULT_ROWS));
    _window = [[NSWindow alloc] initWithContentRect:content
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                            backing:NSBackingStoreBuffered defer:NO];
    _window.title = NS(kTitle);
    _window.delegate = self;
    _window.releasedWhenClosed = NO;
    _view = [[MPView alloc] initWithFrame:content];
    _window.contentView = _view;
    _window.contentMinSize = NSMakeSize(M.MinContentWidth() + [NSScroller scrollerWidthForControlSize:NSControlSizeRegular
                                                                                         scrollerStyle:NSScrollerStyleLegacy],
                                        M.ContentHeightFor(MIN_ROWS));

    // The saved place (window.txt holds the content rectangle, from the screen's bottom left, as
    // AppKit counts), made to fit the screen it is on now; else centred, sized for eight rows.
    WindowRect r;
    if (LoadWindowRect(&r)) {
        r.w = FitSavedWidth(r.w, (long)_window.contentMinSize.width, M.DefaultWidth());
        NSRect want = NSMakeRect(r.x, r.y, r.w, r.h);
        NSScreen* screen = NSScreen.mainScreen;
        for (NSScreen* s in NSScreen.screens)
            if (NSPointInRect(NSMakePoint(NSMidX(want), NSMidY(want)), s.frame)) screen = s;
        NSRect wa = [_window contentRectForFrameRect:screen.visibleFrame];
        FitWindowRect(&r, (long)wa.origin.x, (long)wa.origin.y, (long)wa.size.width, (long)wa.size.height);
        [_window setFrame:[_window frameRectForContentRect:NSMakeRect(r.x, r.y, r.w, r.h)] display:NO];
    } else {
        [_window center];
    }
    [_view updateScroll];
    [_window makeKeyAndOrderFront:nil];
    [_view focusFirst];
    if (@available(macOS 14.0, *)) [NSApp activate];
    else [NSApp activateIgnoringOtherApps:YES];

    // SIGTERM and the like quit as the user would, so everything is saved.
    int fd = WatchQuitSignals();
    if (fd >= 0) {
        _quit = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(_quit, ^{
            char b;
            if (read(fd, &b, 1) > 0) [NSApp terminate:nil];
        });
        dispatch_resume(_quit);
    }
}

- (void)saveAll
{
    if (_saved) return;
    _saved = YES;
    NSRect c = [_window contentRectForFrameRect:_window.frame];
    WindowRect r = { (long)c.origin.x, (long)c.origin.y, (long)c.size.width, (long)c.size.height };
    SaveWindowRect(r);
    [_view takeBoxes];
    [_view save];
    [_view stop];
}

- (void)windowWillClose:(NSNotification*)n { [self saveAll]; }
- (void)applicationWillTerminate:(NSNotification*)n { [self saveAll]; }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app { return YES; }
- (BOOL)applicationSupportsSecureRestorableState:(NSApplication*)app { return YES; }

@end

int main()
{
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        MPAppDelegate* delegate = [[MPAppDelegate alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
