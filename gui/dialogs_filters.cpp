// Filters: list, add/edit (also several at once), find, duplicate check and filter options.
#include <QApplication>
#include <QDateTime>
#include <QListWidget>
#include <QProgressBar>
#include <QShortcut>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <algorithm>
#include "dialogs.hpp"
#include "generated/rc_ids.h"
#include "mainwindow.hpp"
#include "rcdialog.hpp"

using namespace rc;
using pdw::Filter;

namespace {

const QStringList kTypes = {"FLEX Capcode", "POCSAG Riccode", "TEXT", "ERMES Address", "ACARS Reg.no", "Mobitex MAN"};
const QStringList kLabelColors = {"Default", "Yellow", "Red", "Orange", "Light Blue", "Cyan", "White", "Green", "Gray", "Brown",
                                  "Light Cyan", "Blue", "Magenta", "Sea Green", "Pink", "Ice Blue", "Turqoise", "Don't Change"};
const QString kDontChange = "Don't change";

int geti(pdw::Decoder &d, const char *k, const char *sec = "PDW") { return (int)d.get_int(k, sec); }
void seti(pdw::Decoder &d, const char *k, long long v, const char *sec = "PDW") { d.set_int(k, v, sec); }
QString gets(pdw::Decoder &d, const char *k, const char *sec = "PDW")
{
    std::string v;
    d.get_setting(k, v, sec);
    return QString::fromStdString(v);
}
void sets(pdw::Decoder &d, const char *k, const QString &v, const char *sec = "PDW") { d.set_setting(k, v.toStdString(), sec); }
void warn(QWidget *p, const QString &t, const QString &m) { QMessageBox::critical(p, t, m); }
QColor rgb(uint32_t c) { return QColor((c >> 16) & 255, (c >> 8) & 255, c & 255); }

// BuildFilterString() of the original.
QString buildFilterString(const Filter &f, bool extra)
{
    static const char *types[7] = {"UNUSED", "FLEX", "POCSAG", "TEXT", "ERMES ", "ACARS ", "MOBITX"};
    static const char *waves[11] = {"Default", "Sound-0", "Sound-1", "Sound-2", "Sound-3", "Sound-4", "Sound-5", "Sound-6", "Sound-7", "Sound-8", "Sound-9"};
    QString s = types[std::max(0, std::min(6, f.type))];
    if (f.type == Filter::Acars) s += ' ';
    s += ' ';
    s += f.capcode.empty() ? QString("         ") : QString::fromStdString(f.capcode);
    if (f.type == Filter::Mobitex && !f.capcode.empty() && f.capcode.back() == 'X') s.chop(2);
    else if (f.type == Filter::Pocsag && f.capcode.size() >= 2 && f.capcode[f.capcode.size() - 2] == '-') s.chop(2);
    if (f.reject) {
        s += " | REJECT";
        if (!f.text.empty() && f.capcode.empty()) s += " | \"" + QString::fromStdString(f.text) + "\"";
        return s;
    }
    if (extra) {
        s += " | ";
        s += f.cmd_enabled ? "CMD" : "cmd";
        s += " | ";
        s += f.label_enabled ? "LAB" : "lab";
        s += " | ";
        s += f.sep_filterfile_en ? "SEP" : "sep";
        s += " | ";
        if (f.monitor_only) s += f.wave_number ? "M-ONLY " : "NoSound";
        else s += f.wave_number == -1 ? "NoSound" : waves[std::max(0, std::min(10, f.wave_number))];
    }
    if (!f.label.empty()) s += " | " + QString::fromStdString(f.label);
    if (!f.text.empty()) s += " | \"" + QString::fromStdString(f.text) + "\"";
    return s;
}

int capcodeLen(int type, bool groupMode)
{
    const int len[7] = {0, groupMode ? 7 : 9, 7, 0, 7, 6, 7};
    return len[std::max(0, std::min(6, type))];
}

// ---- add / edit ------------------------------------------------------------------------------------------------------------------
struct EditResult { bool ok = false; std::vector<int> newSelection; };

// Edits `sel` (or adds a filter after the first selected one when `editing` is false).
class FilterEditor {
public:
    FilterEditor(MainWindow *w, QWidget *parent, std::vector<Filter> &list, std::vector<int> sel, bool editing)
        : w_(w), dec_(w->decoder()), list_(list), sel_(std::move(sel)), editing_(editing), dlg_("FILTEREDITDLGBOX", parent)
    {
        multiple_ = editing_ && sel_.size() > 1;
        groupMode_ = geti(dec_, "FlexGroupMode") != 0;
        if (editing_) filter_ = list_[sel_.front()];
        else {
            filter_ = Filter();
            filter_.type = geti(dec_, "FilterDefaultType", "Filter") + 1;
        }
        build();
    }

    EditResult run()
    {
        EditResult r;
        r.newSelection = sel_;
        dlg_.exec();
        r.ok = committed_;
        r.newSelection = sel_;
        return r;
    }

private:
    MainWindow *w_;
    pdw::Decoder &dec_;
    std::vector<Filter> &list_;
    std::vector<int> sel_;
    bool editing_, multiple_ = false, groupMode_ = false, committed_ = false, initializing_ = true;
    RcDialog dlg_;
    Filter filter_;
    // per-field "differs among the selection" flags (multi edit)
    bool d_capcode_ = false, d_text_ = false, d_label_ = false, d_sep_[3] = {false, false, false};
    bool d_mono_ = false, d_reject_ = false, d_exact_ = false, d_labelen_ = false, d_cmd_ = false, d_color_ = false, d_audio_ = false,
         d_smtp_ = false, d_sepen_ = false;
    int sepFiles_ = 0;

    void setTri(int id, bool differs, bool value)
    {
        auto *c = dlg_.get<QCheckBox>(id);
        if (!c) return;
        c->setTristate(differs);
        c->setCheckState(differs ? Qt::PartiallyChecked : (value ? Qt::Checked : Qt::Unchecked));
    }
    Qt::CheckState tri(int id) const { auto *c = dlg_.get<QCheckBox>(id); return c ? c->checkState() : Qt::Unchecked; }

    void fillAudio()
    {
        auto *cb = dlg_.get<QComboBox>(IDC_FILTERAUDIO);
        QSignalBlocker b(cb);
        cb->clear();
        cb->addItem("No sound");
        const bool mono = dlg_.get<QCheckBox>(IDC_FILTER_MONITOR_ONLY)->checkState() == Qt::Checked;
        if (mono) cb->addItem("Monitor_only.wav");
        else {
            cb->addItem("Default");
            for (int i = 0; i < 10; i++) cb->addItem(QString("Sound%1.wav").arg(i));
        }
        if (multiple_) cb->addItem(kDontChange);
    }

    void build()
    {
        auto &d = dlg_;
        d.setWindowTitle(!editing_ ? "PDW Add Filter" : multiple_ ? "PDW (multiple) Edit Filter" : "PDW Edit Filter");
        d.fillCombo(IDC_FILTERTYPE, kTypes);
        d.fillCombo(IDC_FILTERLABELCOLOR, multiple_ ? kLabelColors : kLabelColors.mid(0, 17));
        // label colours drawn in their own colour on the pane background
        if (auto *lc = d.get<QComboBox>(IDC_FILTERLABELCOLOR)) {
            for (int i = 0; i < lc->count() - (multiple_ ? 1 : 0); i++) {
                lc->setItemData(i, QBrush(rgb(dec_.color_rgb(20 + i))), Qt::ForegroundRole);
                lc->setItemData(i, QBrush(rgb(dec_.color_rgb(1))), Qt::BackgroundRole);
            }
        }
        d.setEnabled(IDC_FILTERTYPE, !editing_);
        if (geti(dec_, "Enable", "SMTP")) { d.setEnabled(IDC_FILTERSMTP, true); d.setText(IDC_FILTERSMTP, "Send email"); }

        if (editing_ && multiple_) {
            for (int idx : sel_) {
                const Filter &f = list_[idx];
                if (f.capcode != filter_.capcode) d_capcode_ = true;
                if (f.text != filter_.text) d_text_ = true;
                if (f.label != filter_.label) d_label_ = true;
                for (int i = 0; i < 3; i++) {
                    const std::string a = i < (int)f.sep_filterfiles.size() ? f.sep_filterfiles[i] : "";
                    const std::string b = i < (int)filter_.sep_filterfiles.size() ? filter_.sep_filterfiles[i] : "";
                    if (a != b) d_sep_[i] = true;
                }
                d_mono_ |= f.monitor_only != filter_.monitor_only;
                d_reject_ |= f.reject != filter_.reject;
                d_exact_ |= f.match_exact_msg != filter_.match_exact_msg;
                d_labelen_ |= f.label_enabled != filter_.label_enabled;
                d_cmd_ |= f.cmd_enabled != filter_.cmd_enabled;
                d_color_ |= f.label_color != filter_.label_color;
                d_audio_ |= f.wave_number != filter_.wave_number;
                d_smtp_ |= f.smtp != filter_.smtp;
                d_sepen_ |= f.sep_filterfile_en != filter_.sep_filterfile_en;
                sepFiles_ = std::max(sepFiles_, (int)f.sep_filterfiles.size());
            }
            unsigned hits = 0;
            for (int idx : sel_) hits += list_[idx].hitcounter;
            d.setText(IDC_FILTERHITS, QString("Total number of hits: %1").arg(hits));
            d.setText(IDC_FILTERLASTHIT, "(total of selection)");
            d.ctl(IDC_FILTERRESET)->hide();
            d.ctl(IDC_DONTCHANGE)->show();
            for (int id : {IDC_FILTER_PREVIOUS, IDC_FILTER_APPLY, IDC_FILTER_NEXT}) d.ctl(id)->hide();
        } else if (editing_) {
            d.setText(IDC_FILTERHITS, QString("Number of hits: %1").arg(filter_.hitcounter));
            if (filter_.hitcounter) {
                d.setText(IDC_FILTERLASTHIT, QString("Last hit: %1  %2").arg(QString::fromStdString(filter_.lasthit_date), QString::fromStdString(filter_.lasthit_time)));
                d.setEnabled(IDC_FILTERRESET, true);
            }
        }
        if (!editing_) for (int id : {IDC_FILTER_PREVIOUS, IDC_FILTER_APPLY, IDC_FILTER_NEXT}) d.ctl(id)->hide();
        else if (!multiple_) {
            d.setEnabled(IDC_FILTER_NEXT, sel_.front() != (int)list_.size() - 1);
            d.setEnabled(IDC_FILTER_PREVIOUS, sel_.front() != 0);
        }
        load();
        connectSignals();
        initializing_ = false;
        update();
        d.get<QLineEdit>(IDC_FILTERCAPCODE)->setFocus();
    }

    void load()
    {
        auto &d = dlg_;
        const bool wasInit = initializing_;
        initializing_ = true;
        const int cl = capcodeLen(filter_.type, groupMode_);
        d.get<QLineEdit>(IDC_FILTERCAPCODE)->setMaxLength(cl ? cl : 32);
        d.get<QLineEdit>(IDC_FILTERLABEL)->setMaxLength(256);
        d.get<QLineEdit>(IDC_FILTERTEXT)->setMaxLength(40);
        const QString cap = QString::fromStdString(filter_.capcode).left(cl);
        d.setText(IDC_FILTERCAPCODE, d_capcode_ ? kDontChange : cap);
        d.setText(IDC_FILTERTEXT, d_text_ ? kDontChange : QString::fromStdString(filter_.text));
        d.setText(IDC_FILTERLABEL, d_label_ ? kDontChange : QString::fromStdString(filter_.label));
        for (int i = 0; i < 3; i++)
            d.setText(IDC_SEPFILTERFILE1 + i, d_sep_[i] ? kDontChange : (i < (int)filter_.sep_filterfiles.size() ? QString::fromStdString(filter_.sep_filterfiles[i]) : QString()));
        d.setComboIndex(IDC_FILTERLABELCOLOR, d_color_ ? 17 : filter_.label_color);
        setTri(IDC_FILTER_MONITOR_ONLY, d_mono_, filter_.monitor_only);
        setTri(IDC_FILTERREJECT, d_reject_, filter_.reject);
        setTri(IDC_FILTERMATCHEXACT, d_exact_, filter_.match_exact_msg);
        setTri(IDC_FILTERLABELEN, d_labelen_, filter_.label_enabled);
        setTri(IDC_FILTERCMD, d_cmd_, filter_.cmd_enabled);
        setTri(IDC_FILTERSMTP, d_smtp_, filter_.smtp);
        setTri(IDC_SEPFILTERFILEEN, d_sepen_, filter_.sep_filterfile_en);
        fillAudio();
        auto *au = d.get<QComboBox>(IDC_FILTERAUDIO);
        if (d_mono_) d.setEnabled(IDC_FILTERAUDIO, false);
        else if (!filter_.monitor_only) au->setCurrentIndex(std::max(0, filter_.wave_number + 1));
        else au->setCurrentIndex(std::max(0, filter_.wave_number));
        if (multiple_ && d_audio_) au->setCurrentIndex(au->count() - 1);
        d.setComboIndex(IDC_FILTERTYPE, filter_.type - 1);
        // POCSAG function number / Mobitex RX-TX
        d.fillCombo(IDC_FILTERFNU, {"All", "1", "2", "3", "4"});
        const std::string &c = filter_.capcode;
        if (filter_.type == Filter::Mobitex) {
            auto *rx = d.get<QCheckBox>(IDC_FILTERRXTXMAN);
            rx->setTristate(true);
            const char k = c.size() >= 2 ? c[c.size() - 2] : 0;
            rx->setCheckState(k == 'R' ? Qt::Unchecked : k == 'T' ? Qt::Checked : Qt::PartiallyChecked);
        } else if (filter_.type == Filter::Pocsag) {
            d.setComboIndex(IDC_FILTERFNU, c.size() >= 2 && c[c.size() - 2] == '-' ? atoi(c.c_str() + c.size() - 1) : 0);
        }
        initializing_ = wasInit;
    }

    // WM_WININICHANGE in the original: recompute which controls make sense now.
    void update()
    {
        if (initializing_) return;
        auto &d = dlg_;
        const int type = d.comboIndex(IDC_FILTERTYPE) + 1;
        const bool reject = tri(IDC_FILTERREJECT) == Qt::Checked;
        const bool capcode = !d.text(IDC_FILTERCAPCODE).isEmpty(), text = !d.text(IDC_FILTERTEXT).isEmpty();
        const bool captxt = capcode || text, label = !d.text(IDC_FILTERLABEL).isEmpty();
        const bool sepEn = tri(IDC_SEPFILTERFILEEN) == Qt::Checked;
        const bool sep1 = !d.text(IDC_SEPFILTERFILE1).isEmpty(), sep2 = !d.text(IDC_SEPFILTERFILE2).isEmpty(), sep3 = !d.text(IDC_SEPFILTERFILE3).isEmpty();
        d.setEnabled(IDC_FILTERREJECT, captxt);
        d.setEnabled(IDC_FILTER_MONITOR_ONLY, captxt);
        const bool mobitex = type == Filter::Mobitex;
        if (!mobitex) {
            d.setEnabled(IDC_FILTERMATCHEXACT, text && d.text(IDC_FILTERTEXT).left(1) != "^");
            d.ctl(IDC_FILTERFNU)->setVisible(type == Filter::Pocsag);
            d.setEnabled(IDC_FILTERFNU, capcode);
            d.ctl(IDC_FILTERCAPCODE)->setFixedWidth(type == Filter::Pocsag ? d.ctl(IDC_FILTERFNU)->x() - d.ctl(IDC_FILTERCAPCODE)->x() - 3
                                                                           : d.ctl(IDC_FILTERMATCHEXACT)->x() - 8 - d.ctl(IDC_FILTERCAPCODE)->x());
        } else {
            d.setEnabled(IDC_FILTERRXTXMAN, capcode);
            auto *rx = d.get<QCheckBox>(IDC_FILTERRXTXMAN);
            rx->setText(rx->checkState() == Qt::Unchecked ? "Only Receiving MAN" : rx->checkState() == Qt::Checked ? "Only Sending MAN" : "Both RX and TX MAN");
            d.ctl(IDC_FILTERFNU)->hide();
        }
        d.ctl(IDC_FILTERRXTXMAN)->setVisible(mobitex);
        d.ctl(IDC_FILTERMATCHEXACT)->setVisible(!mobitex);
        if (type != Filter::Text) {
            if (!multiple_) d.setEnabled(IDC_FILTERCAPCODE, true);
            d.setEnabled(IDC_FILTERTEXT, capcode);
        } else {
            d.setEnabled(IDC_FILTERCAPCODE, false);
            d.setEnabled(IDC_FILTERTEXT, true);
        }
        d.setEnabled(IDC_FILTERAUDIO, tri(IDC_FILTER_MONITOR_ONLY) != Qt::PartiallyChecked && !reject && captxt);
        d.setEnabled(IDC_FILTERLABEL, !reject && captxt);
        d.setEnabled(IDC_FILTERLABELEN, !reject && label);
        d.setEnabled(IDC_FILTERLABELCOLOR, !reject && label);
        d.setEnabled(IDC_SEPFILTERFILEEN, !reject && captxt);
        d.setEnabled(IDC_SEPFILTERFILEBROWSE1, sepEn);
        d.ctl(IDC_SEPFILTERFILEBROWSE2)->setVisible(sep1);
        d.ctl(IDC_SEPFILTERFILEBROWSE3)->setVisible(sep2);
        d.setText(IDC_SEPFILTERFILEBROWSE1, sep1 ? "Clear" : "Browse");
        d.setText(IDC_SEPFILTERFILEBROWSE2, sep2 ? "Clear" : "Browse");
        d.setText(IDC_SEPFILTERFILEBROWSE3, sep3 ? "Clear" : "Browse");
        // keep the separate file list packed to the top
        for (int i = 0; i < 2; i++) {
            if (!d.text(IDC_SEPFILTERFILE3 - i).isEmpty() && d.text(IDC_SEPFILTERFILE2 - i).isEmpty()) {
                d.setText(IDC_SEPFILTERFILE2 - i, d.text(IDC_SEPFILTERFILE3 - i));
                d.setText(IDC_SEPFILTERFILE3 - i, "");
            }
        }
        sepFiles_ = (!d.text(IDC_SEPFILTERFILE1).isEmpty()) + (!d.text(IDC_SEPFILTERFILE2).isEmpty()) + (!d.text(IDC_SEPFILTERFILE3).isEmpty());
        d.setEnabled(IDC_FILTERCMD, multiple_ ? true : (captxt && !reject));
        d.setEnabled(IDC_FILTERSMTP, captxt && geti(dec_, "Enable", "SMTP"));
    }

    void connectSignals()
    {
        auto &d = dlg_;
        auto upd = [this] { update(); };
        for (int id : {IDC_FILTERCAPCODE, IDC_FILTERTEXT, IDC_FILTERLABEL, IDC_SEPFILTERFILE1, IDC_SEPFILTERFILE2, IDC_SEPFILTERFILE3})
            QObject::connect(d.get<QLineEdit>(id), &QLineEdit::textChanged, &d, upd);
        for (int id : {IDC_FILTERLABELEN, IDC_FILTERRXTXMAN, IDC_FILTERMATCHEXACT})
            QObject::connect(d.get<QCheckBox>(id), &QCheckBox::stateChanged, &d, [upd](int) { upd(); });
        QObject::connect(d.get<QComboBox>(IDC_FILTERFNU), &QComboBox::currentIndexChanged, &d, upd);
        QObject::connect(d.get<QComboBox>(IDC_FILTERTYPE), &QComboBox::activated, &d, [this] {
            if (initializing_) return;
            filter_.type = dlg_.comboIndex(IDC_FILTERTYPE) + 1;
            load();
            update();
        });
        QObject::connect(d.get<QCheckBox>(IDC_FILTERREJECT), &QCheckBox::clicked, &d, [this] {
            if (multiple_) return;
            auto &dd = dlg_;
            dd.setText(IDC_SEPFILTERFILE1, "");
            if (tri(IDC_FILTERREJECT) == Qt::Checked) {
                dd.setChecked(IDC_FILTER_MONITOR_ONLY, false);
                dd.setChecked(IDC_SEPFILTERFILEEN, false);
                dd.setComboIndex(IDC_FILTERLABELCOLOR, 0);
            } else dd.setComboIndex(IDC_FILTERLABELCOLOR, 0);
            update();
        });
        QObject::connect(d.get<QCheckBox>(IDC_FILTER_MONITOR_ONLY), &QCheckBox::clicked, &d, [this] {
            auto &dd = dlg_;
            const bool mono = tri(IDC_FILTER_MONITOR_ONLY) == Qt::Checked;
            if (mono) dd.setChecked(IDC_FILTERREJECT, false);
            fillAudio();
            auto *au = dd.get<QComboBox>(IDC_FILTERAUDIO);
            au->setCurrentIndex(multiple_ ? au->count() - 1 : 1);
            update();
        });
        QObject::connect(d.get<QCheckBox>(IDC_SEPFILTERFILEEN), &QCheckBox::clicked, &d, [this] {
            for (int i = 0; i < 3; i++) dlg_.setText(IDC_SEPFILTERFILE1 + i, "");
            update();
        });
        QObject::connect(d.button(IDC_FILTERRESET), &QPushButton::clicked, &d, [this] {
            filter_.hitcounter = 0;
            filter_.lasthit_date.clear();
            filter_.lasthit_time.clear();
            dlg_.setText(IDC_FILTERHITS, "Number of hits: 0");
            dlg_.setText(IDC_FILTERLASTHIT, "");
            dlg_.setEnabled(IDC_FILTERRESET, false);
        });
        for (int i = 0; i < 3; i++) {
            QObject::connect(d.button(IDC_SEPFILTERFILEBROWSE1 + i), &QPushButton::clicked, &d, [this, i] {
                const int edit = IDC_SEPFILTERFILE1 + i;
                if (!dlg_.text(edit).isEmpty()) { dlg_.setText(edit, ""); return; }
                const QString p = QFileDialog::getOpenFileName(&dlg_, "Open Separate Filter File...", gets(dec_, "LogfilePath"), "Text File (*.txt);;All Files (*)");
                if (p.isEmpty()) return;
                dlg_.setText(edit, QFileInfo(p).absolutePath().compare(QDir::cleanPath(gets(dec_, "LogfilePath")), Qt::CaseInsensitive) == 0 ? QFileInfo(p).fileName() : p);
            });
        }
        QObject::connect(d.button(IDOK), &QPushButton::clicked, &d, [this] { /* handled below */ });
        d.button(IDOK)->disconnect();
        QObject::connect(d.button(IDOK), &QPushButton::clicked, &d, [this] { if (apply(false)) dlg_.accept(); });
        QObject::connect(d.button(IDC_FILTER_APPLY), &QPushButton::clicked, &d, [this] { apply(true); });
        for (int id : {IDC_FILTER_NEXT, IDC_FILTER_PREVIOUS})
            QObject::connect(d.button(id), &QPushButton::clicked, &d, [this, id] {
                int idx = sel_.front() + (id == IDC_FILTER_NEXT ? 1 : -1);
                if (idx < 0 || idx >= (int)list_.size()) return;
                sel_ = {idx};
                filter_ = list_[idx];
                d_capcode_ = d_text_ = d_label_ = false;
                load();
                dlg_.setText(IDC_FILTERHITS, QString("Number of hits: %1").arg(filter_.hitcounter));
                dlg_.setText(IDC_FILTERLASTHIT, filter_.hitcounter ? QString("Last hit: %1  %2").arg(QString::fromStdString(filter_.lasthit_date), QString::fromStdString(filter_.lasthit_time)) : QString());
                dlg_.setEnabled(IDC_FILTERRESET, filter_.hitcounter != 0);
                dlg_.setEnabled(IDC_FILTER_NEXT, idx != (int)list_.size() - 1);
                dlg_.setEnabled(IDC_FILTER_PREVIOUS, idx != 0);
                update();
            });
    }

    // OK / Apply: validate like the original, then write the fields into the filters concerned.
    bool apply(bool stay)
    {
        auto &d = dlg_;
        Filter f = filter_;
        f.reject = tri(IDC_FILTERREJECT) == Qt::Checked;
        const bool capDont = d.text(IDC_FILTERCAPCODE).startsWith("Don't cha");
        QString cap = d.text(IDC_FILTERCAPCODE);
        if (f.type != Filter::Text && !capDont) {
            const int len = cap.size(), need = capcodeLen(f.type, groupMode_);
            if (!len) { warn(&d, "PDW Filter address", "You must enter an Address!"); return false; }
            QString full;
            int l = len;
            while (l < need) {
                if (f.type == Filter::Acars || f.type == Filter::Mobitex) { warn(&d, "PDW Filter address", "The address is too short!"); return false; }
                if (f.type == Filter::Flex) {
                    if (l != 7) { warn(&d, "PDW Filter address", "A FLEX address needs to contain 7 or 9 digits!"); return false; }
                    break;
                }
                full += '0';
                l++;
            }
            full += cap;
            if (f.type == Filter::Acars) full = full.toUpper();
            int bad = -1;
            for (int i = 0; i < len; i++) {
                const QChar c = full[full.size() - len + i];
                const bool okc = f.type == Filter::Acars ? (c >= 'A' && c <= 'Z') || c == '-' || c == '?' : (c >= '0' && c <= '9') || c == '?';
                if (!okc) { bad = i; break; }
            }
            if (bad >= 0) {
                warn(&d, "PDW Filter address", "Invalid character(s) in Address!");
                d.get<QLineEdit>(IDC_FILTERCAPCODE)->setSelection(bad, 1);
                return false;
            }
            if (f.type == Filter::Mobitex) {
                const Qt::CheckState s = tri(IDC_FILTERRXTXMAN);
                if (s == Qt::Checked) full += "TX";
                else if (s == Qt::Unchecked) full += "RX";
            } else if (f.type == Filter::Pocsag) {
                if (d.comboIndex(IDC_FILTERFNU) > 0) full += QString("-%1").arg(d.comboIndex(IDC_FILTERFNU));
            } else if (f.type == Filter::Flex && full.size() == 9 && full.startsWith("00") && full < "002101249") full = full.mid(2);
            f.capcode = full.toStdString();
        }
        QString text = d.text(IDC_FILTERTEXT);
        if (!text.isEmpty() && text != kDontChange) {
            if (text.contains('"')) { warn(&d, "PDW Filter Text", "The character \" is not allowed here!"); return false; }
            if (text.indexOf('^') > 0) { warn(&d, "PDW Filter Text", "The character ^ can only be used\nat the beginning of the line"); return false; }
            f.match_exact_msg = tri(IDC_FILTERMATCHEXACT) == Qt::Checked;
        } else if (f.type == Filter::Text && text != kDontChange) {
            warn(&d, "PDW Filter Text", "You must enter a text string!");
            return false;
        }
        f.label_enabled = tri(IDC_FILTERLABELEN) == Qt::Checked;
        QString label = d.text(IDC_FILTERLABEL);
        if (label != kDontChange) {
            if (label.isEmpty() && f.label_enabled) f.label_enabled = false;
            else if (label.contains('"')) { warn(&d, "PDW Filter Label", "The character \" is not allowed here!"); return false; }
        }
        std::string sep[3];
        if (tri(IDC_SEPFILTERFILEEN) == Qt::Checked) {
            if (d.text(IDC_SEPFILTERFILE1).isEmpty() && d.text(IDC_SEPFILTERFILE2).isEmpty() && d.text(IDC_SEPFILTERFILE3).isEmpty()) {
                warn(&d, "PDW Separate filterfile", "If you enable the separate filter file\nyou must enter at least one filename!");
                return false;
            }
            for (int j = 0; j < sepFiles_; j++) {
                const QString name = d.text(IDC_SEPFILTERFILE1 + j);
                if (name.contains(';')) { warn(&d, "PDW Separate filterfile", "The character ; is not allowed here!"); return false; }
                if (name != kDontChange) {
                    const QString full = name.contains('/') ? name : gets(dec_, "LogfilePath") + "/" + name;
                    QDir().mkpath(QFileInfo(full).absolutePath());
                    if (!QFile::exists(full)) {
                        QFile t(full);
                        if (!t.open(QIODevice::Append)) { warn(&d, "PDW Separate Filterfile", "Error opening separate filter file!"); return false; }
                        t.close();
                        QFile::remove(full);
                    }
                }
                sep[j] = name.toStdString();
                if (j >= 1) {
                    for (int k = 0; k < j; k++)
                        if (name.compare(QString::fromStdString(sep[k]), Qt::CaseInsensitive) == 0 && name != kDontChange) {
                            warn(&d, "PDW Separate Filterfile", QString("Separate filterfiles %1 and %2 are equal.\nPlease change one of them.").arg(k + 1).arg(j + 1));
                            return false;
                        }
                }
            }
        }
        // ---- write ----
        const int audioIdx = d.comboIndex(IDC_FILTERAUDIO);
        const int colorIdx = d.comboIndex(IDC_FILTERLABELCOLOR);
        std::vector<int> targets;
        if (!editing_) {
            const int at = list_.empty() ? 0 : (sel_.empty() ? (int)list_.size() : sel_.front() + 1);
            list_.insert(list_.begin() + at, f);
            targets = {at};
        } else targets = sel_;
        for (int idx : targets) {
            Filter &t = list_[idx];
            if (!capDont && f.type != Filter::Text) t.capcode = f.capcode;
            else if (!editing_) t.capcode = f.capcode;
            if (tri(IDC_FILTERREJECT) != Qt::PartiallyChecked) t.reject = f.reject;
            if (!f.reject) {
                if (tri(IDC_FILTER_MONITOR_ONLY) != Qt::PartiallyChecked) {
                    t.monitor_only = tri(IDC_FILTER_MONITOR_ONLY) == Qt::Checked;
                    const int dontChange = t.monitor_only ? 2 : 12;
                    if (audioIdx != dontChange || !multiple_) {
                        t.wave_number = audioIdx;
                        if (!t.monitor_only) t.wave_number -= 1;
                    }
                }
                if (text != kDontChange) t.text = text.toStdString();
                if (tri(IDC_FILTERLABELEN) != Qt::PartiallyChecked) t.label_enabled = f.label_enabled;
                if (label.isEmpty()) { t.label_color = 0; t.label.clear(); }
                else {
                    if (colorIdx != 17) t.label_color = colorIdx;
                    if (label != kDontChange) t.label = label.toStdString();
                }
                if (tri(IDC_FILTERMATCHEXACT) != Qt::PartiallyChecked) t.match_exact_msg = tri(IDC_FILTERMATCHEXACT) == Qt::Checked;
                if (tri(IDC_FILTERCMD) != Qt::PartiallyChecked) t.cmd_enabled = tri(IDC_FILTERCMD) == Qt::Checked;
                if (tri(IDC_FILTERSMTP) != Qt::PartiallyChecked) t.smtp = tri(IDC_FILTERSMTP) == Qt::Checked;
                if (tri(IDC_SEPFILTERFILEEN) != Qt::PartiallyChecked) {
                    t.sep_filterfile_en = tri(IDC_SEPFILTERFILEEN) == Qt::Checked;
                    if (!t.sep_filterfile_en) t.sep_filterfiles.clear();
                    else {
                        t.sep_filterfiles.resize(sepFiles_);
                        for (int j = 0; j < sepFiles_; j++) {
                            if (sep[j] == kDontChange.toStdString()) continue;
                            std::string s = sep[j];
                            if (s.find('.') == std::string::npos && !s.empty()) s += ".txt";
                            t.sep_filterfiles[j] = s;
                        }
                    }
                }
            }
            if (editing_ && !multiple_) {
                t.hitcounter = filter_.hitcounter;
                t.lasthit_date = filter_.lasthit_date;
                t.lasthit_time = filter_.lasthit_time;
            }
            if (!editing_ || !multiple_) filter_ = t;
        }
        committed_ = true;
        sel_ = targets;
        (void)stay;
        return true;
    }
};

// ---- find --------------------------------------------------------------------------------------------------------------------------
struct FindState { QString last; bool caseSensitive = false; };
FindState g_find;

// FindFilter() of the original: returns the number of hits and selects the first one after `from`.
int findFilter(const std::vector<Filter> &list, QListWidget *lw, int from, const QString &needle, bool showHits, bool backwards)
{
    QString s = needle.isEmpty() ? g_find.last : needle;
    if (!needle.isEmpty()) g_find.last = needle;
    if (!g_find.caseSensitive) s = s.toUpper();
    const int n = (int)list.size();
    if (n == 0) return 0;
    if (from < 0) { from = lw->currentRow() + 1; if (backwards) from -= 2; }
    int hits = 0, match = -1;
    for (int i = 0; i < n; i++, backwards ? from-- : from++) {
        if (from >= n) from = 0;
        else if (from < 0) from = n - 1;
        QString a = QString::fromStdString(list[from].capcode), t = QString::fromStdString(list[from].text), l = QString::fromStdString(list[from].label);
        if (!g_find.caseSensitive) { t = t.toUpper(); l = l.toUpper(); }
        if (a.contains(s) || l.contains(s) || t.contains(s)) {
            if (match < 0) match = from;
            hits++;
            if (!showHits) break;
        }
    }
    if (match >= 0) {
        lw->clearSelection();
        lw->setCurrentRow(match);
        lw->item(match)->setSelected(true);
        lw->scrollToItem(lw->item(match));
    } else QApplication::beep();
    return hits;
}

}  // namespace

namespace dlg {

// ---- Filter list ---------------------------------------------------------------------------------------------------------------------
bool filters(MainWindow *w)
{
    auto &dec = w->decoder();
    std::vector<Filter> list = dec.filters();
    QDialog d(w);
    d.setFont(RcDialog::dialogFont("Verdana", 8));
    d.resize(std::max(700, w->width() - 80), std::max(420, w->height() - 60));
    auto *lay = new QVBoxLayout(&d);
    auto *lw = new QListWidget;
    lw->setSelectionMode(QAbstractItemView::ExtendedSelection);
    lw->setContextMenuPolicy(Qt::CustomContextMenu);
    QFont mono = RcDialog::dialogFont("Verdana", 8);
    mono.setBold(true);
    lw->setFont(mono);
    lay->addWidget(lw, 1);
    auto *row = new QHBoxLayout;
    auto *add = new QPushButton("Add"), *edit = new QPushButton("Edit"), *del = new QPushButton("Remove");
    auto *opts = new QPushButton("Options"), *find = new QPushButton("Find"), *ok = new QPushButton("OK");
    for (auto *b : {add, edit, del}) row->addWidget(b);
    row->addSpacing(20);
    row->addWidget(opts);
    row->addWidget(find);
    row->addStretch();
    row->addWidget(ok);
    lay->addLayout(row);
    QObject::connect(ok, &QPushButton::clicked, &d, &QDialog::accept);

    const bool colored = geti(dec, "FilterWindowColors") != 0;
    const bool extra = geti(dec, "FilterWindowExtra") != 0;
    auto itemText = [&](int i) { return buildFilterString(list[i], extra); };
    auto paintItem = [&](int i) {
        QListWidgetItem *it = lw->item(i);
        it->setText(itemText(i));
        if (colored) {
            it->setForeground(list[i].label_enabled ? rgb(dec.color_rgb(20 + list[i].label_color)) : QColor(0x44, 0x44, 0x44));
            it->setBackground(rgb(dec.color_rgb(1)));
        }
    };
    auto title = [&] {
        const auto sel = lw->selectedItems();
        QString t = QString("PDW Filters (%1)").arg(list.size());
        if (sel.size() > 1) t += QString(" - %1 filters selected").arg(sel.size());
        else if (sel.size() == 1) {
            const Filter &f = list[lw->row(sel[0])];
            if (f.type == Filter::Text) t += " - \"" + QString::fromStdString(f.text) + "\"" + (f.label.empty() ? "" : " | " + QString::fromStdString(f.label));
            else {
                t += " - " + QString::fromStdString(f.capcode);
                if (!f.label.empty()) t += " | " + QString::fromStdString(f.label);
                if (!f.text.empty()) t += " | \"" + QString::fromStdString(f.text) + "\"";
            }
        }
        d.setWindowTitle(t);
        const bool more = sel.size() > 1;
        add->setEnabled(!more);
        find->setEnabled(!more && !list.empty());
    };
    auto reload = [&](int select) {
        lw->clear();
        for (size_t i = 0; i < list.size(); i++) { lw->addItem(""); paintItem((int)i); }
        if (!list.empty()) { lw->setCurrentRow(std::max(0, std::min(select, (int)list.size() - 1))); lw->currentItem()->setSelected(true); }
        title();
    };
    if (colored) {
        QPalette p = lw->palette();
        p.setColor(QPalette::Base, rgb(dec.color_rgb(1)));
        lw->setPalette(p);
    }
    auto selected = [&] {
        std::vector<int> s;
        for (auto *it : lw->selectedItems()) s.push_back(lw->row(it));
        std::sort(s.begin(), s.end());
        return s;
    };
    QObject::connect(lw, &QListWidget::itemSelectionChanged, &d, title);
    reload(0);

    auto doAdd = [&] {
        if (selected().size() > 1) return;
        std::vector<int> s = selected();
        FilterEditor ed(w, &d, list, s, false);
        const EditResult r = ed.run();
        if (r.ok) reload(r.newSelection.front());
    };
    auto doEdit = [&] {
        auto s = selected();
        if (s.empty()) { warn(&d, "PDW Filter", "You must select a filter to edit!"); return; }
        FilterEditor ed(w, &d, list, s, true);
        const EditResult r = ed.run();
        if (r.ok) {
            const int first = r.newSelection.empty() ? s.front() : r.newSelection.front();
            reload(first);
            lw->clearSelection();
            for (int i : s) if (i < lw->count()) lw->item(i)->setSelected(true);
            title();
        }
    };
    auto doDel = [&] {
        auto s = selected();
        if (s.empty()) { warn(&d, "PDW Filter", "You must select a filter to remove!"); return; }
        const QString q = s.size() > 1 ? "Remove selected filters ?" : QString("Remove filter '%1' ?").arg(QString::fromStdString(list[s[0]].type != Filter::Text ? list[s[0]].capcode : list[s[0]].text));
        if (QMessageBox::question(&d, s.size() > 1 ? "PDW Remove filters" : "PDW Remove filter", q) != QMessageBox::Yes) return;
        for (auto it = s.rbegin(); it != s.rend(); ++it) list.erase(list.begin() + *it);
        reload(s.front());
    };
    auto doFind = [&] {
        if (list.empty()) return;
        QDialog fd(&d);
        RcDialog f("FILTERFINDDLGBOX", &d);
        f.setText(IDC_FILTERFIND, g_find.last.isEmpty() ? "address / text / label" : g_find.last);
        if (g_find.last.isEmpty()) f.get<QLineEdit>(IDC_FILTERFIND)->selectAll();
        f.setChecked(IDC_FILTERFIND_CASE, g_find.caseSensitive);
        QString old;
        auto run = [&](bool again) {
            const QString s = f.text(IDC_FILTERFIND);
            if (s.isEmpty()) { f.setText(IDC_FILTERFIND_HITS, "0"); return; }
            int from = lw->currentRow();
            if (again && s == old) from = lw->currentRow() + 1;
            const int hits = findFilter(list, lw, from, s, true, false);
            f.setText(IDC_FILTERFIND_HITS, QString::number(hits));
            old = s;
        };
        QObject::connect(f.get<QLineEdit>(IDC_FILTERFIND), &QLineEdit::textChanged, &f, [&] { run(false); });
        QObject::connect(f.get<QLineEdit>(IDC_FILTERFIND), &QLineEdit::returnPressed, &f, [&] { run(true); });
        QObject::connect(f.get<QCheckBox>(IDC_FILTERFIND_CASE), &QCheckBox::toggled, &f, [&](bool on) { g_find.caseSensitive = on; run(false); });
        f.exec();
    };
    auto doDuplicates = [&] {
        RcDialog dd("FILTERCHECKDUPLICATEDLGBOX", &d);
        auto *res = dd.get<QListWidget>(IDC_FILTERFIND_DUPLICATE);
        static int i1 = 0, i2 = 1;
        int chosen = -1;
        QObject::connect(dd.button(IDOK), &QPushButton::clicked, &dd, [&] {
            res->clear();
            res->addItem("Searching for duplicate filters");
            res->addItem("Please be patient...");
            bool found = false;
            const int n = (int)list.size();
            if (i2 <= i1) i2 = i1 + 1;
            while (i1 < n && !found) {
                while (i2 < n && !found) {
                    if (list[i1].type == list[i2].type && list[i1].capcode == list[i2].capcode && list[i1].text == list[i2].text) found = true;
                    else i2++;
                }
                if (!found) { i1++; i2 = i1 + 1; }
                dd.setText(IDC_FILTERDUP_PCT, QString("%1%").arg(n ? i1 * 100 / n : 100));
                if (auto *pb = dd.get<QProgressBar>(IDC_PROGRESS1)) pb->setValue(n ? i1 * 100 / n : 100);
                QApplication::processEvents();
            }
            res->clear();
            if (found) {
                auto describe = [&](int i, int k) {
                    QString s = QString("%1. ").arg(k);
                    const Filter &f = list[i];
                    if (!f.capcode.empty()) s += QString::fromStdString(f.capcode);
                    if (!f.text.empty()) s += (f.capcode.empty() ? "" : " - ") + QString::fromStdString(f.text);
                    if (!f.label.empty()) s += " - " + QString::fromStdString(f.label);
                    return s;
                };
                res->addItem(describe(i1, 1));
                res->addItem(describe(i2, 2));
                i2++;
            } else {
                res->addItem("No (more) duplicate filters found");
                i1 = 0;
                i2 = 1;
            }
            QApplication::beep();
        });
        QObject::connect(res, &QListWidget::itemDoubleClicked, &dd, [&](QListWidgetItem *it) {
            chosen = res->row(it) ? i2 - 1 : i1;
            dd.accept();
        });
        dd.exec();
        if (chosen >= 0 && chosen < (int)list.size()) {
            lw->clearSelection();
            lw->setCurrentRow(chosen);
            lw->item(chosen)->setSelected(true);
            lw->scrollToItem(lw->item(chosen));
        }
    };
    auto doOptions = [&] { dlg::filterOptions(w); };
    QObject::connect(add, &QPushButton::clicked, &d, doAdd);
    QObject::connect(edit, &QPushButton::clicked, &d, doEdit);
    QObject::connect(del, &QPushButton::clicked, &d, doDel);
    QObject::connect(find, &QPushButton::clicked, &d, doFind);
    QObject::connect(opts, &QPushButton::clicked, &d, doOptions);
    QObject::connect(lw, &QListWidget::itemDoubleClicked, &d, [&](QListWidgetItem *) { doEdit(); });
    static std::vector<Filter> clipboard;
    QObject::connect(lw, &QListWidget::customContextMenuRequested, &d, [&](const QPoint &pos) {
        QMenu m;
        const bool any = !selected().empty(), more = selected().size() > 1;
        auto act = [&](const QString &t, bool en, std::function<void()> fn) {
            QAction *a = m.addAction(t);
            a->setEnabled(en);
            QObject::connect(a, &QAction::triggered, &d, fn);
        };
        auto sortBy = [&](bool address) {
            auto s = selected();
            std::vector<int> idx = s.size() > 1 ? s : std::vector<int>();
            if (idx.empty()) for (size_t i = 0; i < list.size(); i++) idx.push_back((int)i);
            std::vector<Filter> part;
            for (int i : idx) part.push_back(list[i]);
            std::stable_sort(part.begin(), part.end(), [&](const Filter &a, const Filter &b) {
                return address ? a.capcode < b.capcode : QString::fromStdString(a.label).toLower() < QString::fromStdString(b.label).toLower();
            });
            for (size_t k = 0; k < idx.size(); k++) list[idx[k]] = part[k];
            reload(idx.front());
        };
        act("Sort by Address", any, [&] { sortBy(true); });
        act("Sort by Label", any, [&] { sortBy(false); });
        m.addSeparator();
        act("Select All", true, [&] { lw->selectAll(); });
        act("Copy", any, [&] { clipboard.clear(); for (int i : selected()) clipboard.push_back(list[i]); });
        act("Paste", !clipboard.empty() && any, [&] {
            auto s = selected();
            int at = s.back() + 1;
            for (const Filter &f : clipboard) list.insert(list.begin() + at++, f);
            reload(at - 1);
        });
        m.addSeparator();
        act("Add...", !more, doAdd);
        act("Edit...", any, doEdit);
        act("Delete...", any, doDel);
        m.addSeparator();
        act(more ? "Reset Hitcounters" : "Reset Hitcounter", any, [&] {
            dec.reset_hitcounters(false, selected());
            list = dec.filters();   // (the list is edited locally: re-read hit counters, keep the rest)
            reload(selected().empty() ? 0 : selected().front());
        });
        act("Find Filter", !more && !list.empty(), doFind);
        act("Check for duplicate filters", true, doDuplicates);
        act("Filter Options...", true, doOptions);
        m.exec(lw->mapToGlobal(pos));
    });
    QShortcut *sc = new QShortcut(QKeySequence("F3"), &d);
    QObject::connect(sc, &QShortcut::activated, &d, [&] { if (!list.empty()) findFilter(list, lw, -1, "", false, false); });
    QShortcut *sc2 = new QShortcut(QKeySequence("Shift+F3"), &d);
    QObject::connect(sc2, &QShortcut::activated, &d, [&] { if (!list.empty()) findFilter(list, lw, -1, "", false, true); });
    QShortcut *sc3 = new QShortcut(QKeySequence("Ctrl+F"), &d);
    QObject::connect(sc3, &QShortcut::activated, &d, doFind);
    QShortcut *sc4 = new QShortcut(QKeySequence("Delete"), &d);
    QObject::connect(sc4, &QShortcut::activated, &d, doDel);

    d.exec();
    dec.set_filters(list);
    dec.save_filters();
    return true;
}

// ---- Filter options -------------------------------------------------------------------------------------------------------------------------------
bool filterOptions(MainWindow *w)
{
    auto &dec = w->decoder();
    RcDialog d("FILTEROPTIONSDLGBOX", w);
    d.setChecked(IDC_LABELLOG, geti(dec, "LabelLog"));
    d.setChecked(IDC_LABELNEWLINE, geti(dec, "LabelNewline"));
    d.setChecked(IDC_FILTERFILEEN, geti(dec, "FilterFileEnabled", "Filter"));
    d.setChecked(IDC_FILTERFILEDATE, geti(dec, "FilterFileUseDate", "Filter"));
    d.setChecked(IDC_FILTERTONE, geti(dec, "ShowTone"));
    d.setChecked(IDC_FILTERNUMERIC, geti(dec, "ShowNumeric"));
    d.setChecked(IDC_FILTERBINARYHEX, geti(dec, "ShowBinary"));
    auto *beep = d.get<QCheckBox>(IDC_FILTERBEEP);
    beep->setTristate(true);
    beep->setCheckState(geti(dec, "FilterBeep") == 0 ? Qt::Unchecked : geti(dec, "FilterBeep") == 1 ? Qt::Checked : Qt::PartiallyChecked);
    d.setChecked(IDC_FILTERWINDOWONLY, geti(dec, "FilterWindowOnly"));
    d.setChecked(IDC_FILTERSCOLORS, geti(dec, "FilterWindowColors"));
    d.setChecked(IDC_FILTERSEXTRA, geti(dec, "FilterWindowExtra"));
    const QString cols = gets(dec, "ColFilterfile");
    for (int i = 1; i <= 7; i++) d.setChecked(IDC_FILTERLOGCOLUMN + i, cols.contains(QChar('0' + i)));
    if (dec.mode() == pdw::Mode::Mobitex) { d.setText(IDC_FILTERLOGCOLUMN + 1, "MAN"); d.setText(IDC_FILTERLOGCOLUMN + 4, "Sender"); }
    else if (dec.mode() == pdw::Mode::Acars) {
        d.setText(IDC_FILTERLOGCOLUMN + 1, "Reg.No."); d.setText(IDC_FILTERLOGCOLUMN + 4, "Msg.No.");
        d.setText(IDC_FILTERLOGCOLUMN + 5, "DBI"); d.setText(IDC_FILTERLOGCOLUMN + 6, "Mode");
    }
    d.fillCombo(IDC_FILTERDEFTYPE, kTypes);
    d.setComboIndex(IDC_FILTERDEFTYPE, geti(dec, "FilterDefaultType", "Filter"));
    d.setChecked(IDC_FILTERCMDEN, geti(dec, "FilterCmdFileEnabled", "Filter"));
    d.setText(IDC_FILTERCMDFILE, gets(dec, "FilterCmdFile", "Filter"));
    d.setText(IDC_FILTERCMDARGS, gets(dec, "FilterCmdArgs", "Filter"));
    const QString dateName = QDateTime::currentDateTime().toString("yyMMdd") + ".flt";
    auto syncBeep = [&] {
        const auto s = beep->checkState();
        d.setText(IDC_FILTERBEEP, s == Qt::Checked ? "Filter Beep / Wavfiles (enabled)" : s == Qt::Unchecked ? "Filter Beep / Wavfiles (disabled)"
                                                                                                           : "Filter Beep / Wavfiles (enabled for filtered messages)");
    };
    auto syncFile = [&] {
        const bool en = d.checked(IDC_FILTERFILEEN), date = d.checked(IDC_FILTERFILEDATE);
        d.setEnabled(IDC_FILTERFILEDATE, en);
        d.setEnabled(IDC_FILTERFILE, en && !date);
        d.setEnabled(IDC_FILTERBROWSE, en && !date);
        d.setText(IDC_FILTERFILE, en ? (date ? dateName : gets(dec, "FilterFile", "Filter")) : QString());
    };
    auto syncCmd = [&] {
        const bool en = d.checked(IDC_FILTERCMDEN);
        d.setEnabled(IDC_FILTERCMDBROWSE, en);
        d.setEnabled(IDC_FILTERCMDFILE, en);
        d.setEnabled(IDC_FILTERCMDARGS, en && !d.text(IDC_FILTERCMDFILE).isEmpty());
    };
    syncBeep(); syncFile(); syncCmd();
    QObject::connect(beep, &QCheckBox::stateChanged, &d, [syncBeep](int) { syncBeep(); });
    QObject::connect(d.get<QCheckBox>(IDC_FILTERFILEEN), &QCheckBox::toggled, &d, syncFile);
    QObject::connect(d.get<QCheckBox>(IDC_FILTERFILEDATE), &QCheckBox::toggled, &d, syncFile);
    QObject::connect(d.get<QCheckBox>(IDC_FILTERCMDEN), &QCheckBox::toggled, &d, [&](bool on) {
        if (on) QMessageBox::information(&d, "PDW Command file arguments",
                                         "Syntax :\n\n%1 : Capcode\n%2 : Time\n%3 : Date\n%4 : Mode\n%5 : Type\n%6 : Bitrate\n%7 : Message\n%8 : Label");
        syncCmd();
    });
    QObject::connect(d.get<QLineEdit>(IDC_FILTERCMDFILE), &QLineEdit::textChanged, &d, syncCmd);
    QObject::connect(d.button(IDC_FILTERBROWSE), &QPushButton::clicked, &d, [&] {
        const QString p = QFileDialog::getOpenFileName(&d, "Open Filter File...", gets(dec, "LogfilePath"), "Filter Log Files (*.flt);;All Files (*)");
        if (!p.isEmpty()) d.setText(IDC_FILTERFILE, p);
    });
    QObject::connect(d.button(IDC_FILTERCMDBROWSE), &QPushButton::clicked, &d, [&] {
        const QString p = QFileDialog::getOpenFileName(&d, "Select Command File...", w->dataDir());
        if (!p.isEmpty()) d.setText(IDC_FILTERCMDFILE, p);
    });
    for (;;) {
        if (d.exec() != QDialog::Accepted) return false;
        const bool en = d.checked(IDC_FILTERFILEEN), date = d.checked(IDC_FILTERFILEDATE);
        QString name = d.text(IDC_FILTERFILE);
        if (en && !date) {
            if (name.isEmpty()) { warn(&d, "PDW Filterfile", "You haven't entered a file name!"); continue; }
            if (!name.contains('.')) name += ".flt";
            if (name == gets(dec, "LogFile")) { warn(&d, "PDW Filterfile", QString("Filterfile can't be same as Logfile!\n( %1 )").arg(name)); continue; }
            const QString full = name.contains('/') ? name : gets(dec, "LogfilePath") + "/" + name;
            QDir().mkpath(QFileInfo(full).absolutePath());
            QFile f(full);
            if (!f.open(QIODevice::Append)) { warn(&d, "PDW Filterfile", QString("Error opening filterfile!\n( %1 )").arg(name)); continue; }
        }
        if (d.checked(IDC_FILTERCMDEN) && d.text(IDC_FILTERCMDFILE).isEmpty()) {
            warn(&d, "PDW Filter command file", "You must enter a command file!");
            continue;
        }
        sets(dec, "FilterFile", en && !date ? name : QString(), "Filter");
        QString c;
        for (int i = 1; i <= 7; i++) if (d.checked(IDC_FILTERLOGCOLUMN + i)) c += QChar('0' + i);
        sets(dec, "ColFilterfile", c);
        seti(dec, "FilterFileEnabled", en, "Filter");
        seti(dec, "FilterFileUseDate", date, "Filter");
        seti(dec, "ShowTone", d.checked(IDC_FILTERTONE));
        seti(dec, "ShowNumeric", d.checked(IDC_FILTERNUMERIC));
        seti(dec, "ShowBinary", d.checked(IDC_FILTERBINARYHEX));
        seti(dec, "FilterBeep", beep->checkState() == Qt::Checked ? 1 : beep->checkState() == Qt::Unchecked ? 0 : 2);
        seti(dec, "FilterWindowOnly", d.checked(IDC_FILTERWINDOWONLY));
        seti(dec, "FilterWindowColors", d.checked(IDC_FILTERSCOLORS));
        seti(dec, "FilterWindowExtra", d.checked(IDC_FILTERSEXTRA));
        seti(dec, "FilterCmdFileEnabled", d.checked(IDC_FILTERCMDEN), "Filter");
        sets(dec, "FilterCmdFile", d.text(IDC_FILTERCMDFILE), "Filter");
        sets(dec, "FilterCmdArgs", d.text(IDC_FILTERCMDARGS), "Filter");
        seti(dec, "LabelLog", d.checked(IDC_LABELLOG));
        seti(dec, "LabelNewline", d.checked(IDC_LABELNEWLINE));
        seti(dec, "FilterDefaultType", d.comboIndex(IDC_FILTERDEFTYPE), "Filter");
        w->settingsChanged();
        if (!w->dataDir().isEmpty()) dec.save_settings();
        return true;
    }
}

}  // namespace dlg
