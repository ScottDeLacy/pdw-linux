// Minimal Win32 type/macro shim so the imported PDW decoder sources compile on
// Linux/macOS. Only what the imported files reference; no behaviour.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#define FAR
#define NEAR
#define PASCAL
#define CALLBACK
#define VOID void
#define MAX_PATH 4096
#define WM_USER 0x0400
#define TRUE 1
#define FALSE 0
#define TEXT(x) x
#define GWL_HINSTANCE (-6)
#define MAKELONG(a, b) ((int32_t)(((uint16_t)(a)) | (((uint32_t)((uint16_t)(b))) << 16)))
#define RGB(r, g, b) ((COLORREF)(((uint8_t)(r)) | (((uint32_t)(uint8_t)(g)) << 8) | (((uint32_t)(uint8_t)(b)) << 16)))
#define lstrcpy strcpy
#define lstrcat strcat

typedef int BOOL;
typedef int INT;
typedef unsigned int UINT;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;  // Windows DWORD is 32-bit; keep it so even though Linux long is 64-bit
typedef int32_t LONG;
typedef uint32_t COLORREF;
typedef char TCHAR;
typedef char *LPSTR;
typedef char *LPTSTR;
typedef const char *LPCSTR;
typedef const char *LPCTSTR;
typedef void *HANDLE;
typedef void *HWND;
typedef void *HINSTANCE;
typedef void *HACCEL;
typedef void *HMENU;
typedef void *HDC;
typedef void *HFONT;
typedef void *HBRUSH;
typedef void *HPEN;
typedef void *HWAVEIN;
typedef void *HWAVEOUT;
typedef intptr_t WPARAM;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
typedef void *DLGPROC;

struct LOGFONT { char lfFaceName[32]; int lfHeight; };
struct OPENFILENAME { int unused; };
struct RECT { int left, top, right, bottom; };

struct SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};
