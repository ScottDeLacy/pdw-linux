#include "panewidget.hpp"
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>

namespace {
constexpr int kLeftPad = 2;

QColor rgb(uint32_t c) { return QColor((c >> 16) & 255, (c >> 8) & 255, c & 255); }

// UTF-8 text with one colour per byte -> QString with one colour per UTF-16 unit.
void decode(const std::string &utf8, const std::vector<uint8_t> &bytec, QString &out, std::vector<uint8_t> &chc)
{
    out.clear();
    chc.clear();
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned char c = utf8[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        QString s = QString::fromUtf8(utf8.data() + i, n);
        uint8_t col = i < bytec.size() ? bytec[i] : 0;
        for (int k = 0; k < s.size(); k++) chc.push_back(col);
        out += s;
        i += n;
    }
}
}  // namespace

PaneView::PaneView(pdw::Decoder *dec, QWidget *parent) : QAbstractScrollArea(parent), dec_(dec)
{
    setFrameShape(QFrame::NoFrame);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    refreshStyle();
}

void PaneView::refreshStyle()
{
    std::string face;
    dec_->get_setting("Font.FaceName", face);
    long long height = dec_->get_int("Font.Height");
    long long weight = dec_->get_int("Font.Weight");
    QFont f(QString::fromStdString(face.empty() ? "Courier New" : face));
    f.setStyleHint(QFont::TypeWriter);
    f.setFixedPitch(true);
    if (!QFontInfo(f).fixedPitch()) f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    int px = height < 0 ? int(-height * 96 / 72.0 + 0.5) : (height > 0 ? (int)height : 14);   // Windows logfont height
    f.setPixelSize(std::max(8, px));
    f.setWeight(weight >= 600 ? QFont::Bold : QFont::Normal);
    font_ = f;
    QFontMetrics fm(font_);
    cw_ = std::max(1, fm.horizontalAdvance(QLatin1Char('0')));
    ch_ = fm.height();
    QPalette p = viewport()->palette();
    p.setColor(QPalette::Window, rgb(dec_->color_rgb(1)));
    viewport()->setPalette(p);
    viewport()->setAutoFillBackground(true);
    updateScrollBars();
    viewport()->update();
}

void PaneView::setMaxLines(int n)
{
    max_lines_ = std::max(10, n);
    while ((int)lines_.size() > max_lines_) lines_.pop_front();
    updateScrollBars();
}

void PaneView::addLine(const std::string &utf8, const std::vector<uint8_t> &colors)
{
    const bool atBottom = verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 1;
    PaneLine l;
    decode(utf8, colors, l.text, l.colors);
    max_cols_ = std::max(max_cols_, (int)l.text.size());
    lines_.push_back(std::move(l));
    if ((int)lines_.size() > max_lines_) {
        lines_.pop_front();
        if (sel_active_) { sel_r0_--; sel_r1_--; if (sel_r0_ < 0 && sel_r1_ < 0) clearSelection(); }
    }
    updateScrollBars();
    if (atBottom) verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    viewport()->update();
}

void PaneView::clear()
{
    lines_.clear();
    max_cols_ = 0;
    clearSelection();
    updateScrollBars();
    viewport()->update();
}

void PaneView::updateScrollBars()
{
    const int vis = std::max(1, viewport()->height() / ch_);
    verticalScrollBar()->setRange(0, std::max(0, (int)lines_.size() - vis));
    verticalScrollBar()->setPageStep(vis);
    verticalScrollBar()->setSingleStep(std::max(1, (int)dec_->get_int("ScrollSpeed")));
    const int wpx = max_cols_ * cw_ + 2 * kLeftPad;
    horizontalScrollBar()->setRange(0, std::max(0, wpx - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());
    horizontalScrollBar()->setSingleStep(cw_);
}

void PaneView::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    const bool atBottom = verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 1;
    updateScrollBars();
    if (atBottom) verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void PaneView::paintEvent(QPaintEvent *)
{
    QPainter p(viewport());
    p.setFont(font_);
    const int first = verticalScrollBar()->value();
    const int xoff = -horizontalScrollBar()->value() + kLeftPad;
    const int rows = viewport()->height() / ch_ + 2;
    const QFontMetrics fm(font_);
    for (int r = 0; r < rows && first + r < (int)lines_.size(); r++) {
        const PaneLine &l = lines_[first + r];
        const int y = r * ch_;
        int i = 0;
        while (i < l.text.size()) {
            int j = i;
            const uint8_t col = l.colors[i];
            while (j < l.text.size() && l.colors[j] == col) j++;
            if (col != 0 && col != 1) {
                p.setPen(rgb(dec_->color_rgb(col)));
                p.drawText(QRect(xoff + i * cw_, y, (j - i) * cw_ + 4, ch_), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextDontClip,
                           l.text.mid(i, j - i));
            }
            i = j;
        }
        // monospaced cells: draw per run so a proportional fallback font still lines up
    }
    if (sel_active_) {
        int r0 = std::min(sel_r0_, sel_r1_), r1 = std::max(sel_r0_, sel_r1_);
        int c0 = std::min(sel_c0_, sel_c1_), c1 = std::max(sel_c0_, sel_c1_);
        p.setCompositionMode(QPainter::RasterOp_SourceXorDestination);
        for (int r = r0; r <= r1; r++) {
            if (r < first || r >= first + rows) continue;
            p.fillRect(xoff + c0 * cw_, (r - first) * ch_, (c1 - c0 + 1) * cw_, ch_, Qt::white);
        }
    }
}

QPoint PaneView::cellAt(const QPoint &pt) const
{
    int col = (pt.x() + horizontalScrollBar()->value() - kLeftPad) / cw_;
    int row = pt.y() / ch_ + verticalScrollBar()->value();
    row = std::max(0, std::min(row, (int)lines_.size() - 1));
    return {std::max(0, col), row};
}

void PaneView::setSel(int r0, int c0, int r1, int c1)
{
    sel_r0_ = r0; sel_c0_ = c0; sel_r1_ = r1; sel_c1_ = c1;
    sel_active_ = true;
    viewport()->update();
    emit selectionChanged();
}

void PaneView::clearSelection()
{
    if (!sel_active_) return;
    sel_active_ = false;
    viewport()->update();
    emit selectionChanged();
}

void PaneView::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton || lines_.empty()) return;
    QPoint c = cellAt(e->pos());
    selecting_ = true;
    setSel(c.y(), c.x(), c.y(), c.x());
}

void PaneView::mouseMoveEvent(QMouseEvent *e)
{
    if (!selecting_) return;
    QPoint c = cellAt(e->pos());
    setSel(sel_r0_, sel_c0_, c.y(), c.x());
}

void PaneView::mouseReleaseEvent(QMouseEvent *)
{
    selecting_ = false;
    if (sel_active_ && sel_r0_ == sel_r1_ && sel_c0_ == sel_c1_) clearSelection();   // a plain click selects nothing
}

// Double click: select the word (characters above a space) under the pointer.
void PaneView::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton || lines_.empty()) return;
    QPoint c = cellAt(e->pos());
    const QString &t = lines_[c.y()].text;
    if (c.x() >= t.size() || t[c.x()].unicode() <= 32) return;
    int a = c.x(), b = c.x();
    while (a > 0 && t[a - 1].unicode() > 32) a--;
    while (b + 1 < t.size() && t[b + 1].unicode() > 32) b++;
    setSel(c.y(), a, c.y(), b);
}

void PaneView::wheelEvent(QWheelEvent *e)
{
    const int speed = std::max(1, (int)dec_->get_int("ScrollSpeed"));
    const int steps = e->angleDelta().y() / 120;
    verticalScrollBar()->setValue(verticalScrollBar()->value() - steps * speed * 3);
}

void PaneView::contextMenuEvent(QContextMenuEvent *e) { emit contextMenuRequested(e->globalPos()); }

QString PaneView::selectedText() const
{
    if (!sel_active_) return {};
    int r0 = std::min(sel_r0_, sel_r1_), r1 = std::max(sel_r0_, sel_r1_);
    int c0 = std::min(sel_c0_, sel_c1_), c1 = std::max(sel_c0_, sel_c1_);
    QStringList out;
    for (int r = r0; r <= r1 && r < (int)lines_.size(); r++) {
        const QString &t = lines_[r].text;
        out << (c0 < t.size() ? t.mid(c0, c1 - c0 + 1) : QString());
    }
    return out.join('\n');
}

QString PaneView::allText() const
{
    QStringList out;
    for (const PaneLine &l : lines_) out << l.text;
    return out.join('\n');
}

// ---- title bar --------------------------------------------------------------------------------
HeaderBar::HeaderBar(pdw::Decoder *dec, bool filterPane, QWidget *parent) : QWidget(parent), dec_(dec), filter_(filterPane)
{
    setFixedHeight(19);
}

void HeaderBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), palette().window());
    QFont f = font();
    f.setBold(true);
    p.setFont(f);
    const auto cols = dec_->columns(filter_);
    for (size_t i = 0; i < cols.size(); i++) {
        const auto &c = cols[i];
        const int x = c.start * cw_ - off_;
        const int w = (c.id == 7) ? width() - x - 1 : c.width * cw_;
        QRect box(x, 0, w, height() - 1);
        p.setPen(palette().color(QPalette::Light));
        p.drawLine(box.left(), box.top(), box.right(), box.top());
        p.drawLine(box.left(), box.top(), box.left(), box.bottom());
        p.setPen(palette().color(QPalette::Dark));
        p.drawLine(box.left(), box.bottom(), box.right(), box.bottom());
        p.drawLine(box.right(), box.top(), box.right(), box.bottom());
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(box.adjusted(2, 0, 0, 0), Qt::AlignHCenter | Qt::AlignVCenter, QString::fromStdString(c.title));
    }
}
