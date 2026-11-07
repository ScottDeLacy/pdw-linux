// Renders every resource-script dialog to a PNG: `pdw --dialog-shots DIR` (use with
// QT_QPA_PLATFORM=offscreen). Used to compare the layouts with the original's screenshots.
#include <QDir>
#include "generated/rc_data.h"
#include "rcdialog.hpp"

int dialogShots(const QString &dir)
{
    QDir().mkpath(dir);
    int n = 0;
    for (const rc::DialogDef *d = rc::kDialogs; d->name; d++) {
        RcDialog dlg(d->name);
        dlg.show();
        dlg.grab().save(QDir(dir).filePath(QString("%1.png").arg(d->name)));
        n++;
    }
    return n;
}
