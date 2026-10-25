#include <QApplication>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QListWidget>
#include <QSpinBox>
#include <QTimer>
#include <QColorDialog>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QMediaDevices>
#include <QMessageBox>
#include <QPainter>
#include <QSysInfo>
#include <QUrl>
#include "dialogs.hpp"
#include "generated/rc_ids.h"
#include "guisettings.hpp"
#include "mainwindow.hpp"
#include "rcdialog.hpp"

using namespace rc;

namespace {
int geti(pdw::Decoder &d, const char *k, const char *sec = "PDW") { return (int)d.get_int(k, sec); }
void seti(pdw::Decoder &d, const char *k, long long v, const char *sec = "PDW") { d.set_int(k, v, sec); }
QString gets(pdw::Decoder &d, const char *k, const char *sec = "PDW")
{
    std::string v;
    d.get_setting(k, v, sec);
    return QString::fromStdString(v);
}
void sets(pdw::Decoder &d, const char *k, const QString &v, const char *sec = "PDW") { d.set_setting(k, v.toStdString(), sec); }
void commit(MainWindow *w)
{
    w->settingsChanged();
    if (!w->dataDir().isEmpty()) w->decoder().save_settings();
}
void warn(QWidget *p, const QString &t, const QString &m) { QMessageBox::critical(p, t, m); }
}  // namespace

namespace dlg {

// ---- Interface / custom audio levels -----------------------------------------------------------------------------
static bool customAudio(MainWindow *w, QWidget *parent)
{
    auto &dec = w->decoder();
    RcDialog d("CUSTOM_AUDIO_DLG", parent);
    struct Row { const char *key; int id; };
    const Row rows[] = {{"Threshold512", IDC_THRESHOLD512}, {"Threshold1200", IDC_THRESHOLD1200}, {"Threshold1600", IDC_THRESHOLD1600},
                        {"Threshold2400", IDC_THRESHOLD2400}, {"Resync512", IDC_RESYNC512}, {"Resync1200", IDC_RESYNC1200},
                        {"Resync1600", IDC_RESYNC1600}, {"Resync2400", IDC_RESYNC2400}, {"Centering512", IDC_CENTERING512},
                        {"Centering1200", IDC_CENTERING1200}, {"Centering1600", IDC_CENTERING1600}, {"Centering2400", IDC_CENTERING2400}};
    QStringList digits;
    for (int i = 0; i < 10; i++) digits << QString::number(i);
    for (const Row &r : rows) {
        d.fillCombo(r.id, digits);
        d.setComboIndex(r.id, std::max(0, std::min(9, geti(dec, r.key))));
    }
    if (d.exec() != QDialog::Accepted) return false;
    for (const Row &r : rows) seti(dec, r.key, d.comboIndex(r.id));
    seti(dec, "AudioConfiguration", 0);
    dec.apply_settings();
    return true;
}

// ---- Interface / Setup ------------------------------------------------------------------------------------------------
// The original selects a serial interface or the sound card. Here the input is a sound device, an
// rtl_fm child process (RTL-SDR dongle) or a file; the audio level presets are PDW's.
bool setup(MainWindow *w)
{
    auto &dec = w->decoder();
    auto &gs = guiSettings(w->dataDir());
    QDialog d(w);
    d.setWindowTitle("PDW Interface Setup");
    d.setFont(RcDialog::dialogFont("Verdana", 8));
    auto *grid = new QGridLayout(&d);

    auto *rtl = new QRadioButton("RTL-SDR dongle (rtl_fm)");
    auto *dev = new QRadioButton("Sound device");
    auto *none = new QRadioButton("None (files and pipes only)");
    const QString cur = gs.value("input/kind", "none").toString();
    (cur == "rtl" ? rtl : cur == "device" ? dev : none)->setChecked(true);

    auto *freq = new QLineEdit(gs.value("rtl/mhz", "").toString());
    freq->setPlaceholderText("e.g. 152.400");
    auto *rmode = new QComboBox;
    rmode->addItems({"fm", "am", "wbfm"});
    rmode->setCurrentText(gs.value("rtl/mode", "fm").toString());
    auto *gain = new QSpinBox; gain->setRange(0, 60); gain->setSpecialValueText("auto"); gain->setValue(gs.value("rtl/gain", 40).toInt());
    auto *ppm = new QSpinBox; ppm->setRange(-300, 300); ppm->setValue(gs.value("rtl/ppm", 0).toInt());
    auto *sq = new QSpinBox; sq->setRange(0, 500); sq->setValue(gs.value("rtl/squelch", 0).toInt());
    auto *rate = new QComboBox; rate->addItems({"22050", "24000", "44100", "48000"}); rate->setCurrentText(gs.value("rtl/rate", "22050").toString());
    auto *devno = new QSpinBox; devno->setRange(0, 9); devno->setValue(gs.value("rtl/device", 0).toInt());
    auto *prog = new QLineEdit(gs.value("rtl/program", "rtl_fm").toString());
    auto *extra = new QLineEdit(gs.value("rtl/extra", "").toString());

    auto *devs = new QComboBox;
    devs->addItems(DeviceSource::deviceNames());
    if (devs->count() == 0) devs->addItem("No sound devices present/available");
    devs->setCurrentIndex(std::min(devs->count() - 1, gs.value("device/index", 0).toInt()));
    auto *drate = new QComboBox; drate->addItems({"8000", "11025", "22050", "44100", "48000"}); drate->setCurrentText(gs.value("device/rate", "44100").toString());

    auto *config = new QComboBox;
    config->addItems({"Custom", "Discriminator 1", "Discriminator 2", "Discriminator 3", "Discriminator 4", "Earphone 1", "Earphone 2", "Earphone 3",
                      "Speaker Out 1", "Speaker Out 2", "Speaker Out 3", "Tape/Rec Out 1", "Tape/Rec Out 2", "Tape/Rec Out 3"});
    config->setCurrentIndex(geti(dec, "AudioConfiguration"));
    auto *custom = new QPushButton("Set-Custom");
    custom->setEnabled(config->currentIndex() == 0);
    QObject::connect(config, &QComboBox::currentIndexChanged, &d, [custom](int i) { custom->setEnabled(i == 0); });
    QObject::connect(custom, &QPushButton::clicked, &d, [&] { customAudio(w, &d); config->setCurrentIndex(0); });
    auto *invert = new QCheckBox("Invert data (opposite polarity)");
    invert->setChecked(geti(dec, "InvertData"));

    int r = 0;
    auto *gIn = new QGroupBox("Input");
    auto *fl = new QGridLayout(gIn);
    int y = 0;
    fl->addWidget(rtl, y++, 0, 1, 4);
    fl->addWidget(new QLabel("Frequency (MHz)"), y, 0); fl->addWidget(freq, y, 1);
    fl->addWidget(new QLabel("Mode"), y, 2); fl->addWidget(rmode, y++, 3);
    fl->addWidget(new QLabel("Gain (dB)"), y, 0); fl->addWidget(gain, y, 1);
    fl->addWidget(new QLabel("PPM"), y, 2); fl->addWidget(ppm, y++, 3);
    fl->addWidget(new QLabel("Squelch"), y, 0); fl->addWidget(sq, y, 1);
    fl->addWidget(new QLabel("Sample rate"), y, 2); fl->addWidget(rate, y++, 3);
    fl->addWidget(new QLabel("Dongle #"), y, 0); fl->addWidget(devno, y, 1);
    fl->addWidget(new QLabel("Program"), y, 2); fl->addWidget(prog, y++, 3);
    fl->addWidget(new QLabel("Extra rtl_fm options"), y, 0); fl->addWidget(extra, y++, 1, 1, 3);
    fl->addWidget(dev, y++, 0, 1, 4);
    fl->addWidget(new QLabel("Device"), y, 0); fl->addWidget(devs, y, 1, 1, 2);
    fl->addWidget(drate, y++, 3);
    fl->addWidget(none, y++, 0, 1, 4);
    grid->addWidget(gIn, r++, 0, 1, 2);

    auto *gCfg = new QGroupBox("Audio level");
    auto *cl = new QGridLayout(gCfg);
    cl->addWidget(new QLabel("Configuration"), 0, 0); cl->addWidget(config, 0, 1); cl->addWidget(custom, 0, 2);
    cl->addWidget(invert, 1, 0, 1, 3);
    grid->addWidget(gCfg, r++, 0, 1, 2);

    auto sync = [&] {
        const bool isRtl = rtl->isChecked(), isDev = dev->isChecked();
        for (QWidget *x : std::initializer_list<QWidget *>{freq, rmode, gain, ppm, sq, rate, devno, prog, extra}) x->setEnabled(isRtl);
        for (QWidget *x : std::initializer_list<QWidget *>{devs, drate}) x->setEnabled(isDev);
    };
    sync();
    for (QRadioButton *b : {rtl, dev, none}) QObject::connect(b, &QRadioButton::toggled, &d, sync);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    grid->addWidget(bb, r++, 0, 1, 2);

    const int oldConfig = geti(dec, "AudioConfiguration");
    for (;;) {
        if (d.exec() != QDialog::Accepted) {
            seti(dec, "AudioConfiguration", oldConfig);
            dec.apply_settings();
            return false;
        }
        if (rtl->isChecked() && freq->text().toDouble() <= 0) {
            warn(&d, "PDW Setup", "Enter the frequency to tune the RTL-SDR dongle to (in MHz).");
            continue;
        }
        break;
    }
    seti(dec, "AudioConfiguration", config->currentIndex());
    seti(dec, "InvertData", invert->isChecked());
    gs.setValue("input/kind", rtl->isChecked() ? "rtl" : dev->isChecked() ? "device" : "none");
    gs.setValue("rtl/mhz", freq->text());
    gs.setValue("rtl/mode", rmode->currentText());
    gs.setValue("rtl/gain", gain->value());
    gs.setValue("rtl/ppm", ppm->value());
    gs.setValue("rtl/squelch", sq->value());
    gs.setValue("rtl/rate", rate->currentText());
    gs.setValue("rtl/device", devno->value());
    gs.setValue("rtl/program", prog->text());
    gs.setValue("rtl/extra", extra->text());
    gs.setValue("device/index", devs->currentIndex());
    gs.setValue("device/rate", drate->currentText());
    gs.sync();
    commit(w);
    if (rtl->isChecked()) {
        RtlFmSource::Params p;
        p.mhz = freq->text().toDouble();
        p.mode = rmode->currentText();
        p.rate = rate->currentText().toInt();
        p.gain = gain->value();
        p.ppm = ppm->value();
        p.squelch = sq->value();
        p.device = devno->value();
        p.program = prog->text().isEmpty() ? "rtl_fm" : prog->text();
        p.extra = extra->text();
        w->useRtlFm(p);
    } else if (dev->isChecked()) {
        w->useDevice(devs->currentIndex(), drate->currentText().toInt());
    }
    return true;
}

// ---- Options / Options ---------------------------------------------------------------------------------------------------------
bool options(MainWindow *w)
{
    auto &dec = w->decoder();
    RcDialog d("OPTIONSDLGBOX", w);
    const pdw::Mode mode = dec.mode();
    const char *names[] = {"Paging (Pocsag/Flex)", "ACARS", "Mobitex", "ERMES"};
    d.setWindowTitle(QString("PDW Options - Currently monitoring : ") + names[int(mode)]);

    d.setChecked(IDC_DECODEPOCSAG, geti(dec, "DecodePocsag"));
    d.setChecked(IDC_POCSAG_512, geti(dec, "Pocsag512"));
    d.setChecked(IDC_POCSAG_1200, geti(dec, "Pocsag1200"));
    d.setChecked(IDC_POCSAG_2400, geti(dec, "Pocsag2400"));
    d.setChecked(IDC_POCSAG_FNU, geti(dec, "PocsagFNU"));
    d.setChecked(IDC_POCSAG_BOTH, geti(dec, "PocsagShowBoth"));
    d.setChecked(IDC_DECODEFLEX, geti(dec, "DecodeFlex"));
    d.setChecked(IDC_FLEX_1600, geti(dec, "Flex1600"));
    d.setChecked(IDC_FLEX_3200, geti(dec, "Flex3200"));
    d.setChecked(IDC_FLEX_6400, geti(dec, "Flex6400"));
    d.setChecked(IDC_SHOWINSTR, geti(dec, "ShowInstr"));
    d.setChecked(IDC_CONVERT_SI, geti(dec, "ConvertInstr"));
    d.setChecked(IDC_FLEXTIME, geti(dec, "FlexTIME"));
    d.setChecked(IDC_SHOW_CFS, geti(dec, "ShowCFS"));
    d.setChecked(IDC_SHOW_REJECTBLOCKED, geti(dec, "ShowRejectBlocked"));
    d.setChecked(geti(dec, "AcarsParityCheck") ? IDC_ACARS_PC_YES : IDC_ACARS_PC_NO, true);

    // Mobitex
    d.get<QLineEdit>(IDC_MB_FRSYNC)->setMaxLength(4);
    d.get<QLineEdit>(IDC_MB_MIN_MSG)->setMaxLength(2);
    d.setText(IDC_MB_FRSYNC, QString("%1").arg(geti(dec, "FRSYNC"), 4, 16, QChar('0')).toUpper());
    d.setChecked(IDC_MB_USEFRSYNC, geti(dec, "CFS"));
    d.fillCombo(IDC_MB_BITSYNC, {"Base", "Mobile"});
    d.setComboIndex(IDC_MB_BITSYNC, geti(dec, "BTSYNC") == 0xCCCC ? 0 : 1);
    d.setText(IDC_MB_MIN_MSG, QString::number(geti(dec, "MINMSG")));
    d.fillCombo(IDC_MB_BITRATE, {"1200", "8000"});
    d.setComboIndex(IDC_MB_BITRATE, 1);
    d.setChecked(IDC_MB_BITSCRAMBLER, geti(dec, "BITSCR"));
    d.setChecked(IDC_MB_RAMNET, geti(dec, "RAMNET"));
    const int show = geti(dec, "SHOW");
    const struct { int id, bit; } mbShow[] = {{IDC_MB_SHOWMPAK, 0x01}, {IDC_MB_SHOWTEXT, 0x02}, {IDC_MB_SHOWHPDATA, 0x04}, {IDC_MB_SHOWDATA, 0x08},
                                              {IDC_MB_VERBOSE, 0x10}, {IDC_MB_SHOWHPID, 0x20}, {IDC_MB_SHOWSWEEP, 0x40}};
    for (auto &m : mbShow) d.setChecked(m.id, show & m.bit);

    // enable rules of the original, depending on the monitored mode
    auto syncPocsag = [&] {
        const bool on = d.checked(IDC_DECODEPOCSAG), both = d.checked(IDC_POCSAG_BOTH), fnu = d.checked(IDC_POCSAG_FNU);
        for (int id : {IDC_POCSAG_512, IDC_POCSAG_1200, IDC_POCSAG_2400}) d.setEnabled(id, on);
        d.setEnabled(IDC_POCSAG_FNU, on && !both);
        d.setEnabled(IDC_POCSAG_BOTH, on && !fnu);
    };
    auto syncFlex = [&] {
        const bool on = d.checked(IDC_DECODEFLEX);
        for (int id : {IDC_FLEX_1600, IDC_FLEX_3200, IDC_FLEX_6400, IDC_SHOWINSTR, IDC_SHOW_CFS}) d.setEnabled(id, on);
        d.setEnabled(IDC_CONVERT_SI, on && d.checked(IDC_SHOWINSTR));
        d.setEnabled(IDC_FLEXTIME, on && dec.flextime_detected());
    };
    if (mode == pdw::Mode::Paging) {
        d.setEnabled(IDC_DECODEPOCSAG, true);
        d.setEnabled(IDC_DECODEFLEX, true);
        syncPocsag();
        syncFlex();
        d.setText(IDC_FLEXTIME, dec.flextime_detected() ? "Use FlexTIME as Systemtime" : "Use FlexTIME (not detected)");
        QObject::connect(d.get<QCheckBox>(IDC_DECODEPOCSAG), &QCheckBox::toggled, &d, syncPocsag);
        QObject::connect(d.get<QCheckBox>(IDC_POCSAG_BOTH), &QCheckBox::toggled, &d, syncPocsag);
        QObject::connect(d.get<QCheckBox>(IDC_POCSAG_FNU), &QCheckBox::toggled, &d, syncPocsag);
        QObject::connect(d.get<QCheckBox>(IDC_DECODEFLEX), &QCheckBox::toggled, &d, syncFlex);
        QObject::connect(d.get<QCheckBox>(IDC_SHOWINSTR), &QCheckBox::toggled, &d, [&](bool on) {
            d.setEnabled(IDC_CONVERT_SI, on && d.checked(IDC_DECODEFLEX));
            if (!on) d.setChecked(IDC_CONVERT_SI, false);
        });
    } else if (mode == pdw::Mode::Mobitex) {
        for (int id : {IDC_MB_USEFRSYNC, IDC_MB_BITSYNC, IDC_MB_MIN_MSG, IDC_MB_BITSCRAMBLER, IDC_MB_RAMNET, IDC_MB_SHOWMPAK, IDC_MB_SHOWTEXT,
                       IDC_MB_SHOWDATA, IDC_MB_SHOWHPDATA, IDC_MB_SHOWSWEEP, IDC_MB_VERBOSE})
            d.setEnabled(id, true);
        d.setEnabled(IDC_MB_SHOWHPID, d.checked(IDC_MB_SHOWHPDATA));
        d.setEnabled(IDC_MB_FRSYNC, d.checked(IDC_MB_USEFRSYNC));
        QObject::connect(d.get<QCheckBox>(IDC_MB_SHOWHPDATA), &QCheckBox::toggled, &d, [&](bool on) { d.setEnabled(IDC_MB_SHOWHPID, on); });
        QObject::connect(d.get<QCheckBox>(IDC_MB_USEFRSYNC), &QCheckBox::toggled, &d, [&](bool on) { d.setEnabled(IDC_MB_FRSYNC, on); });
    } else if (mode == pdw::Mode::Acars) {
        d.setEnabled(IDC_ACARS_PC_YES, true);
        d.setEnabled(IDC_ACARS_PC_NO, true);
    } else {
        d.setEnabled(IDC_SHOW_CFS, true);
    }
    QObject::connect(d.button(IDC_GENERALOPTIONS), &QPushButton::clicked, &d, [&] { dlg::general(w); });

    if (d.exec() != QDialog::Accepted) return false;
    seti(dec, "DecodePocsag", d.checked(IDC_DECODEPOCSAG));
    seti(dec, "Pocsag512", d.checked(IDC_POCSAG_512));
    seti(dec, "Pocsag1200", d.checked(IDC_POCSAG_1200));
    seti(dec, "Pocsag2400", d.checked(IDC_POCSAG_2400));
    seti(dec, "PocsagShowBoth", d.checked(IDC_POCSAG_BOTH));
    seti(dec, "PocsagFNU", d.checked(IDC_POCSAG_FNU));
    seti(dec, "DecodeFlex", d.checked(IDC_DECODEFLEX));
    seti(dec, "Flex1600", d.checked(IDC_FLEX_1600));
    seti(dec, "Flex3200", d.checked(IDC_FLEX_3200));
    seti(dec, "Flex6400", d.checked(IDC_FLEX_6400));
    seti(dec, "ShowInstr", d.checked(IDC_SHOWINSTR));
    seti(dec, "ConvertInstr", d.checked(IDC_CONVERT_SI));
    seti(dec, "FlexTIME", d.checked(IDC_FLEXTIME));
    seti(dec, "ShowCFS", d.checked(IDC_SHOW_CFS));
    seti(dec, "ShowRejectBlocked", d.checked(IDC_SHOW_REJECTBLOCKED));
    seti(dec, "AcarsParityCheck", d.checked(IDC_ACARS_PC_YES));
    seti(dec, "CFS", d.checked(IDC_MB_USEFRSYNC));
    seti(dec, "BITSCR", d.checked(IDC_MB_BITSCRAMBLER));
    seti(dec, "RAMNET", d.checked(IDC_MB_RAMNET));
    int sh = 0;
    for (auto &m : mbShow) if (d.checked(m.id)) sh |= m.bit;
    seti(dec, "SHOW", sh);
    seti(dec, "BTSYNC", d.comboIndex(IDC_MB_BITSYNC) > 0 ? 0x3333 : 0xCCCC);
    bool ok = false;
    const int fs = d.text(IDC_MB_FRSYNC).toInt(&ok, 16);
    if (ok && d.text(IDC_MB_FRSYNC).size() > 1) seti(dec, "FRSYNC", fs);
    if (!d.text(IDC_MB_MIN_MSG).isEmpty()) seti(dec, "MINMSG", d.text(IDC_MB_MIN_MSG).toInt());
    if ((!geti(dec, "DecodeFlex") || !geti(dec, "DecodePocsag")) && mode != pdw::Mode::Acars) dec.reset_audio();
    commit(w);
    return true;
}

// ---- Display / Colors ------------------------------------------------------------------------------------------------------------
namespace {
struct ColorBtn { int id; const char *key; };

class Preview : public QWidget {
public:
    Preview(QWidget *p, const QVector<QPair<QString, QColor>> &lines, QColor bg) : QWidget(p), lines_(lines), bg_(bg) {}
    void set(const QVector<QPair<QString, QColor>> &l, QColor bg) { lines_ = l; bg_ = bg; update(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), bg_);
        QFont f("Monospace");
        f.setStyleHint(QFont::TypeWriter);
        f.setPixelSize(13);
        p.setFont(f);
        int y = 14;
        for (auto &l : lines_) { p.setPen(l.second); p.drawText(6, y, l.first); y += 14; }
    }

private:
    QVector<QPair<QString, QColor>> lines_;
    QColor bg_;
};
}  // namespace

bool colors(MainWindow *w)
{
    auto &dec = w->decoder();
    const pdw::Mode mode = dec.mode();
    const char *dlgName = mode == pdw::Mode::Acars ? "ACARS_COLORSDLGBOX" : mode == pdw::Mode::Mobitex ? "MOBITEX_COLORSDLGBOX"
                          : mode == pdw::Mode::Ermes ? "ERMES_COLORSDLGBOX" : "COLORSDLGBOX";
    RcDialog d(dlgName, w);
    QVector<ColorBtn> btns;
    int winId = IDC_COLORWIN, defId = IDC_COLORDEFAULT;
    switch (mode) {
        case pdw::Mode::Acars:
            winId = IDC_AC_COLORWIN; defId = IDC_AC_COLORDEFAULT;
            btns = {{IDC_AC_COLORMSGNO, "Color.ACSEQNO"}, {IDC_AC_COLORTIMESTAMP, "Color.Timestamp"}, {IDC_AC_COLORDBI, "Color.ACDBI"},
                    {IDC_AC_COLORMODE, "Color.FLEXPhase"}, {IDC_AC_COLORTAGS, "Color.FLEXBinary"}, {IDC_AC_COLORMSG, "Color.Alphanumeric"},
                    {IDC_AC_COLORBACKGND, "Color.Background"}, {IDC_AC_COLORFILTMATCH, "Color.FilterMatch"},
                    {IDC_AC_COLORBITERRORS, "Color.BitErrors"}, {IDC_AC_COLORFILTERLABEL, "Color.FilterLabel"}};
            break;
        case pdw::Mode::Mobitex:
            winId = IDC_MB_COLORWIN; defId = IDC_MB_COLORDEFAULT;
            btns = {{IDC_MB_COLORMAN, "Color.Capcode"}, {IDC_MB_COLORTIMESTAMP, "Color.Timestamp"}, {IDC_MB_COLORTYPE, "Color.FLEXPhase"},
                    {IDC_MB_COLORSENDER, "Color.MbSender"}, {IDC_MB_COLORMESSAGE, "Color.Alphanumeric"}, {IDC_MB_COLORMISC, "Color.FLEXBinary"},
                    {IDC_MB_COLORBACKGND, "Color.Background"}, {IDC_MB_COLORFILTMATCH, "Color.FilterMatch"},
                    {IDC_MB_COLORBITERRORS, "Color.BitErrors"}, {IDC_MB_COLORFILTERLABEL, "Color.FilterLabel"}};
            break;
        case pdw::Mode::Ermes:
            winId = IDC_EM_COLORWIN; defId = IDC_EM_COLORDEFAULT;
            btns = {{IDC_EM_COLORCAPCODE, "Color.Capcode"}, {IDC_EM_COLORTIMESTAMP, "Color.Timestamp"}, {IDC_EM_COLORFUNCTION, "Color.FLEXPhase"},
                    {IDC_EM_COLORBITERRORS, "Color.BitErrors"}, {IDC_EM_COLORNUMERIC, "Color.Numeric"}, {IDC_EM_COLORALPHANUM, "Color.Alphanumeric"},
                    {IDC_EM_COLORBACKGND, "Color.Background"}, {IDC_EM_COLORFILTMATCH, "Color.FilterMatch"},
                    {IDC_EM_COLORTRANSPARENT, "Color.FLEXBinary"}, {IDC_EM_COLORFILTERLABEL, "Color.FilterLabel"}};
            break;
        default:
            btns = {{IDC_COLORCAPCODE, "Color.Capcode"}, {IDC_COLORTIMESTAMP, "Color.Timestamp"}, {IDC_COLORFLEXPHASE, "Color.FLEXPhase"},
                    {IDC_COLORBITERRORS, "Color.BitErrors"}, {IDC_COLORNUMERIC, "Color.Numeric"}, {IDC_COLORALPHANUM, "Color.Alphanumeric"},
                    {IDC_COLORBACKGND, "Color.Background"}, {IDC_COLORFILTMATCH, "Color.FilterMatch"}, {IDC_COLORFLEXBIN, "Color.FLEXBinary"},
                    {IDC_COLORFILTERLABEL, "Color.FilterLabel"}};
    }
    QMap<QString, QColor> tmp;
    auto parse = [](const QString &s) { const auto p = s.split(','); return QColor(p[0].toInt(), p[1].toInt(), p[2].toInt()); };
    for (const ColorBtn &b : btns) tmp[b.key] = parse(gets(dec, b.key));
    QWidget *placeholder = d.ctl(winId);
    auto *prev = new Preview(&d, {}, Qt::black);
    prev->setGeometry(placeholder->geometry());
    placeholder->hide();
    auto refresh = [&] {
        QVector<QPair<QString, QColor>> lines;
        const QColor addr = tmp.value("Color.Capcode", Qt::white), ts = tmp.value("Color.Timestamp"), ph = tmp.value("Color.FLEXPhase");
        const QColor msg = tmp.value("Color.Alphanumeric"), num = tmp.value("Color.Numeric", Qt::red), err = tmp.value("Color.BitErrors");
        lines << qMakePair(QString("1234567  12:34:56 25-10-26  FLEX-A  ALPHA  1600  Sample alphanumeric message"), msg);
        lines << qMakePair(QString("7654321  12:34:57 25-10-26  FLEX-A  StNUM  1600  0845-123456"), num);
        lines << qMakePair(QString("bit errors in a message"), err);
        lines << qMakePair(QString("filter match"), tmp.value("Color.FilterMatch"));
        lines << qMakePair(QString("filter label"), tmp.value("Color.FilterLabel"));
        lines << qMakePair(QString("address / time / mode / binary: %1").arg(addr.name()), addr);
        lines << qMakePair(QString("time stamp"), ts);
        lines << qMakePair(QString("phase / function / type"), ph);
        prev->set(lines, tmp.value("Color.Background", Qt::black));
    };
    refresh();
    for (const ColorBtn &b : btns) {
        QObject::connect(d.button(b.id), &QPushButton::clicked, &d, [&, key = QString(b.key)] {
            const QColor c = QColorDialog::getColor(tmp[key], &d, "Select color");
            if (c.isValid()) { tmp[key] = c; refresh(); }
        });
    }
    QObject::connect(d.button(defId), &QPushButton::clicked, &d, [&] {
        if (QMessageBox::question(&d, "PDW Reset colors", "Reset all colors to Default?") != QMessageBox::Yes) return;
        const QMap<QString, QColor> def = {{"Color.Background", QColor(0, 0, 0)}, {"Color.Capcode", QColor(255, 255, 255)}, {"Color.FLEXPhase", QColor(255, 0, 0)},
                                           {"Color.Timestamp", QColor(0, 0, 255)}, {"Color.Numeric", QColor(255, 0, 0)}, {"Color.Alphanumeric", QColor(0, 255, 255)},
                                           {"Color.FLEXBinary", QColor(0, 255, 255)}, {"Color.BitErrors", QColor(192, 192, 192)}, {"Color.FilterMatch", QColor(0, 255, 0)},
                                           {"Color.FilterLabel", QColor(0, 0, 255)}, {"Color.ACSEQNO", QColor(255, 255, 255)}, {"Color.ACDBI", QColor(255, 0, 0)},
                                           {"Color.MbSender", QColor(255, 0, 0)}};
        for (auto it = tmp.begin(); it != tmp.end(); ++it) it.value() = def.value(it.key(), it.value());
        refresh();
    });
    if (d.exec() != QDialog::Accepted) return false;
    for (auto it = tmp.begin(); it != tmp.end(); ++it)
        sets(dec, it.key().toUtf8().constData(), QString("%1,%2,%3").arg(it.value().red()).arg(it.value().green()).arg(it.value().blue()));
    commit(w);
    return true;
}

// ---- Options / SMTP ---------------------------------------------------------------------------------------------------------------------
bool mail(MainWindow *w)
{
    auto &dec = w->decoder();
    RcDialog d("MAIL_DLGBOX", w);
    const int opts = geti(dec, "Options", "SMTP");
    d.setChecked(IDC_SMTP, geti(dec, "Enable", "SMTP"));
    d.fillCombo(IDC_SMTP_SETTING, {"All messages", "Filtered messages", "Filtered & Monitor-Only messages", "Selected filters only"});
    d.fillCombo(IDC_SMTP_SENDIN, {"Subject", "Message", "Subject & Message"});
    d.fillCombo(IDC_SMTP_CHARSET, {"us-ascii     (Standard)", "iso-8859-1   (West European)", "iso-8859-2   (East European)", "iso-8859-3   (South European)",
                                   "iso-8859-4   (North European)", "iso-8859-5   (Cyrillic)", "iso-8859-6   (Arabic)", "iso-8859-7   (Greek)",
                                   "iso-8859-8   (Hebrew)", "iso-8859-9   (Turkish)", "iso-8859-10  (Nordic)", "iso-2022-kr  (Korean)",
                                   "KOI8-R       (Russian)", "EUC-KR       (Korean)", "Shift_JIS    (Japanese)", "ISO-2022-JP  (Japanese)",
                                   "EUC-JP       (Japanese)", "GB2312       (Chinese)", "Big5         (Traditional Chinese)",
                                   "windows-1250 (Central Europ Windows)", "windows-1251 (Cyrillic Windows)", "windows-1252 (Western Europ Windows)",
                                   "windows-1253 (Greek Windows)", "windows-1254 (Turkish (Windows)", "windows-1255 (Hebrew Windows)",
                                   "windows-1256 (Arabic Windows)", "windows-1257 (Baltic Windows)", "windows-1258 (Vietnamese Windows)"});
    d.setText(IDC_SMTP_HOST, gets(dec, "Host", "SMTP"));
    d.setText(IDC_SMTP_HELO, gets(dec, "HeloDomain", "SMTP"));
    d.setText(IDC_SMTP_TO, gets(dec, "To", "SMTP"));
    d.setText(IDC_SMTP_FROM, gets(dec, "From", "SMTP"));
    d.setText(IDC_SMTP_USER, gets(dec, "User", "SMTP"));
    d.setText(IDC_SMTP_PASSWORD, gets(dec, "Password", "SMTP"));
    if (auto *pw = d.get<QLineEdit>(IDC_SMTP_PASSWORD)) pw->setEchoMode(QLineEdit::Password);
    d.setText(IDC_SMTP_PORT, QString::number(geti(dec, "Port", "SMTP")));
    const struct { int id, bit; } checks[] = {{IDC_SMTP_ADDRESS, 0x10}, {IDC_SMTP_TIME, 0x20}, {IDC_SMTP_DATE, 0x40}, {IDC_SMTP_MODE, 0x80}, {IDC_SMTP_TYPE, 0x100},
                                              {IDC_SMTP_BITRATE, 0x200}, {IDC_SMTP_MESSAGE, 0x400}, {IDC_SMTP_LABEL, 0x800}, {IDC_SMTP_AUTH, 0x4000},
                                              {IDC_SMTP_SSL, 0x200000}};
    for (auto &c : checks) d.setChecked(c.id, opts & c.bit);
    d.setComboIndex(IDC_SMTP_SETTING, opts & 0xF);
    d.setComboIndex(IDC_SMTP_SENDIN, std::max(0, ((opts & 0x3000) >> 12) - 1));
    d.setComboIndex(IDC_SMTP_CHARSET, std::max(0, ((opts & 0x1F0000) >> 16) - 1) ? ((opts & 0x1F0000) >> 16) - 1 : 1);
    const auto c = dec.counters();
    d.setText(IDC_SMTP_SESSIONS, QString::number(c.smtp_sessions));
    d.setText(IDC_SMTP_EMAILS, QString::number(c.smtp_mails));
    d.setText(IDC_SMTP_ERRORS, QString::number(c.smtp_errors));
    d.setText(IDC_SMTP_LASTERROR, QString::fromStdString(c.smtp_last_error));

    auto gather = [&] {
        int o = d.checked(IDC_SMTP) ? 0x8000 : 0;
        o += std::max(0, d.comboIndex(IDC_SMTP_SETTING));
        for (auto &k : checks) if (d.checked(k.id)) o += k.bit;
        o += (std::max(0, d.comboIndex(IDC_SMTP_SENDIN)) + 1) << 12;
        o += (std::max(0, d.comboIndex(IDC_SMTP_CHARSET)) + 1) << 16;
        return o;
    };
    auto sync = [&] {
        const bool on = d.checked(IDC_SMTP);
        for (int id : {IDC_SMTP_SETTING, IDC_SMTP_PORT, IDC_SMTP_ADDRESS, IDC_SMTP_TIME, IDC_SMTP_DATE, IDC_SMTP_MODE, IDC_SMTP_TYPE, IDC_SMTP_BITRATE,
                       IDC_SMTP_MESSAGE, IDC_SMTP_LABEL, IDC_SMTP_HOST, IDC_SMTP_HELO, IDC_SMTP_TO, IDC_SMTP_FROM, IDC_SMTP_TEST, IDC_SMTP_RESPONSE,
                       IDC_SMTP_AUTH, IDC_SMTP_SENDIN, IDC_SMTP_CHARSET, IDC_SMTP_SSL})
            d.setEnabled(id, on);
        d.setEnabled(IDC_SMTP_USER, on && d.checked(IDC_SMTP_AUTH));
        d.setEnabled(IDC_SMTP_PASSWORD, on && d.checked(IDC_SMTP_AUTH));
    };
    sync();
    QObject::connect(d.get<QCheckBox>(IDC_SMTP), &QCheckBox::toggled, &d, sync);
    QObject::connect(d.get<QCheckBox>(IDC_SMTP_AUTH), &QCheckBox::toggled, &d, sync);

    auto store = [&] {
        sets(dec, "Host", d.text(IDC_SMTP_HOST), "SMTP");
        sets(dec, "HeloDomain", d.text(IDC_SMTP_HELO), "SMTP");
        sets(dec, "To", d.text(IDC_SMTP_TO), "SMTP");
        sets(dec, "From", d.text(IDC_SMTP_FROM), "SMTP");
        sets(dec, "User", d.text(IDC_SMTP_USER), "SMTP");
        sets(dec, "Password", d.text(IDC_SMTP_PASSWORD), "SMTP");
        seti(dec, "Port", d.text(IDC_SMTP_PORT).toInt(), "SMTP");
        const int o = gather();
        seti(dec, "Options", o, "SMTP");
        seti(dec, "Enable", (o & 0x8000) ? 1 : 0, "SMTP");
        seti(dec, "SSL", (o & 0x200000) ? 1 : 0, "SMTP");
    };
    QObject::connect(d.button(IDC_SMTP_TEST), &QPushButton::clicked, &d, [&] {
        const int o = gather();
        if ((o & 0xF) && !(o & ~0xF)) return;
        store();
        dec.apply_settings();
        if (auto *lb = d.get<QListWidget>(IDC_SMTP_RESPONSE)) {
            lb->clear();
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const QString t = QString::fromStdString(dec.mail_test());
            QApplication::restoreOverrideCursor();
            for (const QString &l : t.split('\n', Qt::SkipEmptyParts)) lb->addItem(l);
        }
    });
    if (d.exec() != QDialog::Accepted) return false;
    const int o = gather();
    if ((o & 0xF) && !(o & ~0xF)) {   // nothing selected to be mailed (as the original's check)
    }
    store();
    commit(w);
    return true;
}

// ---- Monitor / Statistics --------------------------------------------------------------------------------------------------------------
bool statistics(MainWindow *w)
{
    auto &dec = w->decoder();
    RcDialog d("MONSTATDLGBOX", w);
    static const int hrN[9] = {IDC_STATHRF64N, IDC_STATHRF32N, IDC_STATHRF16N, IDC_STATHRP24N, IDC_STATHRP12N, IDC_STATHRP512N, IDC_STATHR_ACARS_N, IDC_STATHR_MB_N, IDC_STATHR_EM_N};
    static const int hrA[9] = {IDC_STATHRF64A, IDC_STATHRF32A, IDC_STATHRF16A, IDC_STATHRP24A, IDC_STATHRP12A, IDC_STATHRP512A, IDC_STATHR_ACARS_A, IDC_STATHR_MB_A, IDC_STATHR_EM_A};
    static const int dlN[9] = {IDC_STATDLF64N, IDC_STATDLF32N, IDC_STATDLF16N, IDC_STATDLP24N, IDC_STATDLP12N, IDC_STATDLP512N, IDC_STATDL_ACARS_N, IDC_STATDL_MB_N, IDC_STATDL_EM_N};
    static const int dlA[9] = {IDC_STATDLF64A, IDC_STATDLF32A, IDC_STATDLF16A, IDC_STATDLP24A, IDC_STATDLP12A, IDC_STATDLP512A, IDC_STATDL_ACARS_A, IDC_STATDL_MB_A, IDC_STATDL_EM_A};
    const auto c = dec.counters();
    auto fmt = [](unsigned long v, unsigned long cap) { return v > cap ? QString("******") : QString::number(v); };
    for (int i = 0; i < 9; i++) {
        d.setText(hrN[i], fmt(c.hourly_msgs[i][0], 999999));
        d.setText(hrA[i], fmt(c.hourly_msgs[i][1], 999999));
        d.setText(dlN[i], fmt(c.daily_msgs[i][0], 9999999));
        d.setText(dlA[i], fmt(c.daily_msgs[i][1], 9999999));
    }
    const QString today = QDateTime::currentDateTime().toString("yyMMdd") + ".st";
    d.setChecked(IDC_STATFILEEN, geti(dec, "StatFileEnabled"));
    d.setChecked(IDC_STATFILEDATE, geti(dec, "StatFileUseDate"));
    auto sync = [&] {
        const bool en = d.checked(IDC_STATFILEEN), date = d.checked(IDC_STATFILEDATE);
        d.setEnabled(IDC_STATFILEDATE, en);
        d.setEnabled(IDC_STATFILE, en && !date);
        d.setEnabled(IDC_STATBROWSE, en && !date);
        d.setText(IDC_STATFILE, en ? (date ? today : gets(dec, "StatFile")) : QString());
    };
    sync();
    QObject::connect(d.get<QCheckBox>(IDC_STATFILEEN), &QCheckBox::toggled, &d, [&](bool on) { if (!on) d.setChecked(IDC_STATFILEDATE, false); sync(); });
    QObject::connect(d.get<QCheckBox>(IDC_STATFILEDATE), &QCheckBox::toggled, &d, sync);
    QObject::connect(d.button(IDC_STATBROWSE), &QPushButton::clicked, &d, [&] {
        const QString p = QFileDialog::getOpenFileName(&d, "Open Statistics File", w->dataDir(), "Statistics Files (*.st);;All Files (*)");
        if (!p.isEmpty()) d.setText(IDC_STATFILE, p);
    });
    for (;;) {
        if (d.exec() != QDialog::Accepted) return false;
        const bool en = d.checked(IDC_STATFILEEN), date = d.checked(IDC_STATFILEDATE);
        QString name = d.text(IDC_STATFILE);
        if (en && !date) {
            if (name.isEmpty()) { warn(&d, "PDW Statistics", "You haven't entered a file name!"); continue; }
            if (!name.contains('.')) name += ".st";
            QFile f(name.contains('/') ? name : w->dataDir() + "/Logfiles/" + name);
            QDir().mkpath(QFileInfo(f).absolutePath());
            if (!f.open(QIODevice::Append)) { warn(&d, "PDW Logfile", "Error opening statfile!"); continue; }
        }
        seti(dec, "StatFileEnabled", en);
        seti(dec, "StatFileUseDate", date);
        if (en) sets(dec, "StatFile", date ? QString() : name);
        commit(w);
        return true;
    }
}

// ---- Display / Systemtray -----------------------------------------------------------------------------------------------------------------
bool systemTray(MainWindow *w)
{
    auto &dec = w->decoder();
    RcDialog d("SYSTEMTRAYDLGBOX", w);
    int restore = geti(dec, "SystemTrayRestore");
    d.setChecked(IDC_SYSTEMTRAY, geti(dec, "SystemTray"));
    d.setChecked(IDC_SYSTEMTRAY_RESTORE, restore != 0);
    auto sync = [&] {
        const bool tray = d.checked(IDC_SYSTEMTRAY), rest = d.checked(IDC_SYSTEMTRAY_RESTORE);
        d.setEnabled(IDC_SYSTEMTRAY_RESTORE, tray);
        for (int id : {IDC_SYSTEMTRAY_NEW, IDC_SYSTEMTRAY_MONLY, IDC_SYSTEMTRAY_FILTER}) d.setEnabled(id, tray && rest);
    };
    d.setChecked(IDC_SYSTEMTRAY_NEW, restore == 1);
    d.setChecked(IDC_SYSTEMTRAY_MONLY, restore == 2);
    d.setChecked(IDC_SYSTEMTRAY_FILTER, restore == 3);
    sync();
    QObject::connect(d.get<QCheckBox>(IDC_SYSTEMTRAY), &QCheckBox::toggled, &d, [&](bool on) { if (!on) restore = 0; sync(); });
    QObject::connect(d.get<QCheckBox>(IDC_SYSTEMTRAY_RESTORE), &QCheckBox::toggled, &d, [&](bool on) {
        if (on && !d.checked(IDC_SYSTEMTRAY_NEW) && !d.checked(IDC_SYSTEMTRAY_MONLY) && !d.checked(IDC_SYSTEMTRAY_FILTER)) {
            d.setChecked(IDC_SYSTEMTRAY_NEW, true);
            restore = 1;
        }
        if (!on) restore = 0;
        sync();
    });
    QObject::connect(d.get<QRadioButton>(IDC_SYSTEMTRAY_NEW), &QRadioButton::clicked, &d, [&] { restore = 1; });
    QObject::connect(d.get<QRadioButton>(IDC_SYSTEMTRAY_MONLY), &QRadioButton::clicked, &d, [&] { restore = 2; });
    QObject::connect(d.get<QRadioButton>(IDC_SYSTEMTRAY_FILTER), &QRadioButton::clicked, &d, [&] { restore = 3; });
    if (d.exec() != QDialog::Accepted) return false;
    seti(dec, "SystemTray", d.checked(IDC_SYSTEMTRAY));
    seti(dec, "SystemTrayRestore", restore);
    commit(w);
    w->updateTray();
    return true;
}

// ---- Help ----------------------------------------------------------------------------------------------------------------------------------------
bool debug(MainWindow *w)
{
    auto &dec = w->decoder();
    RcDialog d("DEBUGDLGBOX", w);
    d.setText(IDC_DEBUG_OS, QSysInfo::prettyProductName());
    d.setText(IDC_DEBUG_STARTED, w->startedText());
    d.setText(IDC_DEBUG_INPUT, w->inputDescription());
    d.setText(IDC_DEBUG_FLEXTIME, dec.flextime_detected() ? "Detected" : "Not detected");
    auto refresh = [&] {
        const unsigned long s = dec.seconds_elapsed();
        d.setText(IDC_DEBUG_RUNNING, QString("%1d%2h%3m%4s").arg(s / 86400).arg((s / 3600) % 24).arg((s / 60) % 60).arg(s % 60));
        const auto c = dec.counters();
        d.setText(IDC_DEBUG_MSG, QString::number(c.messages));
        d.setText(IDC_DEBUG_REJECTED, QString::number(c.rejected));
        d.setText(IDC_DEBUG_BLOCKED, QString::number(c.blocked));
        d.setText(IDC_DEBUG_BLOCKBUFFER, QString("%1 / %2").arg(c.block_buffer).arg(c.block_buffer_peak));
        const bool paging = dec.mode() == pdw::Mode::Paging;
        d.setText(IDC_DEBUG_GROUPMSG, paging ? QString::number(c.groupcalls) : "-");
        d.setText(IDC_DEBUG_MISSED, paging ? QString("%1 / %2").arg(c.missed_groupcalls).arg(c.missed_groupcall_messages) : "-");
    };
    refresh();
    QTimer t;
    QObject::connect(&t, &QTimer::timeout, &d, refresh);
    t.start(1000);
    d.exec();
    return false;
}

bool about(MainWindow *w)
{
    RcDialog d("ABOUTDLGBOX", w);
    d.setText(IDC_VERSION, "PDW 3.1 - Linux/macOS port 0.2");
    auto *logo = new QLabel(&d);
    logo->setPixmap(QPixmap(":/pdwlogo2.bmp"));
    logo->setGeometry(12, 16, 119, 79);
    auto *credit = new QLabel("Independent port of PDW by Jason Petty and Peter Hunt.\nGPL-3.0-or-later. Original: github.com/Discriminator/PDW", &d);
    credit->setGeometry(12, d.height() - 40, d.width() - 24, 34);
    QObject::connect(d.button(IDC_WEBSITE), &QPushButton::clicked, &d, [&] {
        QDesktopServices::openUrl(QUrl("https://www.discriminator.nl/pdw/index-en.html"));
        d.accept();
    });
    d.exec();
    return false;
}

}  // namespace dlg
