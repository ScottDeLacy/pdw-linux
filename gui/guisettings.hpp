// Settings that exist only in the desktop front end (input source, rtl_fm parameters). They live in
// pdw-gui.ini next to pdw.ini, so the original's own files stay untouched by them.
#pragma once
#include <QDir>
#include <QSettings>
#include <QString>
#include <memory>

inline QSettings &guiSettings(const QString &dataDir = QString())
{
    static std::unique_ptr<QSettings> s;
    static QString dir;
    if (!s || (!dataDir.isEmpty() && dataDir != dir)) {
        dir = dataDir;
        s.reset(new QSettings(dir.isEmpty() ? QString() : QDir(dir).filePath("pdw-gui.ini"), QSettings::IniFormat));
    }
    return *s;
}
