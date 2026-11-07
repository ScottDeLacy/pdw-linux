#include "rcdialog.hpp"
#include <QButtonGroup>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QIntValidator>
#include <QHeaderView>
#include "generated/rc_data.h"

namespace {

// Is `flag` set in a style string like "BS_AUTOCHECKBOX | NOT WS_TABSTOP"?
bool has(const QString &style, const char *flag)
{
    for (QString p : style.split('|', Qt::SkipEmptyParts))
        if (p.trimmed() == flag) return true;
    return false;
}

bool notFlag(const QString &style, const char *flag)
{
    return style.contains(QString("NOT ") + flag);
}

}  // namespace

QFont RcDialog::dialogFont(const QString &requested, int pointSize)
{
    QFont f;
    const QStringList families = {requested, "Verdana", "DejaVu Sans", "Noto Sans", "Helvetica", "Arial"};
    const QStringList have = QFontDatabase::families();
    for (const QString &fam : families)
        if (have.contains(fam)) { f.setFamily(fam); break; }
    f.setPointSize(pointSize);
    return f;
}

RcDialog::RcDialog(const char *name, QWidget *parent) : QDialog(parent)
{
    const rc::DialogDef *def = nullptr;
    for (const rc::DialogDef *d = rc::kDialogs; d->name; d++)
        if (strcmp(d->name, name) == 0) { def = d; break; }
    if (!def) return;

    setWindowTitle(QString::fromUtf8(def->caption));
    setFont(dialogFont(def->font, def->font_size));
    setModal(true);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);

    // GetDialogBaseUnits(): average width of the alphabet / font height
    const QFontMetrics fm(font());
    const double bx = fm.horizontalAdvance("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") / 52.0;
    const double by = fm.height();
    auto X = [&](int v) { return int(v * bx * 1.1 / 4.0 + 0.5); };
    auto Y = [&](int v) { return int(v * by / 8.0 + 0.5); };

    setFixedSize(X(def->w), Y(def->h));

    QButtonGroup *radios = nullptr;
    bool lastWasRadio = false;

    for (const rc::ControlDef &c : def->controls) {
        const QString kind = c.kind, style = c.style, cls = c.cls, ex = c.exstyle;
        const QString text = QString::fromUtf8(c.text);
        QWidget *w = nullptr;
        bool isRadio = false;

        if (kind == "LTEXT" || kind == "CTEXT" || kind == "RTEXT") {
            QString t = text;                       // labels carry mnemonic markers but have no buddy
            t.replace("&&", "\x01").remove('&').replace('\x01', '&');
            auto *l = new QLabel(t, this);
            l->setTextFormat(Qt::PlainText);
            l->setAlignment((kind == "LTEXT" ? Qt::AlignLeft : kind == "CTEXT" ? Qt::AlignHCenter : Qt::AlignRight) | Qt::AlignTop);
            if (ex.contains("WS_EX_CLIENTEDGE")) {
                l->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
                l->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            }
            l->setWordWrap(false);
            w = l;
        } else if (kind == "GROUPBOX") {
            auto *g = new QGroupBox(text, this);
            g->setAttribute(Qt::WA_TransparentForMouseEvents);
            w = g;
        } else if (kind == "PUSHBUTTON" || kind == "DEFPUSHBUTTON") {
            auto *b = new QPushButton(text, this);
            b->setAutoDefault(false);
            if (kind == "DEFPUSHBUTTON") b->setDefault(true);
            if (c.id == rc::IDOK) connect(b, &QPushButton::clicked, this, &QDialog::accept);
            if (c.id == rc::IDCANCEL) connect(b, &QPushButton::clicked, this, &QDialog::reject);
            w = b;
        } else if (kind == "EDITTEXT") {
            auto *e = new QLineEdit(this);
            if (has(style, "ES_CENTER")) e->setAlignment(Qt::AlignHCenter);
            if (has(style, "ES_NUMBER")) e->setValidator(new QIntValidator(0, 999999, e));
            if (has(style, "ES_PASSWORD")) e->setEchoMode(QLineEdit::Password);
            if (has(style, "ES_READONLY")) e->setReadOnly(true);
            w = e;
        } else if (kind == "COMBOBOX") {
            auto *cb = new QComboBox(this);
            cb->setEditable(false);
            w = cb;
        } else if (kind == "LISTBOX") {
            auto *lb = new QListWidget(this);
            if (has(style, "LBS_EXTENDEDSEL")) lb->setSelectionMode(QAbstractItemView::ExtendedSelection);
            w = lb;
        } else if (kind == "CONTROL") {
            if (cls == "Button") {
                if (has(style, "BS_AUTOCHECKBOX") || has(style, "BS_CHECKBOX") || has(style, "BS_AUTO3STATE")) {
                    auto *cb = new QCheckBox(text, this);
                    if (has(style, "BS_AUTO3STATE")) cb->setTristate(true);
                    w = cb;
                } else if (has(style, "BS_AUTORADIOBUTTON") || has(style, "BS_RADIOBUTTON")) {
                    auto *rb = new QRadioButton(text, this);
                    rb->setAutoExclusive(false);
                    if (!radios || !lastWasRadio || has(style, "WS_GROUP")) radios = new QButtonGroup(this);
                    radios->setExclusive(true);
                    radios->addButton(rb);
                    w = rb;
                    isRadio = true;
                } else {
                    auto *b = new QPushButton(text, this);
                    w = b;
                }
            } else if (cls == "Static") {
                auto *l = new QLabel(text, this);
                if (has(style, "SS_BLACKFRAME")) l->setFrameStyle(QFrame::Box | QFrame::Plain);
                if (has(style, "SS_CENTERIMAGE")) l->setAlignment(Qt::AlignCenter);
                w = l;
            } else if (cls == "msctls_progress32") {
                auto *p = new QProgressBar(this);
                p->setTextVisible(false);
                w = p;
            } else if (cls == "SysListView32") {
                auto *t = new QTreeWidget(this);
                t->setRootIsDecorated(false);
                t->setAlternatingRowColors(false);
                t->setSelectionMode(QAbstractItemView::ExtendedSelection);
                t->setUniformRowHeights(true);
                if (has(style, "LVS_NOCOLUMNHEADER")) t->setHeaderHidden(true);
                w = t;
            } else {
                w = new QWidget(this);   // custom window class: the dialog code paints into it
            }
        } else {
            continue;
        }
        lastWasRadio = isRadio;
        if (!w) continue;

        // combo boxes: the resource height includes the drop-down list
        int h = c.h;
        if (kind == "COMBOBOX") h = 12;
        w->setGeometry(X(c.x), Y(c.y), X(c.w), Y(h));
        if (has(style, "WS_DISABLED")) w->setEnabled(false);
        if (notFlag(style, "WS_VISIBLE")) w->hide();
        if (c.id != rc::IDC_STATIC) ctl_.insert(c.id, w);
        w->setProperty("rcid", c.id);
    }
}

bool RcDialog::checked(int id) const
{
    if (auto *c = get<QAbstractButton>(id)) return c->isChecked();
    return false;
}

void RcDialog::setChecked(int id, bool on)
{
    if (auto *c = get<QAbstractButton>(id)) c->setChecked(on);
}

void RcDialog::setEnabled(int id, bool on)
{
    if (QWidget *w = ctl(id)) w->setEnabled(on);
}

void RcDialog::setText(int id, const QString &text)
{
    QWidget *w = ctl(id);
    if (auto *l = qobject_cast<QLabel *>(w)) l->setText(text);
    else if (auto *e = qobject_cast<QLineEdit *>(w)) e->setText(text);
    else if (auto *b = qobject_cast<QAbstractButton *>(w)) b->setText(text);
    else if (auto *g = qobject_cast<QGroupBox *>(w)) g->setTitle(text);
}

QString RcDialog::text(int id) const
{
    QWidget *w = ctl(id);
    if (auto *l = qobject_cast<QLabel *>(w)) return l->text();
    if (auto *e = qobject_cast<QLineEdit *>(w)) return e->text();
    if (auto *b = qobject_cast<QAbstractButton *>(w)) return b->text();
    return {};
}

int RcDialog::comboIndex(int id) const
{
    auto *c = get<QComboBox>(id);
    return c ? c->currentIndex() : -1;
}

void RcDialog::setComboIndex(int id, int index)
{
    if (auto *c = get<QComboBox>(id)) c->setCurrentIndex(index);
}

void RcDialog::fillCombo(int id, const QStringList &items)
{
    if (auto *c = get<QComboBox>(id)) {
        c->clear();
        c->addItems(items);
    }
}
