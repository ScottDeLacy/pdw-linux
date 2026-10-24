// Declarations tying the extracted upstream code to the portable support layer.
#pragma once
#include "compat/windows.h"
#include "headers/pdw.h"

extern int iConvertingGroupcall, iMessageIndex;
extern BYTE message_color[MAX_STR_LEN + 1];
extern bool bMode_IDLE;
extern double dRX_Quality;
extern char szFilenameDate[16];
extern char szPath[MAX_PATH];
extern char szWindowText[6][1000];

void UpdateSigInd(int direction_flg);
bool BlockChecker(char *address, int fnu, char *message, bool reject);
void SetNewWindowText(char *text);
void Reset_ATB(void);
void CreateDateFilename(char *ext, SYSTEMTIME *yesterday);
