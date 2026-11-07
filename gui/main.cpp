// pdw: the desktop front end (Qt). Based on PDW by Jason Petty and Peter Hunt, GPL-3.0-or-later.
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTimer>
#include <cstdio>
#include "guisettings.hpp"
#include "mainwindow.hpp"

int dialogShots(const QString &dir);
int selfTest(MainWindow &w, const QString &dir);

static QString findResources(const QString &argv0)
{
    QStringList cand;
    if (const char *e = getenv("PDW_DATA_DIR")) cand << e;
    const QString exe = QCoreApplication::applicationDirPath();
    cand << exe + "/../share/pdw" << exe + "/../../data" << exe + "/../data" << exe + "/data" << "/usr/share/pdw" << "/usr/local/share/pdw";
    for (const QString &c : cand)
        if (QFile::exists(c + "/language.df")) return QDir(c).absolutePath();
    Q_UNUSED(argv0);
    return {};
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("pdw");
    QApplication::setOrganizationName("pdw-linux");
    QApplication::setApplicationVersion("0.2");

    QCommandLineParser p;
    p.setApplicationDescription("PDW pager decoder (POCSAG, FLEX, ACARS, Mobitex): independent Linux/macOS port");
    p.addHelpOption();
    p.addVersionOption();
    p.addOption({"dir", "PDW folder with pdw.ini, filters.ini, Logfiles/ ... (default: user data folder)", "dir"});
    p.addOption({"play", "play this WAV file", "file"});
    p.addOption({"fast", "play the file as fast as possible"});
    p.addOption({"stdin", "read raw s16le mono audio from stdin (rtl_fm ... | pdw --stdin --rate 22050)"});
    p.addOption({"rate", "sample rate of --stdin (default 22050)", "hz", "22050"});
    p.addOption({"rtl", "tune an RTL-SDR dongle with rtl_fm to this frequency (MHz)", "mhz"});
    p.addOption({"dialog-shots", "write a PNG of every dialog into this folder and exit", "dir"});
    p.addOption({"selftest", "open every dialog, save pictures into this folder, check that OK changes nothing", "dir"});
    p.addOption({"rtl-program", "program to run instead of rtl_fm (for tests)", "path"});
    p.addOption({"screenshot", "when the input ends, save the window as PNG and exit", "png"});
    p.addOption({"dump-panes", "when the input ends, write the text of both panes to this file and exit", "file"});
    p.process(app);

    if (p.isSet("dialog-shots")) {
        printf("%d dialogs\n", dialogShots(p.value("dialog-shots")));
        return 0;
    }

    pdw::Config cfg;
    QString dir = p.value("dir");
    if (dir.isEmpty()) dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/pdw";
    QDir().mkpath(dir);
    cfg.dir = dir.toStdString();
    cfg.resource_dir = findResources(argv[0]).toStdString();
    guiSettings(dir);

    MainWindow w(cfg);
    w.show();
    if (p.isSet("selftest")) return selfTest(w, p.value("selftest"));
    if (p.isSet("screenshot") || p.isSet("dump-panes")) {
        // when the input ends: give the panes a moment to paint, then save what was asked for and quit
        QObject::connect(&w, &MainWindow::inputFinished, [&] {
            QTimer::singleShot(300, [&] {
                if (p.isSet("screenshot")) w.grab().save(p.value("screenshot"));
                if (p.isSet("dump-panes")) {
                    QFile f(p.value("dump-panes"));
                    if (f.open(QIODevice::WriteOnly))
                        f.write((w.monitorPane()->allText() + "\n----\n" + w.filterPane()->allText() + "\n").toUtf8());
                }
                w.saveForExit();
                QCoreApplication::exit(0);
            });
        });
    }
    if (p.isSet("play")) {
        w.playFile(p.value("play"), !p.isSet("fast"));
    } else if (p.isSet("stdin")) {
        w.useStdin(p.value("rate").toDouble());
    } else if (p.isSet("rtl")) {
        RtlFmSource::Params rp;
        auto &gs = guiSettings();
        rp.mhz = p.value("rtl").toDouble();
        rp.gain = gs.value("rtl/gain", 40).toInt();
        rp.ppm = gs.value("rtl/ppm", 0).toInt();
        rp.squelch = gs.value("rtl/squelch", 0).toInt();
        rp.rate = gs.value("rtl/rate", 22050).toInt();
        rp.device = gs.value("rtl/device", 0).toInt();
        rp.program = gs.value("rtl/program", "rtl_fm").toString();
        rp.extra = gs.value("rtl/extra", "").toString();
        rp.mode = gs.value("rtl/mode", "fm").toString();
        if (p.isSet("rtl-program")) rp.program = p.value("rtl-program");
        w.useRtlFm(rp);
    } else {
        // resume what was used last time
        auto &gs = guiSettings();
        const QString kind = gs.value("input/kind", "none").toString();
        if (kind == "rtl" && gs.value("rtl/mhz").toDouble() > 0) {
            RtlFmSource::Params rp;
            rp.mhz = gs.value("rtl/mhz").toDouble();
            rp.mode = gs.value("rtl/mode", "fm").toString();
            rp.gain = gs.value("rtl/gain", 40).toInt();
            rp.ppm = gs.value("rtl/ppm", 0).toInt();
            rp.squelch = gs.value("rtl/squelch", 0).toInt();
            rp.rate = gs.value("rtl/rate", 22050).toInt();
            rp.device = gs.value("rtl/device", 0).toInt();
            rp.program = gs.value("rtl/program", "rtl_fm").toString();
            rp.extra = gs.value("rtl/extra", "").toString();
            w.useRtlFm(rp);
        } else if (kind == "device") {
            w.useDevice(gs.value("device/index", 0).toInt(), gs.value("device/rate", 44100).toInt());
        }
    }
    return app.exec();
}
