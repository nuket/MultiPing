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

// MultiPing for Linux with Qt Widgets: one source, built against Qt 5 and against Qt 6
// (build_linux.sh makes multiping-qt5 and multiping-qt6). The window is the Windows one: its
// layout, look and behaviour come from MultiPingCore.h, the pinging and the files from
// MultiPingPosix.h. Everything but the address boxes is drawn in one paint; no moc is needed.

#include "MultiPingPosix.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QScrollBar>
#include <QSocketNotifier>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>

using namespace mp;

// Git short revision, passed in by build_linux.sh as -DGIT_REV="abc1234".
#ifndef GIT_REV
#define GIT_REV "unknown"
#endif

// Qt's pixels are device-independent, the 96-dpi pixels of the core's metrics: Qt scales them
// for the screen the way Windows scales by its dpi.
static const Metrics M(96);

static QColor Colour(const Rgb& c) { return QColor(c.r, c.g, c.b); }
static QString Q(const std::string& s) { return QString::fromUtf8(s.c_str()); }

// The desktop's UI face at the Windows app's size: points at 96 dpi as pixels, whatever font
// scaling the desktop asks for, so the window matches the Windows one pixel for pixel.
static QFont UiFont(int pt, bool bold = false)
{
    QFont f = QApplication::font();
    f.setPixelSize(M.Pt(pt));
    f.setBold(bold);
    return f;
}

static QStringList FontFamilies()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return QFontDatabase::families();
#else
    return QFontDatabase().families();
#endif
}

// A colour emoji font, where the system has one; without, the app icon stands in for the emoji.
static QString EmojiFamily()
{
    QStringList families = FontFamilies();
    for (const char* name : { "Noto Color Emoji", "Twemoji", "JoyPixels", "Apple Color Emoji" })
        if (families.contains(name)) return name;
    for (const QString& f : families) if (f.contains("Emoji", Qt::CaseInsensitive)) return f;
    return QString();
}

// The title emoji as the font draws it, large, cut to its visible pixels: Qt draws colour emoji
// at a size and height of its own, so the window scales and places this the way Windows does.
// Null when the font draws nothing.
static QImage CroppedEmoji(const QString& family)
{
    QFont f(family);
    f.setPixelSize(128);
    QFontMetrics fm(f);
    QString text = Q(kTitleEmoji);
    QImage image(fm.horizontalAdvance(text) + 64, fm.height() + 64, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setFont(f);
    p.drawText(32, 32 + fm.ascent(), text);
    p.end();
    int x0 = image.width(), y0 = image.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < image.height(); y++) {
        const QRgb* line = (const QRgb*)image.constScanLine(y);
        for (int x = 0; x < image.width(); x++) {
            if (qAlpha(line[x]) < 16) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    return x1 < 0 ? QImage() : image.copy(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

struct Row {
    int id;
    std::string addr, notes;
    QLineEdit* edit;
    QLineEdit* notesEdit;
    std::unique_ptr<Pinger> pinger;
    Stats stats;
};

class Window : public QWidget {
public:
    Window()
    {
        setWindowTitle(kTitle);
        setMouseTracking(true);
        QString emoji = EmojiFamily();
        if (!emoji.isEmpty()) emoji_ = CroppedEmoji(emoji);
        std::string png = IconPng();
        QImage image = QImage::fromData((const uchar*)png.data(), (int)png.size(), "PNG");
        if (!image.isNull()) {
            icon_ = QPixmap::fromImage(image);
            setWindowIcon(QIcon(icon_));
        }
        bar_ = new QScrollBar(Qt::Vertical, this);
        bar_->setSingleStep(1);
        bar_->hide();
        QObject::connect(bar_, &QScrollBar::valueChanged, [this](int v) { SetFirstRow(v); });
        setMinimumSize(M.MinContentWidth() + bar_->sizeHint().width(), M.ContentHeightFor(MIN_ROWS));

        for (const Entry& e : LoadEntries()) AddRow(e.addr, e.notes);
        EnsureTrailingEmptyRow();

        dragTimer_.setInterval(DRAG_SCROLL_MS);
        QObject::connect(&dragTimer_, &QTimer::timeout, [this] { DragTo(true); });

        tick_.setTimerType(Qt::PreciseTimer);
        tick_.setInterval(PING_INTERVAL_MS);
        QObject::connect(&tick_, &QTimer::timeout, [this] { Tick(); });
        tick_.start();
    }

    // At the start, as on Windows: the caret at the start of the first address, nothing selected.
    void FocusFirst()
    {
        if (rows_.empty()) return;
        rows_[0]->edit->setFocus();
        rows_[0]->edit->setCursorPosition(0);
    }
    // The first window's size: eight rows, a graph about 335 px wide.
    QSize sizeHint() const override { return QSize(M.DefaultWidth(), M.ContentHeightFor(DEFAULT_ROWS)); }

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override { UpdateScroll(); Layout(); }

    void wheelEvent(QWheelEvent* e) override
    {
        // One row per notch; a touchpad's small steps add up to notches.
        wheel_ += e->angleDelta().y() / 120.0;
        int notches = (int)wheel_;
        wheel_ -= notches;
        if (notches) SetFirstRow(first_ - notches);
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (dragRow_) { dragY_ = e->pos().y(); DragTo(false); return; }
        int over = DeleteAt(e->pos()), grip = GripAt(e->pos());
        if (over != hover_ || grip != gripHover_) { hover_ = over; gripHover_ = grip; update(); }
        if (over >= 0) setCursor(Qt::PointingHandCursor);
        else if (grip >= 0) setCursor(Qt::OpenHandCursor);
        else unsetCursor();
    }
    void leaveEvent(QEvent*) override
    {
        if (hover_ >= 0 || gripHover_ >= 0) { hover_ = gripHover_ = -1; update(); }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        int grip = GripAt(e->pos());
        if (grip >= 0) {
            // A row's handle: the row follows the pointer up and down until the button goes up.
            dragRow_ = rows_[(size_t)grip].get();
            dragMoved_ = false;
            dragY_ = e->pos().y();
            setCursor(Qt::ClosedHandCursor);
            update();
            return;
        }
        down_ = DeleteAt(e->pos());
        if (down_ >= 0) update();
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        if (dragRow_) {
            dragY_ = e->pos().y();   // the release can bring the pointer's last move with it
            DragTo(false);
            EndDrag();
            gripHover_ = GripAt(e->pos());
            if (gripHover_ >= 0) setCursor(Qt::OpenHandCursor); else unsetCursor();
            return;
        }
        if (down_ < 0) return;
        int pressed = down_;
        down_ = -1;
        update();
        if (DeleteAt(e->pos()) == pressed) {
            int id = rows_[(size_t)pressed]->id;
            QTimer::singleShot(0, this, [this, id] { DeleteRow(id); });
        }
    }

    void closeEvent(QCloseEvent* e) override
    {
        closing_ = true;
        tick_.stop();
        for (auto& r : rows_) {   // what the boxes hold, edited or not
            r->addr = Trim(r->edit->text().toUtf8().toStdString());
            r->notes = OneLine(r->notesEdit->text().toUtf8().toStdString());
        }
        QRect g = geometry();
        WindowRect r = { g.x(), g.y(), g.width(), g.height() };
        SaveWindowRect(r);
        Save();
        rows_.clear();   // the pingers stop
        e->accept();
    }

    // The address and notes boxes: Enter, Tab, Shift+Tab and Escape, and applying on focus loss.
    bool eventFilter(QObject* o, QEvent* e) override
    {
        QLineEdit* edit = qobject_cast<QLineEdit*>(o);
        bool notes = false;
        size_t i = IndexOf(edit, &notes);
        if (!edit || i == (size_t)-1 || closing_) return QWidget::eventFilter(o, e);
        if (e->type() == QEvent::KeyPress) {
            QKeyEvent* k = static_cast<QKeyEvent*>(e);
            size_t n = 2 * rows_.size(), f = 2 * i + (notes ? 1 : 0);   // the boxes in reading order
            switch (k->key()) {
            case Qt::Key_Return:
            case Qt::Key_Enter:
                // From either box, on to the next row's address.
                if (notes) ApplyNotes(rows_[i].get());
                else ApplyEdit(rows_[i].get());
                EnsureTrailingEmptyRow();          // a filled-in last row gets a fresh empty row after it
                if (i + 1 < rows_.size()) FocusEdit(i + 1);
                return true;
            case Qt::Key_Tab:
                f = (f + 1) % n;
                FocusEdit(f / 2, f % 2 == 1);
                return true;
            case Qt::Key_Backtab:
                f = (f + n - 1) % n;
                FocusEdit(f / 2, f % 2 == 1);
                return true;
            case Qt::Key_Escape:
                edit->setText(Q(notes ? rows_[i]->notes : rows_[i]->addr));
                return true;
            }
        } else if (e->type() == QEvent::FocusOut) {
            int id = rows_[i]->id;
            QTimer::singleShot(0, this, [this, id, notes] {
                Row* r = FindRow(id);
                if (!r || closing_) return;
                if (notes) ApplyNotes(r);
                else if (ApplyEdit(r)) EnsureTrailingEmptyRow();
            });
        }
        return QWidget::eventFilter(o, e);
    }

private:
    std::vector<std::unique_ptr<Row>> rows_;
    int nextId_ = 1;
    int first_ = 0;         // index of the first visible row (row-based scrolling)
    int hover_ = -1;        // the row whose delete button the pointer is over
    int down_ = -1;         // the row whose delete button is pressed
    int gripHover_ = -1;    // the row whose handle the pointer is over
    Row* dragRow_ = nullptr;   // the row being moved by its handle
    bool dragMoved_ = false;   // and whether it has left its place
    int dragY_ = 0;            // the pointer's height while it is
    QTimer dragTimer_;         // scrolls the list while the pointer is above or below the rows shown
    double wheel_ = 0;
    bool closing_ = false;
    QScrollBar* bar_;
    QTimer tick_;
    QImage emoji_;          // the title emoji, cut out; null without an emoji font
    QPixmap icon_;

    int ClientWidth() const { return width() - (bar_->isHidden() ? 0 : bar_->width()); }
    int Fit() const { return M.RowsThatFit(height()); }

    Row* FindRow(int id)
    {
        for (auto& r : rows_) if (r->id == id) return r.get();
        return nullptr;
    }
    // The row an address or notes box belongs to, and which of the two it is.
    size_t IndexOf(QLineEdit* edit, bool* notes = nullptr) const
    {
        for (size_t i = 0; i < rows_.size(); i++) {
            if (rows_[i]->edit != edit && rows_[i]->notesEdit != edit) continue;
            if (notes) *notes = rows_[i]->notesEdit == edit;
            return i;
        }
        return (size_t)-1;
    }

    // The visible row whose handle is at `p`, or -1. The empty row at the end has none.
    int GripAt(const QPoint& p) const
    {
        Cols c = M.Columns(ClientWidth());
        for (int slot = 0; slot < Fit() && first_ + slot + 1 < (int)rows_.size(); slot++) {
            Rect g = M.Cell(c, C_GRIP, M.RowTop(slot));
            if (QRect(g.x, g.y, g.w, g.h).contains(p)) return first_ + slot;
        }
        return -1;
    }

    // Moves the row being dragged to where the pointer is, scrolling the list a row when `scroll`
    // is set (the timer's tick), and keeps the timer running while the pointer is above or below
    // the rows shown.
    void DragTo(bool scroll)
    {
        if (!dragRow_) return;
        int n = (int)rows_.size(), fit = Fit(), first = first_;
        size_t from = 0;
        while (rows_[from].get() != dragRow_) from++;
        int to = DragTarget(M, dragY_, &first, fit, n, n - 1, scroll);
        bool moved = to != (int)from, scrolled = first != first_;
        if (moved) { MoveItem(rows_, from, (size_t)to); dragMoved_ = true; }
        if (scrolled) { first_ = first; UpdateScroll(); }
        if (moved || scrolled) Layout();
        if (!DragOutside(M, dragY_, fit)) dragTimer_.stop();
        else if (!dragTimer_.isActive()) dragTimer_.start();
    }

    void EndDrag()
    {
        dragRow_ = nullptr;
        dragTimer_.stop();
        update();
        if (dragMoved_) Save();
    }

    // The visible row whose delete button is at `p`, or -1.
    int DeleteAt(const QPoint& p) const
    {
        Cols c = M.Columns(ClientWidth());
        for (int slot = 0; slot < Fit() && first_ + slot < (int)rows_.size(); slot++) {
            Rect d = M.Cell(c, C_DEL, M.RowTop(slot));
            if (QRect(d.x, d.y, d.w, d.h).contains(p)) return first_ + slot;
        }
        return -1;
    }

    void Save()
    {
        std::vector<Entry> entries;
        for (auto& r : rows_) entries.push_back(Entry{ r->addr, r->notes });
        SaveEntries(entries);
    }

    // An address or notes box: Enter, Tab and Escape handled by eventFilter.
    QLineEdit* MakeEdit(const std::string& text)
    {
        QLineEdit* edit = new QLineEdit(Q(text), this);
        edit->setFont(UiFont(FONT_PT));
        edit->installEventFilter(this);
        return edit;
    }

    Row* AddRow(const std::string& addr, const std::string& notes)
    {
        std::unique_ptr<Row> r(new Row);
        r->id = nextId_++;
        r->addr = addr;
        r->notes = notes;
        r->edit = MakeEdit(addr);
        r->edit->setPlaceholderText(kCue);
        r->notesEdit = MakeEdit(notes);
        if (!addr.empty()) r->pinger.reset(new Pinger(addr));
        rows_.push_back(std::move(r));
        return rows_.back().get();
    }

    // The list always ends with an empty row so it is obvious where a new address goes.
    void EnsureTrailingEmptyRow()
    {
        if (rows_.empty() || !rows_.back()->addr.empty()) {
            AddRow("", "");
            UpdateScroll();
            Layout();
        }
    }

    // Reads the box into the row; restarts pinging if the address changed. True if it did.
    bool ApplyEdit(Row* r)
    {
        std::string a = Trim(r->edit->text().toUtf8().toStdString());
        if (a == r->addr) return false;
        r->addr = a;
        r->pinger.reset(a.empty() ? nullptr : new Pinger(a));
        r->stats.Reset();
        Save();
        update();
        return true;
    }

    // Reads the notes box into the row, and saves when they changed. True if they did.
    bool ApplyNotes(Row* r)
    {
        std::string n = OneLine(r->notesEdit->text().toUtf8().toStdString());
        if (n == r->notes) return false;
        r->notes = n;
        Save();
        return true;
    }

    void DeleteRow(int id)
    {
        if (closing_ || dragRow_) return;
        size_t i = 0;
        while (i < rows_.size() && rows_[i]->id != id) i++;
        if (i == rows_.size()) return;
        QLineEdit* edits[2] = { rows_[i]->edit, rows_[i]->notesEdit };
        for (QLineEdit* edit : edits) edit->removeEventFilter(this);   // their focus loss is no edit to apply
        rows_.erase(rows_.begin() + (long)i);
        for (QLineEdit* edit : edits) delete edit;
        hover_ = gripHover_ = -1;
        EnsureTrailingEmptyRow();
        Save();
        UpdateScroll();
        Layout();
        FocusEdit(i < rows_.size() ? i : rows_.size() - 1);
    }

    // Puts the caret in row i's address box, or its notes box, with the text selected.
    void FocusEdit(size_t i, bool notes = false)
    {
        if (i >= rows_.size()) return;
        EnsureRowVisible((int)i);
        QLineEdit* edit = notes ? rows_[i]->notesEdit : rows_[i]->edit;
        edit->setFocus();
        edit->selectAll();
    }

    // One tick for every row: take last round's result, then release the next ping. All graphs
    // and labels therefore change in the same instant.
    void Tick()
    {
        for (auto& r : rows_) {
            int res = RES_NONE;
            if (r->pinger && r->pinger->Tick(&res)) r->stats.Record(res);
        }
        update();
    }

    // Scrolling is by whole rows: the title and header stay put, rows first_ .. first_+fit-1 are
    // shown, and any row that would not fit completely is hidden rather than drawn partially.
    void UpdateScroll()
    {
        int fit = Fit(), n = (int)rows_.size();
        first_ = ClampFirstRow(first_, n, fit);
        bool need = n > fit;
        bar_->blockSignals(true);
        bar_->setRange(0, n > fit ? n - fit : 0);
        bar_->setPageStep(fit);
        bar_->setValue(first_);
        bar_->blockSignals(false);
        if (need == bar_->isHidden()) bar_->setVisible(need);   // isHidden: isVisible is false until the window shows
        int w = bar_->sizeHint().width();
        bar_->setGeometry(width() - w, 0, w, height());
    }

    void SetFirstRow(int first)
    {
        first = ClampFirstRow(first, (int)rows_.size(), Fit());
        if (first == first_) return;
        first_ = first;
        UpdateScroll();
        Layout();
    }

    void EnsureRowVisible(int i)
    {
        int fit = Fit();
        if (i < first_) SetFirstRow(i);
        else if (i >= first_ + fit) SetFirstRow(i - fit + 1);
    }

    void Layout()
    {
        Cols c = M.Columns(ClientWidth());
        int fit = Fit();
        for (size_t i = 0; i < rows_.size(); i++) {
            bool shown = (int)i >= first_ && (int)i < first_ + fit;
            QLineEdit* edits[2] = { rows_[i]->edit, rows_[i]->notesEdit };
            for (int k = 0; k < 2; k++) {
                if (shown) {
                    Rect r = M.Cell(c, k ? C_NOTES : C_ADDR, M.RowTop((int)i - first_));
                    edits[k]->setGeometry(r.x, r.y, r.w, r.h);
                }
                if (shown == edits[k]->isHidden()) edits[k]->setVisible(shown);
            }
        }
        update();
    }

    // The palette for the theme's: a dark window, a dark theme. Qt repaints when the theme changes.
    const Palette& Colours() const { return PaletteFor(palette().window().color().lightness() < 128); }

    void PaintGraph(QPainter& p, const Rect& box, int col, const Stats& s, const Palette& P);
    void PaintDelete(QPainter& p, const Rect& box, const Rgb& colour);
    void PaintGrip(QPainter& p, const Rect& box, const Rgb& colour);
};

void Window::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    QColor text = palette().windowText().color();
    const Palette& P = Colours();
    int clientW = ClientWidth();
    Cols c = M.Columns(clientW);

    // Title, large and bold, aligned with the Address column.
    QFont logo = UiFont(FONT_LOGO_PT, true);
    p.setFont(logo);
    p.setPen(text);
    p.drawText(QRect(c.x[0], M.LogoTop(), clientW - c.x[0], M.S(LOGO_H)), Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine, kTitle);
    QFontMetrics lm(logo);
    int titleEnd = c.x[0] + lm.horizontalAdvance(kTitle);
    int x = titleEnd + M.S(10);
    int afterEmoji = x;
    if (!emoji_.isNull()) {
        // As tall as Windows draws it, its middle a pixel above the middle of the capitals.
        int h = M.S(TITLE_EMOJI_PX), w = h * emoji_.width() / emoji_.height();
        QRect caps = lm.tightBoundingRect("M");          // from the baseline, so its top is negative
        int capsMiddle = M.LogoTop() + lm.ascent() + caps.top() + caps.height() / 2;
        QRect er(titleEnd + M.S(TITLE_EMOJI_GAP), capsMiddle - M.S(1) - h / 2, w, h);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(er, emoji_);
        afterEmoji = er.right() + 1;
    } else if (!icon_.isNull()) {
        // No emoji font on this system: the app icon in its place.
        int sz = M.S(TITLE_ICON_PX);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawPixmap(QRect(x, M.LogoTop() + (M.S(LOGO_H) - sz) / 2, sz, sz), icon_);
        afterEmoji = x + sz;
    }

    // Copyright and revision, stacked, right-aligned to the window margin beside the title.
    QFont info = UiFont(FONT_INFO_PT);
    p.setFont(info);
    p.setPen(Colour(P.infoText));
    int lh = QFontMetrics(info).height();
    int iy = M.LogoTop() + (M.S(LOGO_H) - 2 * lh) / 2;
    QRect ir(afterEmoji, iy, clientW - M.S(MARGIN) - afterEmoji, lh);
    p.drawText(ir, Qt::AlignRight | Qt::AlignTop | Qt::TextSingleLine, Q(kCopyright));
    ir.translate(0, lh);
    p.drawText(ir, Qt::AlignRight | Qt::AlignTop | Qt::TextSingleLine, Q(BuildInfo(__DATE__, GIT_REV)));

    // Column headers and the line under them.
    QFont font = UiFont(FONT_PT);
    p.setFont(font);
    p.setPen(text);
    int y = M.HeaderTop();
    for (int k = 0; k < NCOL; k++) {
        Qt::Alignment align = (LeftAligned(k) ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter;
        p.drawText(QRect(c.x[k], y, c.w[k], M.S(HDR_H)), align | Qt::TextSingleLine, kColNames[k]);
    }
    p.setPen(Colour(P.headerLine));
    int ly = y + M.S(HDR_H);
    p.drawLine(c.x[0], ly, c.x[NCOL - 1] + c.w[NCOL - 1] - 1, ly);

    // The visible rows: handle, labels, charts, delete button; the boxes are widgets of their own.
    int fit = Fit();
    for (int slot = 0; slot < fit && first_ + slot < (int)rows_.size(); slot++) {
        const Row& r = *rows_[(size_t)(first_ + slot)];
        int top = M.RowTop(slot);
        std::string labels[5];
        labels[0] = r.stats.Current();
        r.stats.Summary(labels + 1);
        p.setFont(font);
        p.setPen(text);
        for (int k = C_CUR; k <= C_MAX; k++) {
            Rect cell = M.Cell(c, k, top);
            p.drawText(QRect(cell.x, cell.y, cell.w, cell.h), Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine, Q(labels[k - C_CUR]));
        }
        for (int k = C_GRAPH; k <= C_HIST; k++) PaintGraph(p, M.Cell(c, k, top), k, r.stats, P);
        int i = first_ + slot;
        if (i + 1 < (int)rows_.size()) {   // the empty row at the end has no handle: it stays last
            bool active = dragRow_ ? dragRow_ == rows_[(size_t)i].get() : i == gripHover_;
            PaintGrip(p, M.Cell(c, C_GRIP, top), active ? P.gripActive : P.grip);
        }
        PaintDelete(p, M.Cell(c, C_DEL, top), i == down_ ? P.delDown : i == hover_ ? P.delHover : P.del);
    }
}

// The chart in column col (C_GRAPH or C_HIST) of a row's statistics.
void Window::PaintGraph(QPainter& p, const Rect& box, int col, const Stats& s, const Palette& P)
{
    p.save();
    p.translate(box.x, box.y);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(0, 0, box.w, box.h, Colour(P.graphBack));
    Graph g = LayoutChart(s, col, box.w, box.h);
    int bottom = g.plot.y + g.plot.h;

    // Grid lines at 25 / 50 / 75 %.
    for (const Rect& l : g.grid) p.fillRect(l.x, l.y, l.w, l.h, Colour(P.graphGrid));

    for (const GraphBar& b : g.bars) {
        if (!b.timeout) {
            p.fillRect(b.x0, b.top, b.x1 - b.x0, bottom - b.top, Colour(P.graphBar));
        } else {
            p.fillRect(b.x0, b.top, b.x1 - b.x0, bottom - b.top, Colour(P.graphTimeout));
            p.fillRect(b.x0, b.top, b.x1 - b.x0, M.S(3), Colour(P.graphTimeoutTop));
        }
    }

    // Scale label, in the graph's top left corner and the histogram's top right, on a backing of
    // the chart's own colour that keeps it legible over tall bars.
    QFont small = UiFont(FONT_SMALL_PT);
    QFontMetrics fm(small);
    QString label = Q(g.scaleLabel);
    int lw = fm.horizontalAdvance(label);
    QRect lr(g.labelRight ? g.plot.x + g.plot.w - M.S(3) - lw : g.plot.x + M.S(3), g.plot.y + M.S(1), lw, fm.height());
    p.fillRect(lr, Colour(P.graphBack));
    p.setFont(small);
    p.setPen(Colour(P.graphLabel));
    p.drawText(lr, Qt::AlignLeft | Qt::AlignTop | Qt::TextSingleLine, label);

    p.setPen(Colour(P.graphBorder));
    p.setBrush(Qt::NoBrush);
    p.drawRect(0, 0, box.w - 1, box.h - 1);
    p.restore();
}

// The handle's six dots.
void Window::PaintGrip(QPainter& p, const Rect& box, const Rgb& colour)
{
    GripDots g = LayoutGrip(M, box);
    for (const Rect& d : g.dot) p.fillRect(d.x, d.y, d.w, d.h, Colour(colour));
}

// Red filled circle with a white X, antialiased.
void Window::PaintDelete(QPainter& p, const Rect& box, const Rgb& colour)
{
    DeleteGlyph d = LayoutDelete((float)box.w, (float)box.h);
    p.save();
    p.translate(box.x, box.y);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(Colour(colour));
    p.drawEllipse(QPointF(d.cx, d.cy), d.radius, d.radius);
    QPen x(Qt::white, d.stroke, Qt::SolidLine, Qt::FlatCap);
    p.setPen(x);
    p.drawLine(QPointF(d.cx - d.inset, d.cy - d.inset), QPointF(d.cx + d.inset, d.cy + d.inset));
    p.drawLine(QPointF(d.cx - d.inset, d.cy + d.inset), QPointF(d.cx + d.inset, d.cy - d.inset));
    p.restore();
}

int main(int argc, char** argv)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);   // Qt 6 does by itself
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
    QApplication app(argc, argv);
    app.setApplicationName("MultiPing");

    Window w;
    // The saved place, made to fit the screen it is on now; where the window system places windows
    // itself (Wayland), only the size.
    WindowRect r;
    if (LoadWindowRect(&r)) {
        r.w = FitSavedWidth(r.w, w.minimumWidth(), w.sizeHint().width());
        QScreen* screen = QGuiApplication::screenAt(QPoint((int)(r.x + r.w / 2), (int)(r.y + r.h / 2)));
        if (!screen) screen = QGuiApplication::primaryScreen();
        QRect wa = screen->availableGeometry();
        FitWindowRect(&r, wa.x(), wa.y(), wa.width(), wa.height());
        if (QGuiApplication::platformName().startsWith("wayland")) w.resize((int)r.w, (int)r.h);
        else w.setGeometry((int)r.x, (int)r.y, (int)r.w, (int)r.h);
    } else {
        w.resize(w.sizeHint());
    }

    // SIGTERM and the like close the window as the user would, so everything is saved.
    int quitFd = WatchQuitSignals();
    if (quitFd >= 0) {
        QSocketNotifier* quit = new QSocketNotifier(quitFd, QSocketNotifier::Read, &w);
        QObject::connect(quit, &QSocketNotifier::activated, [&w, quitFd] {
            char b;
            if (read(quitFd, &b, 1) > 0) w.close();
        });
    }

    w.show();
    w.FocusFirst();
    return app.exec();
}
