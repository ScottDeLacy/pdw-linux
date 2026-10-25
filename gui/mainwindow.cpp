#include "mainwindow.hpp"
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFontDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QProcess>
#include <QScrollBar>
#include <QSettings>
#include <QStatusBar>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include "dialogs.hpp"
#include "generated/rc_data.h"
#include "generated/rc_ids.h"

using namespace rc;

namespace {
Qt::Key keyFor(const QString &k)
{
    if (k.size() == 1) return Qt::Key(k[0].toUpper().unicode());
    if (k.startsWith("VK_F")) return Qt::Key(Qt::Key_F1 + k.mid(4).toInt() - 1);
    return Qt::Key_unknown;
}

QIcon toolIcon(const char *name)
{
    QImage im(QString(":/%1.bmp").arg(name));
    if (im.isNull()) return {};
    im = im.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < im.height(); y++)
        for (int x = 0; x < im.width(); x++)
            if ((im.pixel(x, y) & 0xFFFFFF) == 0xC0C0C0) im.setPixel(x, y, 0);   // the bitmap's "button face" colour
    return QIcon(QPixmap::fromImage(im));
}
}  // namespace

// ---- signal indicator ------------------------------------------------------------------------------
namespace {
// Needle end points from upstream sigind.cpp (relative to the bitmap's top-left corner).
const int kNeedle[21][4] = {{18, 20, 6, 14},  {18, 20, 8, 12},  {18, 20, 9, 11},  {18, 20, 10, 10}, {18, 20, 11, 9},
                            {18, 20, 12, 9},  {18, 20, 13, 8},  {18, 20, 15, 8},  {18, 20, 16, 7},  {18, 20, 17, 7},
                            {18, 20, 19, 7},  {18, 20, 20, 7},  {18, 20, 22, 8},  {18, 20, 23, 8},  {18, 20, 24, 9},
                            {18, 20, 25, 9},  {18, 20, 26, 10}, {18, 20, 27, 11}, {18, 20, 28, 12}, {18, 20, 29, 13},
                            {18, 20, 30, 14}};
}

SigIndicator::SigIndicator(pdw::Decoder *dec, QWidget *parent) : QWidget(parent), dec_(dec)
{
    bmp_.load(":/sigind.bmp");
    exclam_.load(":/exclamation.bmp");
    setFixedSize(46, 56);
}

void SigIndicator::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), palette().window());
    // gauge: black field with the bitmap, needle in red
    QRect g(4, 4, bmp_.width(), bmp_.height() + 1);
    p.fillRect(g, Qt::black);
    p.drawImage(g.topLeft(), bmp_);
    const int pos = std::max(0, std::min(20, dec_->signal_position()));
    p.setPen(Qt::red);
    p.drawLine(g.left() + kNeedle[pos][0], g.top() + kNeedle[pos][1], g.left() + kNeedle[pos][2], g.top() + kNeedle[pos][3]);

    // RX quality box
    const double q = dec_->rx_quality();
    QRect box(0, 33, 46, 19);
    p.setPen(palette().color(QPalette::Dark));
    p.drawRect(box.adjusted(0, 0, -1, -1));
    QString t;
    QColor c = Qt::black;
    if (q == 0) t = "RX-Q";
    else {
        c = q < 96 ? Qt::red : QColor(0, 128, 0);
        t = q > 99.9 ? "100%" : QString::asprintf("%.1f%%", q);
    }
    QFont f = font();
    f.setPointSize(8);
    f.setBold(true);
    p.setFont(f);
    p.setPen(c);
    p.drawText(box, Qt::AlignCenter, t);
    if (q && q < 90) {
        p.fillRect(QRect(19, 5, 25, 22), Qt::black);
        p.drawImage(19, 5, exclam_);
    }
}

// ---- main window --------------------------------------------------------------------------------------
MainWindow::MainWindow(const pdw::Config &cfg, QWidget *parent) : QMainWindow(parent), cfg_(cfg)
{
    setWindowIcon(QIcon(":/pdw.ico"));
    dec_.reset(new pdw::Decoder(cfg_));

    auto *central = new QWidget(this);
    auto *lay = new QVBoxLayout(central);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    hdr1_ = new HeaderBar(dec_.get(), false, central);
    pane1_ = new PaneView(dec_.get(), central);
    hdr2_ = new HeaderBar(dec_.get(), true, central);
    pane2_ = new PaneView(dec_.get(), central);

    split_ = new QSplitter(Qt::Vertical, central);
    auto *top = new QWidget(split_);
    auto *tl = new QVBoxLayout(top);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(0);
    tl->addWidget(hdr1_);
    tl->addWidget(pane1_, 1);
    auto *bottom = new QWidget(split_);
    auto *bl = new QVBoxLayout(bottom);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(0);
    bl->addWidget(hdr2_);
    bl->addWidget(pane2_, 1);
    split_->addWidget(top);
    split_->addWidget(bottom);
    split_->setChildrenCollapsible(false);
    lay->addWidget(split_);
    setCentralWidget(central);

    sig_ = new SigIndicator(dec_.get(), this);
    buildMenus();
    buildToolbar();
    wireDecoder();
    refreshAll();

    connect(&ui_timer_, &QTimer::timeout, this, [this] {
        sig_->update();
        if (src_ && src_->live()) {}
    });
    ui_timer_.start(150);
    connect(pane1_->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) { hdr1_->setOffset(v); });
    connect(pane2_->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) { hdr2_->setOffset(v); });
    connect(pane1_, &PaneView::contextMenuRequested, this, [this](const QPoint &gp) {
        QMenu m;
        QAction *copy = m.addAction(tr("Copy Selection"));
        copy->setEnabled(pane1_->hasSelection());
        connect(copy, &QAction::triggered, this, [this] { command(IDM_COPY_SELECTION); });
        m.addSeparator();
        for (auto [t, id] : {std::pair{"Options...", IDM_OPTIONS}, {"Filter List...", IDM_FILTERS}, {"Filter Options...", IDM_FILTEROPTIONS}}) {
            QAction *a = m.addAction(tr(t));
            connect(a, &QAction::triggered, this, [this, id = id] { command(id); });
        }
        m.addSeparator();
        for (auto [t, id] : {std::pair{"Scrollback...", IDM_SCROLLBACK}, {"Clear Screen...", IDM_CLEARDISPLAY}}) {
            QAction *a = m.addAction(tr(t));
            connect(a, &QAction::triggered, this, [this, id = id] { command(id); });
        }
        m.addSeparator();
        connect(m.addAction(tr("Exit PDW")), &QAction::triggered, this, [this] { command(IDM_EXIT); });
        m.exec(gp);
    });
    connect(pane2_, &PaneView::contextMenuRequested, this, [this](const QPoint &gp) {
        QMenu m;
        QAction *copy = m.addAction(tr("Copy Selection"));
        copy->setEnabled(pane2_->hasSelection());
        connect(copy, &QAction::triggered, this, [this] { command(IDM_COPY_SELECTION); });
        m.exec(gp);
    });

    // window geometry from pdw.ini
    const int xs = (int)dec_->get_int("xSize"), ys = (int)dec_->get_int("ySize");
    resize(xs > 300 ? xs : 900, ys > 200 ? ys : 560);
    if (dec_->get_int("Maximize")) showMaximized();
    started_ = QDateTime::currentSecsSinceEpoch();
}

MainWindow::~MainWindow()
{
    stopInput();
    stopRecording();
}

void MainWindow::wireDecoder()
{
    dec_->on_line([this](int pane, const std::string &text, const std::vector<uint8_t> &colors) {
        (pane == 0 ? pane1_ : pane2_)->addLine(text, colors);
    });
    dec_->on_status([this](const std::string &s) { setWindowTitle(QString::fromStdString(s)); });
    dec_->on_tray([this](bool hide) {
        if (!hide && tray_) { showNormal(); raise(); activateWindow(); dec_->set_trayed(false); }
    });
    updateTray();
}

void MainWindow::refreshAll()
{
    pane1_->refreshStyle();
    pane2_->refreshStyle();
    pane1_->setMaxLines((int)dec_->get_int("Pane1Size"));
    pane2_->setMaxLines((int)dec_->get_int("Pane2Size"));
    hdr1_->setCell(pane1_->cellWidth());
    hdr2_->setCell(pane2_->cellWidth());
    hdr1_->update();
    hdr2_->update();
    layoutPanes();
    syncMenuState();
    setWindowTitle(QString::fromStdString(dec_->status_text()));
}

void MainWindow::layoutPanes()
{
    int pct = (int)dec_->get_int("Percent");
    if (pct < 5 || pct > 95) pct = 65;
    const int total = std::max(200, split_->height());
    split_->setSizes({total * pct / 100, total * (100 - pct) / 100});
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    static bool first = true;
    if (first) { first = false; layoutPanes(); }
}

void MainWindow::changeEvent(QEvent *e)
{
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::WindowStateChange && isMinimized() && tray_ && dec_->get_int("SystemTray")) {
        QTimer::singleShot(0, this, [this] { hide(); dec_->set_trayed(true); });
    }
}

// ---- menus -------------------------------------------------------------------------------------------------
void MainWindow::buildMenus()
{
    QMenuBar *bar = menuBar();
    QVector<QMenu *> stack;
    QMenu *current = nullptr;
    int depth = 0;
    for (const MenuItem *m = kMenu; m->kind != MenuItem::Eof; m++) {
        switch (m->kind) {
            case MenuItem::Popup:
                current = bar->addMenu(QString::fromUtf8(m->text));
                break;
            case MenuItem::Begin: depth++; break;
            case MenuItem::End: depth--; break;
            case MenuItem::Separator: if (current) current->addSeparator(); break;
            case MenuItem::Item: {
                QString text = QString::fromUtf8(m->text);
                const int tab = text.indexOf("\\t");
                if (tab >= 0) text = text.left(tab);
                QAction *a = current->addAction(text);
                a->setData(m->id);
                connect(a, &QAction::triggered, this, [this, id = m->id] { command(id); });
                actions_.insert(m->id, a);
                break;
            }
            default: break;
        }
    }
    (void)depth;

    // accelerators (also those without a menu item)
    for (const AccelDef *a = kAccel; a->key; a++) {
        Qt::Key k = keyFor(a->key);
        if (k == Qt::Key_unknown) continue;
        Qt::KeyboardModifiers mods;
        if (a->ctrl) mods |= Qt::ControlModifier;
        if (a->alt) mods |= Qt::AltModifier;
        if (a->shift) mods |= Qt::ShiftModifier;
        const QKeySequence ks(int(k) | int(mods));
        QAction *act = actions_.value(a->id);
        if (!act) {
            act = new QAction(this);
            addAction(act);
            actions_.insert(a->id, act);
            connect(act, &QAction::triggered, this, [this, id = a->id] { command(id); });
        }
        act->setShortcut(ks);
        act->setShortcutContext(Qt::WindowShortcut);
    }

    // our additions to the File menu: audio files and recording (PDW's playback/record work on the serial port only)
    QMenu *file = bar->actions().isEmpty() ? nullptr : bar->actions().first()->menu();
    if (file) {
        QAction *first = file->actions().first();
        QAction *play = new QAction(tr("&Play recording (WAV)..."), this);
        QAction *rec = new QAction(tr("&Record audio to WAV..."), this);
        QAction *arec = new QAction(tr("&Auto-record audio"), this);
        arec->setCheckable(true);
        rec->setCheckable(true);
        actions_[IDM_PLAYBACK] = play;
        actions_[IDM_RECORD] = rec;
        actions_[IDM_AUTORECORD] = arec;
        play->setShortcut(QKeySequence("Shift+Alt+P"));
        rec->setShortcut(QKeySequence("Shift+Alt+R"));
        arec->setShortcut(QKeySequence("Shift+Alt+A"));
        for (QAction *a : {play, rec, arec}) {
            const int id = a == play ? IDM_PLAYBACK : a == rec ? IDM_RECORD : IDM_AUTORECORD;
            connect(a, &QAction::triggered, this, [this, id] { command(id); });
        }
        file->insertAction(first, play);
        file->insertAction(first, rec);
        file->insertAction(first, arec);
        file->insertSeparator(first);
    }

    // checkable items
    mode_group_ = new QActionGroup(this);
    for (int id : {IDM_POCSAGFLEX, IDM_ACARS, IDM_MOBITEX, IDM_ERMES}) {
        if (QAction *a = actions_.value(id)) { a->setCheckable(true); mode_group_->addAction(a); }
    }
    for (int id : {IDM_FILTERFILE_EN, IDM_FILTERCOMMANDFILE})
        if (QAction *a = actions_.value(id)) a->setCheckable(true);

    // Character Set menu: Default + the tables of language.df
    for (QAction *ma : bar->actions())
        if (ma->menu() && ma->text().contains("Character")) lang_menu_ = ma->menu();
    if (lang_menu_) {
        lang_group_ = new QActionGroup(this);
        QAction *def = actions_.value(IDM_ENGLISH);
        if (def) { def->setCheckable(true); lang_group_->addAction(def); }
        const auto langs = dec_->languages();
        for (size_t i = 0; i < langs.size(); i++) {
            QAction *a = lang_menu_->addAction(QString::fromStdString(langs[i]).trimmed());
            a->setCheckable(true);
            lang_group_->addAction(a);
            const int id = IDM_ENGLISH + 1 + (int)i;
            actions_[id] = a;
            connect(a, &QAction::triggered, this, [this, id] { command(id); });
        }
    }
}

void MainWindow::syncMenuState()
{
    const pdw::Mode m = dec_->mode();
    auto chk = [&](int id, bool on) { if (QAction *a = actions_.value(id)) { QSignalBlocker b(a); a->setChecked(on); } };
    chk(IDM_POCSAGFLEX, m == pdw::Mode::Paging);
    chk(IDM_ACARS, m == pdw::Mode::Acars);
    chk(IDM_MOBITEX, m == pdw::Mode::Mobitex);
    chk(IDM_ERMES, m == pdw::Mode::Ermes);
    chk(IDM_FILTERFILE_EN, dec_->get_int("FilterFileEnabled", "Filter") != 0);
    chk(IDM_FILTERCOMMANDFILE, dec_->get_int("FilterCmdFileEnabled", "Filter") != 0);
    chk(IDM_RECORD, recorder_.isOpen() && !auto_rec_);
    chk(IDM_AUTORECORD, auto_rec_);
    const int li = (int)dec_->get_int("LangIndex");
    chk(IDM_ENGLISH + li, true);
    if (QAction *t = actions_.value(IDT_TOOLBAR_BTN9)) { QSignalBlocker b(t); t->setChecked(paused_); }
}

void MainWindow::buildToolbar()
{
    QToolBar *tb = addToolBar("PDW");
    tb->setMovable(false);
    tb->setIconSize(QSize(18, 18));
    struct B { const char *icon; int id; const char *tip; };
    const B buttons[] = {
        {"open", IDT_TOOLBAR_BTN0, "Open/Close Logfile"}, {"copy", IDT_TOOLBAR_BTN1, "Copy selection"},
        {"cpyUP", IDT_TOOLBAR_BTN2, "Copy monitor window"}, {"cpyLO", IDT_TOOLBAR_BTN3, "Copy filter window"},
        {"save", IDT_TOOLBAR_BTN4, "Save clipboard"}, {"prt", IDT_TOOLBAR_BTN5, "Print clipboard"},
        {"options", IDT_TOOLBAR_BTN6, "Options"}, {"filter", IDT_TOOLBAR_BTN7, "Filters"},
        {"stats", IDT_TOOLBAR_BTN8, "Show statistics"}, {"pause", IDT_TOOLBAR_BTN9, "Pause PDW"},
        {"help", IDT_TOOLBAR_BTN10, "Help"}, {"clr", IDT_TOOLBAR_BTN11, "Clear screen"},
        {"mode", IDT_TOOLBAR_BTN12, "Change mode"}};
    for (const B &b : buttons) {
        QAction *a = tb->addAction(toolIcon(b.icon), tr(b.tip));
        a->setToolTip(tr(b.tip));
        if (b.id == IDT_TOOLBAR_BTN9) a->setCheckable(true);
        actions_[b.id] = a;
        connect(a, &QAction::triggered, this, [this, id = b.id] { command(id); });
        if (b.id == IDT_TOOLBAR_BTN0 || b.id == IDT_TOOLBAR_BTN5 || b.id == IDT_TOOLBAR_BTN8 || b.id == IDT_TOOLBAR_BTN9 ||
            b.id == IDT_TOOLBAR_BTN12 - 2)
            tb->addSeparator();
    }
    auto *spacer = new QWidget(tb);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);
    tb->addWidget(sig_);
}

// ---- commands ---------------------------------------------------------------------------------------------
void MainWindow::command(int id)
{
    switch (id) {
        case IDM_LOGFILE: case IDT_TOOLBAR_BTN0: dlg::logfile(this); break;
        case IDM_EXIT: close(); break;
        case IDM_COPY_SELECTION: case IDT_TOOLBAR_BTN1: {
            PaneView *p = pane1_->hasSelection() ? pane1_ : pane2_->hasSelection() ? pane2_ : nullptr;
            if (p) QApplication::clipboard()->setText(p->selectedText());
            break;
        }
        case IDM_COPY_UPPER: case IDT_TOOLBAR_BTN2: QApplication::clipboard()->setText(pane1_->allText()); break;
        case IDM_COPY_LOWER: case IDT_TOOLBAR_BTN3: QApplication::clipboard()->setText(pane2_->allText()); break;
        case IDM_COPY_SAVE: case IDT_TOOLBAR_BTN4: {
            const QString path = QFileDialog::getSaveFileName(this, tr("Save clipboard"),
                                                              QString::fromStdString(dec_->data_dir()) + "/clipboard.txt", tr("Text files (*.txt);;All files (*)"));
            if (path.isEmpty()) break;
            QFile f(path);
            if (f.open(QIODevice::WriteOnly)) f.write(QApplication::clipboard()->text().toUtf8());
            break;
        }
        case IDM_COPY_PRINT: case IDT_TOOLBAR_BTN5: QMessageBox::information(this, "PDW", tr("Print the clipboard: paste it into a text editor and print from there.")); break;
        case IDM_INTERFACE: dlg::setup(this); break;
        case IDM_VOLUME: {
            for (const char *prog : {"pavucontrol", "gnome-control-center sound", "kcmshell6 kcm_pulseaudio", "systemsettings kcm_pulseaudio"}) {
                QStringList parts = QProcess::splitCommand(QString(prog));
                if (QProcess::startDetached(parts.takeFirst(), parts)) return;
            }
            QMessageBox::information(this, "PDW", tr("Open your system's sound settings to adjust the recording level."));
            break;
        }
        case IDM_OPTIONS: case IDT_TOOLBAR_BTN6: dlg::options(this); break;
        case IDM_GENERAL: dlg::general(this); break;
        case IDM_MAIL: dlg::mail(this); break;
        case IDM_FILTERS: case IDT_TOOLBAR_BTN7: dlg::filters(this); break;
        case IDM_FILTEROPTIONS: dlg::filterOptions(this); break;
        case IDM_RELOAD: {
            const size_t before = dec_->filter_count();
            const bool ok = dec_->reload_filters();
            QMessageBox::information(this, tr("PDW Reloaded filters"),
                                     ok ? tr("Old  number  of filters :  %1\nNew number of filters :  %2").arg(before).arg(dec_->filter_count())
                                        : tr("Unable to load filters.ini."));
            break;
        }
        case IDM_RESET_HITCOUNTERS:
            if (QMessageBox::question(this, tr("PDW Filters"), tr("Reset ALL hitcounters?")) == QMessageBox::Yes) dec_->reset_hitcounters(true);
            break;
        case IDM_FILTERFILE_EN: dec_->set_int("FilterFileEnabled", actions_[id]->isChecked(), "Filter"); break;
        case IDM_FILTERCOMMANDFILE: dec_->set_int("FilterCmdFileEnabled", actions_[id]->isChecked(), "Filter"); break;
        case IDM_CLEARDISPLAY: case IDT_TOOLBAR_BTN11: dlg::clearScreen(this); break;
        case IDM_COLOR: dlg::colors(this); break;
        case IDM_FONT: {
            bool ok = false;
            QFont f = QFontDialog::getFont(&ok, pane1_->font(), this, tr("PDW Font"), QFontDialog::MonospacedFonts);
            if (ok) {
                dec_->set_setting("Font.FaceName", f.family().toStdString());
                dec_->set_int("Font.Height", -int(f.pointSizeF() + 0.5));
                dec_->set_int("Font.Weight", f.bold() ? 700 : 400);
                refreshAll();
            }
            break;
        }
        case IDM_SCREENOPTIONS: dlg::screenOptions(this); break;
        case IDM_SCROLLBACK: dlg::scrollback(this); break;
        case IDM_SYSTEMTRAY: dlg::systemTray(this); break;
        case IDM_POCSAGFLEX: dec_->set_mode(pdw::Mode::Paging); refreshAll(); break;
        case IDM_ACARS: dec_->set_mode(pdw::Mode::Acars); refreshAll(); break;
        case IDM_MOBITEX: dec_->set_mode(pdw::Mode::Mobitex); refreshAll(); break;
        case IDM_ERMES: dec_->set_mode(pdw::Mode::Ermes); refreshAll(); break;
        case IDT_TOOLBAR_BTN12: {   // change mode: step through the Monitor menu
            const pdw::Mode next[] = {pdw::Mode::Acars, pdw::Mode::Mobitex, pdw::Mode::Ermes, pdw::Mode::Paging};
            dec_->set_mode(next[int(dec_->mode())]);
            refreshAll();
            break;
        }
        case IDM_MONSTAT: dlg::statistics(this); break;
        case IDM_HELP: case IDT_TOOLBAR_BTN10: {
            const QStringList cand = {QString::fromStdString(cfg_.resource_dir) + "/PDW.pdf", QString::fromStdString(cfg_.dir) + "/PDW.pdf"};
            for (const QString &c : cand)
                if (QFile::exists(c)) { QDesktopServices::openUrl(QUrl::fromLocalFile(c)); return; }
            QMessageBox::warning(this, tr("PDW Manual"), tr("Unable to find PDW.pdf"));
            break;
        }
        case IDM_DEBUG: dlg::debug(this); break;
        case IDM_ABOUT: dlg::about(this); break;
        case IDM_PANE_SWITCH:
            dec_->set_int("Percent", 100 - (int)dec_->get_int("Percent"));
            layoutPanes();
            break;
        case IDT_TOOLBAR_BTN9: setPaused(actions_[id]->isChecked()); break;
        case IDM_PLAYBACK: {
            const QString path = QFileDialog::getOpenFileName(this, tr("Play recording"), QString::fromStdString(cfg_.dir),
                                                              tr("Audio (*.wav);;All files (*)"));
            if (!path.isEmpty()) playFile(path);
            break;
        }
        case IDM_RECORD:
            if (recorder_.isOpen()) stopRecording();
            else {
                const QString path = QFileDialog::getSaveFileName(this, tr("Record audio"), QString::fromStdString(cfg_.dir), tr("WAV (*.wav)"));
                if (!path.isEmpty()) startRecording(path);
            }
            syncMenuState();
            break;
        case IDM_AUTORECORD: autoRecord(); break;
        default:
            if (id > IDM_ENGLISH && id < IDM_ENGLISH + 10) {
                dec_->set_int("LangIndex", id - IDM_ENGLISH);
                dec_->apply_settings();
                syncMenuState();
            } else if (id == IDM_ENGLISH) {
                dec_->set_int("LangIndex", 0);
                dec_->apply_settings();
                syncMenuState();
            }
            break;
    }
}

void MainWindow::settingsChanged()
{
    dec_->apply_settings();
    refreshAll();
}

void MainWindow::setPaused(bool p)
{
    paused_ = p;
    if (src_) src_->setPaused(p);
    syncMenuState();
}

// ---- input ---------------------------------------------------------------------------------------------------
void MainWindow::attachSource(InputSource *s)
{
    stopInput();
    src_.reset(s);
    connect(s, &InputSource::samples, this, &MainWindow::onSamples);
    connect(s, &InputSource::failed, this, [this](const QString &why) {
        QMessageBox::warning(this, "PDW", why);
        stopInput();
    });
    connect(s, &InputSource::finished, this, [this] {
        dec_->finish();
        stopInput();
        emit inputFinished();
    });
    s->setPaused(paused_);
}

void MainWindow::stopInput()
{
    if (src_) {
        src_->stop();
        src_.reset();
    }
    resampler_.reset();
}

QString MainWindow::inputDescription() const { return src_ ? src_->description() : QString("none"); }

bool MainWindow::playFile(const QString &path, bool realtime)
{
    auto *s = new WavSource(path, realtime);
    QString err;
    if (!s->load(&err)) {
        QMessageBox::warning(this, "PDW", err);
        delete s;
        return false;
    }
    attachSource(s);
    s->start();
    return true;
}

void MainWindow::useStdin(double rate)
{
    auto *s = new StdinSource(rate);
    attachSource(s);
    s->start();
}

void MainWindow::useRtlFm(const RtlFmSource::Params &p)
{
    auto *s = new RtlFmSource(p);
    attachSource(s);
    s->start();
}

void MainWindow::useDevice(int index, int rate)
{
    auto *s = new DeviceSource(index, rate);
    attachSource(s);
    s->start();
}

void MainWindow::onSamples(const QByteArray &s16, double rate)
{
    const int16_t *p = reinterpret_cast<const int16_t *>(s16.constData());
    const size_t n = size_t(s16.size()) / 2;
    if (recorder_.isOpen()) recorder_.write(p, n);
    if (paused_) return;
    const float gain = 1.0f;
    if (!resampler_ || src_rate_ != rate) {
        resampler_.reset(rate == pdw::kSampleRate ? nullptr : new pdw::Resampler(rate));
        src_rate_ = rate;
    }
    if (!resampler_) {
        dec_->feed_s16(p, n, gain);
    } else {
        std::vector<int16_t> out;
        resampler_->process(p, n, out);
        dec_->feed_s16(out.data(), out.size(), gain);
    }
}

void MainWindow::startRecording(const QString &path)
{
    stopRecording();
    recorder_.open(path.toStdString(), src_rate_ > 0 ? src_rate_ : pdw::kSampleRate);
}

void MainWindow::stopRecording()
{
    recorder_.close();
    auto_rec_ = false;
}

void MainWindow::autoRecord()
{
    if (auto_rec_) {
        stopRecording();
        QMessageBox::information(this, tr("PDW Recording"), tr("Recording Stopped!"));
    } else {
        QDir().mkpath(QString::fromStdString(cfg_.dir) + "/Recordings");
        const QString name = QString("%1/Recordings/%2.wav").arg(QString::fromStdString(cfg_.dir),
                                                                  QDateTime::currentDateTime().toString("yyMMdd [hhmmss]"));
        startRecording(name);
        auto_rec_ = true;
    }
    syncMenuState();
}

QString MainWindow::startedText() const { return QDateTime::fromSecsSinceEpoch(started_).toString("dd-MM-yy HH:mm:ss"); }

// System tray (Display > Systemtray): minimise to the tray, restore on new messages as configured.
void MainWindow::updateTray()
{
    const bool on = dec_->get_int("SystemTray") != 0 && QSystemTrayIcon::isSystemTrayAvailable();
    if (on && !tray_) {
        tray_ = new QSystemTrayIcon(windowIcon(), this);
        tray_->setToolTip("PDW");
        auto *menu = new QMenu(this);
        connect(menu->addAction(tr("Restore")), &QAction::triggered, this, [this] { showNormal(); raise(); activateWindow(); dec_->set_trayed(false); });
        connect(menu->addAction(tr("Filter List...")), &QAction::triggered, this, [this] { command(IDM_FILTERS); });
        connect(menu->addAction(tr("Exit PDW")), &QAction::triggered, this, [this] { dec_->set_trayed(false); close(); });
        tray_->setContextMenu(menu);
        connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
            if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) { showNormal(); raise(); activateWindow(); dec_->set_trayed(false); }
        });
        tray_->show();
    } else if (!on && tray_) {
        delete tray_;
        tray_ = nullptr;
        dec_->set_trayed(false);
        if (isHidden()) showNormal();
    }
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (dec_->get_int("ConfirmExit") && QMessageBox::question(this, tr("PDW"), tr("Exit PDW?")) != QMessageBox::Yes) {
        e->ignore();
        return;
    }
    saveWindowState();
    stopInput();
    dec_->finish();
    if (!cfg_.dir.empty()) dec_->save_settings();
    e->accept();
}

void MainWindow::saveWindowState()
{
    dec_->set_int("Maximize", isMaximized());
    if (!isMaximized()) {
        dec_->set_int("xPos", x());
        dec_->set_int("yPos", y());
        dec_->set_int("xSize", width());
        dec_->set_int("ySize", height());
    }
    const auto sz = split_->sizes();
    if (sz.size() == 2 && sz[0] + sz[1] > 0) dec_->set_int("Percent", sz[0] * 100 / (sz[0] + sz[1]));
}
