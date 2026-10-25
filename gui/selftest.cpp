// `pdw --selftest DIR`: opens every dialog on the real main window (offscreen), takes a picture and
// presses OK. Settings must come back unchanged, which catches mistakes in the control<->setting
// mapping. Output: one line per dialog, exit status 1 on any difference.
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QDir>
#include <QTimer>
#include <cstdio>
#include "dialogs.hpp"
#include "generated/rc_ids.h"
#include "mainwindow.hpp"

namespace {
using Snapshot = std::vector<std::tuple<std::string, std::string, std::string>>;

QString diff(const Snapshot &a, const Snapshot &b)
{
    QString out;
    for (size_t i = 0; i < a.size() && i < b.size(); i++)
        if (a[i] != b[i])
            out += QString(" %1/%2: '%3' -> '%4'").arg(QString::fromStdString(std::get<0>(a[i])), QString::fromStdString(std::get<1>(a[i])),
                                                        QString::fromStdString(std::get<2>(a[i])), QString::fromStdString(std::get<2>(b[i])));
    return out;
}
}  // namespace

int selfTest(MainWindow &w, const QString &dir)
{
    QDir().mkpath(dir);
    struct Case { const char *name; std::function<void()> open; };
    const Case cases[] = {
        {"Options", [&] { dlg::options(&w); }},
        {"General", [&] { dlg::general(&w); }},
        {"Screen Options", [&] { dlg::screenOptions(&w); }},
        {"Scrollback", [&] { dlg::scrollback(&w); }},
        {"Logfile", [&] { dlg::logfile(&w); }},
        {"Filter Options", [&] { dlg::filterOptions(&w); }},
        {"Colors", [&] { dlg::colors(&w); }},
        {"SMTP", [&] { dlg::mail(&w); }},
        {"Statistics", [&] { dlg::statistics(&w); }},
        {"System tray", [&] { dlg::systemTray(&w); }},
        {"Filters", [&] { dlg::filters(&w); }},
    };
    int fails = 0;

    // Scripted use of the filter dialogs: add two filters, edit one, edit both at once.
    {
        auto &dec = w.decoder();
        auto active = [] { return QApplication::activeModalWidget(); };
        auto clickText = [](QWidget *dlg, const QString &text) {
            for (auto *b : dlg->findChildren<QPushButton *>())
                if (b->text() == text && b->isEnabled()) { b->click(); return true; }
            return false;
        };
        auto byId = [](QWidget *dlg, int id) -> QWidget * {
            for (auto *c : dlg->findChildren<QWidget *>())
                if (c->property("rcid").toInt() == id) return c;
            return nullptr;
        };
        auto step = [&](int ms, std::function<void(QWidget *)> fn) {
            QTimer::singleShot(ms, [=] { if (QWidget *m = QApplication::activeModalWidget()) fn(m); });
        };
        auto addFilter = [&](int t, const QString &cap, const QString &label) {
            step(t, [=](QWidget *m) { clickText(m, "Add"); });
            step(t + 150, [=](QWidget *m) {
                if (auto *e = qobject_cast<QLineEdit *>(byId(m, rc::IDC_FILTERCAPCODE))) e->setText(cap);
                if (auto *e = qobject_cast<QLineEdit *>(byId(m, rc::IDC_FILTERLABEL))) e->setText(label);
                if (auto *c = qobject_cast<QCheckBox *>(byId(m, rc::IDC_FILTERLABELEN))) c->setChecked(true);
                clickText(m, "OK");
            });
        };
        addFilter(100, "1111111", "First");
        addFilter(500, "2222222", "Second");
        // select the first, edit its label
        step(900, [&](QWidget *m) {
            if (auto *lw = m->findChild<QListWidget *>()) { lw->clearSelection(); lw->item(0)->setSelected(true); lw->setCurrentRow(0); }
            clickText(m, "Edit");
        });
        step(1050, [=](QWidget *m) {
            if (auto *e = qobject_cast<QLineEdit *>(byId(m, rc::IDC_FILTERLABEL))) e->setText("Renamed");
            clickText(m, "OK");
        });
        // edit both: set a text, leave the label alone
        step(1400, [&](QWidget *m) {
            if (auto *lw = m->findChild<QListWidget *>()) lw->selectAll();
            clickText(m, "Edit");
        });
        step(1550, [=](QWidget *m) {
            m->grab().save(QDir(dir).filePath("Filter_multi_edit.png"));
            if (auto *e = qobject_cast<QLineEdit *>(byId(m, rc::IDC_FILTERTEXT))) e->setText("FIRE");
            clickText(m, "OK");
        });
        step(1900, [&](QWidget *m) { clickText(m, "OK"); });
        dlg::filters(&w);
        const auto f = dec.filters();
        const bool ok = f.size() == 2 && f[0].capcode == "1111111" && f[0].label == "Renamed" && f[1].label == "Second" &&
                        f[0].text == "FIRE" && f[1].text == "FIRE" && f[0].label_enabled && f[1].label_enabled;
        printf("%-16s %s", "Filter add/edit", ok ? "ok\n" : "FAILED\n");
        for (const auto &x : f) printf("   %s | %s | %s | labelen=%d\n", x.capcode.c_str(), x.label.c_str(), x.text.c_str(), x.label_enabled);
        if (!ok) fails++;
    }

    for (const Case &c : cases) {
        const Snapshot before = w.decoder().all_settings();
        const auto filtersBefore = w.decoder().filters().size();
        QTimer::singleShot(150, [&] {
            QWidget *m = QApplication::activeModalWidget();
            if (!m) return;
            m->grab().save(QDir(dir).filePath(QString(c.name).replace(' ', '_') + ".png"));
            if (auto *q = qobject_cast<QDialog *>(m)) q->accept();
        });
        c.open();
        QString d = diff(before, w.decoder().all_settings());
        // saving the SMTP dialog the first time adds the charset bits (as in the original)
        if (QString(c.name) == "SMTP") d.replace(QRegularExpression(" SMTP/Options: '\\d+' -> '\\d+'"), "");
        const bool ok = d.isEmpty() && filtersBefore == w.decoder().filters().size();
        printf("%-16s %s%s\n", c.name, ok ? "ok" : "CHANGED", d.toUtf8().constData());
        if (!ok) fails++;
    }
    return fails ? 1 : 0;
}
