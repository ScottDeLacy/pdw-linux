// Builds a QDialog from a dialog definition of upstream's resource script, keeping PDW's layout
// (positions and sizes are the original dialog units, scaled by the dialog font).
#pragma once
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QTreeWidget>
#include "generated/rc_ids.h"

class RcDialog : public QDialog {
    Q_OBJECT
public:
    // `name` is the resource name, e.g. "SETUPDLGBOX".
    explicit RcDialog(const char *name, QWidget *parent = nullptr);

    QWidget *ctl(int id) const { return ctl_.value(id, nullptr); }
    template <class T>
    T *get(int id) const { return qobject_cast<T *>(ctl(id)); }

    bool checked(int id) const;
    void setChecked(int id, bool on);
    void setEnabled(int id, bool on);
    void setText(int id, const QString &text);
    QString text(int id) const;
    int comboIndex(int id) const;
    void setComboIndex(int id, int index);
    void fillCombo(int id, const QStringList &items);
    QPushButton *button(int id) const { return get<QPushButton>(id); }

    static QFont dialogFont(const QString &requested, int pointSize);

private:
    QMap<int, QWidget *> ctl_;
};
