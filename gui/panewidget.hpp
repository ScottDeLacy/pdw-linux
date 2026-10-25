// The two message panes (monitor / filter) and their column title bars.
#pragma once
#include <QAbstractScrollArea>
#include <QString>
#include <deque>
#include <vector>
#include "pdw/decoder.hpp"

struct PaneLine {
    QString text;
    std::vector<uint8_t> colors;   // one PDW colour index per QChar
};

class PaneView : public QAbstractScrollArea {
    Q_OBJECT
public:
    PaneView(pdw::Decoder *dec, QWidget *parent = nullptr);

    void addLine(const std::string &utf8, const std::vector<uint8_t> &colors);
    void clear();
    void setMaxLines(int n);
    void refreshStyle();                 // fonts and colours from the settings
    int cellWidth() const { return cw_; }
    int lineHeight() const { return ch_; }
    int lineCount() const { return (int)lines_.size(); }

    bool hasSelection() const { return sel_active_; }
    QString selectedText() const;        // rectangular, as in the original
    QString allText() const;
    void clearSelection();

signals:
    void selectionChanged();
    void contextMenuRequested(const QPoint &globalPos);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;

private:
    pdw::Decoder *dec_;
    std::deque<PaneLine> lines_;
    int max_lines_ = 1000;
    int cw_ = 8, ch_ = 14, max_cols_ = 0;
    QFont font_;
    bool sel_active_ = false, selecting_ = false;
    int sel_r0_ = 0, sel_c0_ = 0, sel_r1_ = 0, sel_c1_ = 0;   // anchor and current (line index, column)

    void updateScrollBars();
    QPoint cellAt(const QPoint &p) const;
    void setSel(int r0, int c0, int r1, int c1);
};

// The title bar above a pane: one 3-D box per displayed column.
class HeaderBar : public QWidget {
    Q_OBJECT
public:
    HeaderBar(pdw::Decoder *dec, bool filterPane, QWidget *parent = nullptr);
    void setCell(int cellWidth) { cw_ = cellWidth; update(); }
    void setOffset(int px) { off_ = px; update(); }
    QSize sizeHint() const override { return {100, 19}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    pdw::Decoder *dec_;
    bool filter_;
    int cw_ = 8, off_ = 0;
};
