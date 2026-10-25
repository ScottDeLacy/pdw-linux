// Dialogs of the original PDW, rebuilt on the resource-script layouts. Behaviour follows the
// dialog procedures in upstream PDW.cpp (enable/disable rules, validation, what is stored where).
#include "dialogs.hpp"
#include <QApplication>
#include <QColorDialog>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QPainter>
#include <QSysInfo>
#include "generated/rc_ids.h"
#include "mainwindow.hpp"
#include "rcdialog.hpp"

using namespace rc;

namespace {

int geti(pdw::Decoder &d, const char *key, const char *sec = "PDW") { return (int)d.get_int(key, sec); }
void seti(pdw::Decoder &d, const char *key, long long v, const char *sec = "PDW") { d.set_int(key, v, sec); }
QString gets(pdw::Decoder &d, const char *key, const char *sec = "PDW")
{
    std::string v;
    d.get_setting(key, v, sec);
    return QString::fromStdString(v);
}
void sets(pdw::Decoder &d, const char *key, const QString &v, const char *sec = "PDW") { d.set_setting(key, v.toStdString(), sec); }

// Apply, refresh the display and save pdw.ini (WriteSettings() at the end of each upstream OK handler).
void commit(MainWindow *w)
{
    w->settingsChanged();
    if (!w->dataDir().isEmpty()) w->decoder().save_settings();
}

void warn(QWidget *p, const QString &title, const QString &text) { QMessageBox::critical(p, title, text); }

QStringList columnNames(pdw::Decoder &d)
{
    switch (d.mode()) {
        case pdw::Mode::Mobitex: return {"-", "MAN", "Time", "Date", "Sender", "Type", "Bitrate", "Message"};
        case pdw::Mode::Acars: return {"-", "Aircr.Reg", "Time", "Date", "Msg.No.", "DBI", "Mode", "Message"};
        default: return {"-", "Address", "Time", "Date", "Mode", "Type", "Bitrate", "Message"};
    }
}

QString logColumns(RcDialog &dlg, int base)
{
    QString s;
    for (int i = 1; i <= 7; i++)
        if (dlg.checked(base + i)) s += QChar('0' + i);
    return s;
}

void relabelColumns(RcDialog &dlg, pdw::Decoder &d, int base)
{
    if (d.mode() == pdw::Mode::Mobitex) {
        dlg.setText(base + 1, "MAN");
        dlg.setText(base + 4, "Sender");
    } else if (d.mode() == pdw::Mode::Acars) {
        dlg.setText(base + 1, "Reg.No.");
        dlg.setText(base + 4, "Msg.No.");
        dlg.setText(base + 5, "DBI");
        dlg.setText(base + 6, "Mode");
    }
}

}  // namespace

namespace dlg {

// ---- Display / Clear Screen ---------------------------------------------------------------------------
bool clearScreen(MainWindow *w)
{
    static bool p1 = true, p2 = true;
    RcDialog d("CLEARSCREENDLGBOX", w);
    d.setChecked(IDC_CLEAR_PANE1, p1);
    d.setChecked(IDC_CLEAR_PANE2, p2);
    if (d.exec() != QDialog::Accepted) return false;
    p1 = d.checked(IDC_CLEAR_PANE1);
    p2 = d.checked(IDC_CLEAR_PANE2);
    if (p1) w->monitorPane()->clear();
    if (p2) w->filterPane()->clear();
    w->decoder().clear_panes(p1, p2);
    return true;
}

// ---- Display / Scrollback -------------------------------------------------------------------------------
bool scrollback(MainWindow *w)
{
    auto &dec = w->decoder();
    RcDialog d("SCROLLDLGBOX", w);
    d.get<QLineEdit>(IDC_SCROLLPANE1)->setMaxLength(4);
    d.get<QLineEdit>(IDC_SCROLLPANE2)->setMaxLength(4);
    d.get<QLineEdit>(IDC_PERCENTPANE1)->setMaxLength(2);
    d.setText(IDC_SCROLLPANE1, QString::number(geti(dec, "Pane1Size")));
    d.setText(IDC_SCROLLPANE2, QString::number(geti(dec, "Pane2Size")));
    d.fillCombo(IDC_SCROLLSPEED, {"1 Line", "5 Lines", "10 Lines", "Page"});
    switch (geti(dec, "ScrollSpeed")) {
        case 0: d.setComboIndex(IDC_SCROLLSPEED, 3); break;
        case 1: d.setComboIndex(IDC_SCROLLSPEED, 0); break;
        case 10: d.setComboIndex(IDC_SCROLLSPEED, 2); break;
        default: d.setComboIndex(IDC_SCROLLSPEED, 1); break;
    }
    d.setText(IDC_PERCENTPANE1, QString::number(geti(dec, "Percent")));
    d.setText(IDC_PERCENTPANE2, QString::number(100 - geti(dec, "Percent")));
    QObject::connect(d.get<QLineEdit>(IDC_PERCENTPANE1), &QLineEdit::textChanged, &d,
                     [&d](const QString &t) { d.setText(IDC_PERCENTPANE2, QString::number(100 - t.toInt())); });
    for (;;) {
        if (d.exec() != QDialog::Accepted) return false;
        const int p1 = d.text(IDC_SCROLLPANE1).toInt(), p2 = d.text(IDC_SCROLLPANE2).toInt(), pct = d.text(IDC_PERCENTPANE1).toInt();
        if (p1 < 1000) { warn(&d, "PDW Scrollback", "Message scrollback minimum is 1000!"); continue; }
        if (p2 < 200) { warn(&d, "PDW Scrollback", "Message scrollback minimum is 200!"); continue; }
        if (pct < 10 || pct > 90) { warn(&d, "PDW Scrollback", "Please enter a percentage between 10 and 90!"); continue; }
        seti(dec, "Pane1Size", p1);
        seti(dec, "Pane2Size", p2);
        seti(dec, "Percent", pct);
        static const int speeds[] = {1, 5, 10, 0};
        seti(dec, "ScrollSpeed", speeds[std::max(0, d.comboIndex(IDC_SCROLLSPEED))]);
        commit(w);
        return true;
    }
}

// ---- Options / General -------------------------------------------------------------------------------------
bool general(MainWindow *w)
{
    auto &dec = w->decoder();
    RcDialog d("GENERALOPTIONSDLGBOX", w);
    const int block = geti(dec, "BlockDuplicate");
    d.setChecked(IDC_CONFIRMEXIT, geti(dec, "ConfirmExit"));
    d.setChecked(IDC_BLOCKDUPLICATE, block != 0);
    d.setChecked(IDC_BLOCKEDTXT, block & 0x04);
    d.setText(IDC_BLOCKDUPTIMER, QString::number(block >> 4));
    d.get<QLineEdit>(IDC_BLOCKDUPTIMER)->setMaxLength(1);
    d.fillCombo(IDC_BLOCKDUPOPTION, {"Address+message is the same as previous", "Message is the same as previous",
                                     "Address(+function)+message is repeated within timer"});
    d.setComboIndex(IDC_BLOCKDUPOPTION, std::max(0, (block & 0x03) - 1));
    d.setChecked(IDC_LINEFEEDS, geti(dec, "Linefeed"));
    d.setChecked(IDC_SEPARATOR, geti(dec, "Separator"));
    d.setChecked(geti(dec, "MonthNumber") ? IDC_MONTHNUMBER : IDC_MONTHNUMBER2, true);
    d.fillCombo(IDC_DATEFORMAT, {"DD-MM-YY", "MM-DD-YY", "YY-MM-DD"});
    d.setComboIndex(IDC_DATEFORMAT, geti(dec, "DateFormat"));
    const QString defPath = QString::fromStdString(dec.data_dir()) + "/Logfiles";
    d.setText(IDC_LOGFILEPATH, gets(dec, "LogfilePath"));
    auto syncPath = [&] {
        const bool def = QDir::cleanPath(d.text(IDC_LOGFILEPATH)).compare(QDir::cleanPath(defPath), Qt::CaseInsensitive) == 0;
        d.setText(IDC_LOGFILEPATHTEXT, def ? "Path for logfiles (default)" : "Path for logfiles");
        d.setEnabled(IDC_LOGFILEPATHDEFAULT, !def);
    };
    syncPath();
    auto syncBlock = [&] {
        const bool on = d.checked(IDC_BLOCKDUPLICATE);
        d.setEnabled(IDC_BLOCKDUPOPTION, on);
        d.setEnabled(IDC_BLOCKEDTXT, on);
        d.setEnabled(IDC_BLOCKDUPTIMER, on && d.comboIndex(IDC_BLOCKDUPOPTION) == 2);
    };
    syncBlock();
    QObject::connect(d.get<QCheckBox>(IDC_BLOCKDUPLICATE), &QCheckBox::toggled, &d, syncBlock);
    QObject::connect(d.get<QComboBox>(IDC_BLOCKDUPOPTION), &QComboBox::currentIndexChanged, &d, syncBlock);
    QObject::connect(d.button(IDC_LOGFILEPATHBROWSE), &QPushButton::clicked, &d, [&] {
        const QString p = QFileDialog::getExistingDirectory(&d, "Select a directory...", d.text(IDC_LOGFILEPATH));
        if (!p.isEmpty()) { d.setText(IDC_LOGFILEPATH, p); syncPath(); }
    });
    QObject::connect(d.button(IDC_LOGFILEPATHDEFAULT), &QPushButton::clicked, &d, [&] { d.setText(IDC_LOGFILEPATH, defPath); syncPath(); });
    for (;;) {
        if (d.exec() != QDialog::Accepted) return false;
        const bool on = d.checked(IDC_BLOCKDUPLICATE);
        const int opt = d.comboIndex(IDC_BLOCKDUPOPTION);
        if (geti(dec, "FlexGroupMode") && opt == 1) {
            warn(&d, "PDW Block Duplicate Messages", "When using Flex Group Mode, it's not possible\nto block messages only on the previous text");
            continue;
        }
        int v = 0;
        if (on) {
            if (opt == 2 && d.text(IDC_BLOCKDUPTIMER).toInt() == 0) {
                warn(&d, "PDW Block Duplicate Timer", "0 is not a valid value!");
                continue;
            }
            v = 1 + opt + (d.checked(IDC_BLOCKEDTXT) ? 4 : 0) + (d.text(IDC_BLOCKDUPTIMER).toInt() << 4);
        }
        seti(dec, "ConfirmExit", d.checked(IDC_CONFIRMEXIT));
        seti(dec, "MonthNumber", d.checked(IDC_MONTHNUMBER));
        seti(dec, "BlockDuplicate", v);
        seti(dec, "Linefeed", d.checked(IDC_LINEFEEDS));
        seti(dec, "Separator", d.checked(IDC_SEPARATOR));
        seti(dec, "DateFormat", d.comboIndex(IDC_DATEFORMAT));
        sets(dec, "LogfilePath", d.text(IDC_LOGFILEPATH));
        commit(w);
        return true;
    }
}

// ---- Display / Screen Options -----------------------------------------------------------------------------------
bool screenOptions(MainWindow *w)
{
    auto &dec = w->decoder();
    RcDialog d("SCREENOPTIONSDLGBOX", w);
    const QStringList names = columnNames(dec);
    const int fgm = geti(dec, "FlexGroupMode");
    for (int i = 1; i <= 7; i++) {
        d.fillCombo(IDC_SCREEN_COLUMN + i, names);
        d.setComboIndex(IDC_SCREEN_COLUMN + i, geti(dec, QString("ScreenCol%1").arg(i).toUtf8().constData()));
    }
    d.setChecked(IDC_FLEXGROUPMODE, fgm);
    d.setChecked(IDC_FGM_LOGGING, fgm & 0x02);
    d.setChecked(IDC_FGM_COMBINE, fgm & 0x04);
    d.setChecked(IDC_FGM_HIDEGROUPCODES, fgm & 0x08);
    const bool paging = dec.mode() == pdw::Mode::Paging;
    auto syncGroup = [&](bool on) {
        for (int i = 1; i <= 7; i++) d.setEnabled(IDC_SCREEN_COLUMN + i, !on);
        d.setEnabled(IDC_FGM_LOGGING, on);
        d.setEnabled(IDC_FGM_COMBINE, on);
        d.setEnabled(IDC_FGM_HIDEGROUPCODES, on);
    };
    d.setEnabled(IDC_FLEXGROUPMODE, paging);
    if (paging) syncGroup(fgm != 0);
    else { for (int id : {IDC_FGM_LOGGING, IDC_FGM_COMBINE, IDC_FGM_HIDEGROUPCODES}) d.setEnabled(id, false); }
    QObject::connect(d.get<QCheckBox>(IDC_FLEXGROUPMODE), &QCheckBox::toggled, &d, [&](bool state) {
        const int sel[2][7] = {{1, 2, 3, 4, 5, 6, 7}, {2, 3, 1, 7, 0, 0, 0}};
        for (int i = 1; i <= 7; i++) d.setComboIndex(IDC_SCREEN_COLUMN + i, sel[state ? 1 : 0][i - 1]);
        syncGroup(state);
    });
    // "Message" ends the list; "-" pulls the next column up (as in the original)
    for (int i = 1; i <= 7; i++)
        QObject::connect(d.get<QComboBox>(IDC_SCREEN_COLUMN + i), &QComboBox::activated, &d, [&](int) {
            for (int c = 1; c <= 7; c++) {
                const int cur = d.comboIndex(IDC_SCREEN_COLUMN + c);
                if (cur == 7) { for (int k = c + 1; k <= 7; k++) d.setComboIndex(IDC_SCREEN_COLUMN + k, 0); break; }
                if (cur == 0 && c < 7) {
                    const int next = d.comboIndex(IDC_SCREEN_COLUMN + c + 1);
                    if (next != 0) { d.setComboIndex(IDC_SCREEN_COLUMN + c, next); d.setComboIndex(IDC_SCREEN_COLUMN + c + 1, 0); }
                }
            }
        });
    for (;;) {
        if (d.exec() != QDialog::Accepted) return false;
        int last = 0;
        bool bad = false;
        for (int i = 1; i < 7 && !bad; i++) {
            const int cur = d.comboIndex(IDC_SCREEN_COLUMN + i);
            if (!cur) {
                if (last != 7) { warn(&d, "PDW Screen Options", "The last column always has to be 'message'!"); bad = true; }
                break;
            }
            last = cur;
            for (int j = i + 1; j < 7; j++)
                if (cur == d.comboIndex(IDC_SCREEN_COLUMN + j)) {
                    warn(&d, "PDW Screen Options", QString("Item '%1' has been selected more than once!").arg(names[cur]));
                    bad = true;
                    break;
                }
        }
        if (bad) continue;
        for (int i = 1; i <= 7; i++) seti(dec, QString("ScreenCol%1").arg(i).toUtf8().constData(), d.comboIndex(IDC_SCREEN_COLUMN + i));
        int g = d.checked(IDC_FLEXGROUPMODE) ? 1 : 0;
        if (g) g += (d.checked(IDC_FGM_LOGGING) ? 0x02 : 0) + (d.checked(IDC_FGM_COMBINE) ? 0x04 : 0) + (d.checked(IDC_FGM_HIDEGROUPCODES) ? 0x08 : 0);
        seti(dec, "FlexGroupMode", g);
        commit(w);
        return true;
    }
}

// ---- File / Logfile ----------------------------------------------------------------------------------------------------
bool logfile(MainWindow *w)
{
    auto &dec = w->decoder();
    RcDialog d("LOGFILEDLGBOX", w);
    d.setChecked(IDC_LOGFILEEN, geti(dec, "LogFileEnabled"));
    d.setChecked(IDC_LOGFILEDATE, geti(dec, "LogFileUseDate"));
    relabelColumns(d, dec, IDC_LOGCOLUMN);
    const QString cols = gets(dec, "ColLogfile");
    for (int i = 1; i <= 7; i++) d.setChecked(IDC_LOGCOLUMN + i, cols.contains(QChar('0' + i)));
    const QString dateName = QDateTime::currentDateTime().toString(geti(dec, "MonthNumber") ? "yyMMdd" : "yyMMMdd").toUpper() + ".log";
    auto sync = [&] {
        const bool en = d.checked(IDC_LOGFILEEN), date = d.checked(IDC_LOGFILEDATE);
        d.setEnabled(IDC_LOGFILEDATE, en);
        d.setEnabled(IDC_LOGFILE, en && !date);
        d.setEnabled(IDC_LOGBROWSE, en && !date);
        for (int i = 1; i <= 7; i++) d.setEnabled(IDC_LOGCOLUMN + i, en);
        d.setText(IDC_LOGFILE, date ? dateName : gets(dec, "LogFile"));
    };
    sync();
    QObject::connect(d.get<QCheckBox>(IDC_LOGFILEEN), &QCheckBox::toggled, &d, sync);
    QObject::connect(d.get<QCheckBox>(IDC_LOGFILEDATE), &QCheckBox::toggled, &d, sync);
    QObject::connect(d.button(IDC_LOGBROWSE), &QPushButton::clicked, &d, [&] {
        const QString p = QFileDialog::getOpenFileName(&d, "Open Logfile...", gets(dec, "LogfilePath"), "Log Files (*.log);;All Files (*)");
        if (!p.isEmpty()) d.setText(IDC_LOGFILE, p);
    });
    for (;;) {
        if (d.exec() != QDialog::Accepted) return false;
        const bool use = d.checked(IDC_LOGFILEDATE);
        QString name = d.text(IDC_LOGFILE);
        if (!use) {
            if (name.isEmpty()) { warn(&d, "PDW Logfile", "You haven't entered a file name!"); continue; }
            if (!name.contains('.')) name += ".log";
            if (name.compare(gets(dec, "FilterFile", "Filter"), Qt::CaseInsensitive) == 0) {
                warn(&d, "PDW Logfile", QString("Logfile can't be same as Filterfile!\n( %1 )").arg(name));
                continue;
            }
            const QString full = name.contains('/') ? name : gets(dec, "LogfilePath") + "/" + name;
            QDir().mkpath(QFileInfo(full).absolutePath());
            QFile f(full);
            if (!f.open(QIODevice::Append)) { warn(&d, "PDW Logfile", QString("Error opening logfile!\n( %1 )").arg(name)); continue; }
        }
        seti(dec, "LogFileEnabled", d.checked(IDC_LOGFILEEN));
        seti(dec, "LogFileUseDate", use);
        sets(dec, "LogFile", use ? QString() : name);
        sets(dec, "ColLogfile", logColumns(d, IDC_LOGCOLUMN));
        commit(w);
        return true;
    }
}
}  // namespace dlg
