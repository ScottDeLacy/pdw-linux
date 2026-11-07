// Table of pdw.ini settings: see tools/gen_settings_table.py.
#pragma once
#include <string>

struct SettingEntry {
    const char *section, *key;
    enum Type { Int, String, Color } type;
    void (*i)(bool set, long long &v);
    void (*s)(bool set, std::string &v);
    void (*c)(bool set, unsigned &v);   // COLORREF (0x00BBGGRR)
};
extern const SettingEntry g_setting_table[];   // terminated by key == nullptr
