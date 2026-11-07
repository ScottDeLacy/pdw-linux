// Small utilities used by the imported PDW sources. Written fresh for this port
// (upstream's helper_funcs.* carries a non-GPL "do not reproduce" notice, so it
// is deliberately not used).
#pragma once
#include <functional>
#include <string>
#include "compat/windows.h"

extern char szCurrentDate[40];
extern char szCurrentTime[40];

void Get_Date_Time(void);
bool FileExists(const char *file);
void GetLocalTime(SYSTEMTIME *st);
void SetLocalTime(const SYSTEMTIME *st);

// helper_funcs replacements (path handling accepts '/' and '\\').
void GetFileName(const char *path, char *name, int size);
void GetFilePath(const char *path, char *dir, int size);
void ChangeFileExtension(char *filename, const char *ext);

// Settings overlay: values that win over pdw.ini (see pdw::Config::ini).
void pdw_ini_overlay_set(const char *section, const char *key, const char *value);
void pdw_ini_overlay_clear();
void pdw_set_sound_hook(std::function<void(const std::string &)> hook);
