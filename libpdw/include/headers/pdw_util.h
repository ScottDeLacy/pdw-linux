// Small utilities used by the imported PDW sources. Written fresh for this port
// (upstream's helper_funcs.* carries a non-GPL "do not reproduce" notice, so it
// is deliberately not used).
#pragma once
#include "compat/windows.h"

extern char szCurrentDate[40];
extern char szCurrentTime[40];

void Get_Date_Time(void);
bool FileExists(const char *file);
void GetLocalTime(SYSTEMTIME *st);
void SetLocalTime(const SYSTEMTIME *st);
