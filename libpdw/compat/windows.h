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
#include <strings.h>
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
#define wsprintf sprintf
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

#define LF_FACESIZE 32
#define FW_NORMAL 400
#define OEM_CHARSET 255
#define OUT_STROKE_PRECIS 3
#define CLIP_DEFAULT_PRECIS 0
#define DEFAULT_QUALITY 0
#define FIXED_PITCH 1
#define FF_MODERN 48
#define IDOK 1
#define IDCANCEL 2
#define MB_ICONINFORMATION 0
#define MB_ICONQUESTION 0
#define MB_ICONWARNING 0
#define MB_OKCANCEL 0
#define FILE_ATTRIBUTE_READONLY 0x1
#define FILE_ATTRIBUTE_NORMAL 0x80
#define GetRValue(c) ((BYTE)((c)&0xFF))
#define GetGValue(c) ((BYTE)(((c) >> 8) & 0xFF))
#define GetBValue(c) ((BYTE)(((c) >> 16) & 0xFF))
#define ZeroMemory(p, n) memset((p), 0, (n))
#define stricmp strcasecmp
#define strnicmp strncasecmp

struct LOGFONT {
    LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality,
        lfPitchAndFamily;
    char lfFaceName[LF_FACESIZE];
};
struct OPENFILENAME { int unused; };
struct RECT { int left, top, right, bottom; };

struct SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};

// ---- Win32 profile (INI) and file API, implemented portably in win32_shim.cpp ----
UINT GetPrivateProfileInt(LPCSTR section, LPCSTR key, INT def, LPCSTR file);
DWORD GetPrivateProfileString(LPCSTR section, LPCSTR key, LPCSTR def, char *out, DWORD size, LPCSTR file);
BOOL WritePrivateProfileString(LPCSTR section, LPCSTR key, LPCSTR value, LPCSTR file);
BOOL CopyFile(LPCSTR from, LPCSTR to, BOOL fail_if_exists);
BOOL DeleteFile(LPCSTR file);
BOOL CreateDirectory(LPCSTR path, void *security);
DWORD GetFileAttributes(LPCSTR file);
BOOL SetFileAttributes(LPCSTR file, DWORD attrs);
#define MessageBox(h, text, caption, flags) pdw_message_box((text), (caption))
int pdw_message_box(const char *text, const char *caption);

// ---- file-handle, process and sound calls used by filters/commandfile/wave playback ----
#define GENERIC_READ 0x80000000u
#define FILE_SHARE_READ 1
#define OPEN_EXISTING 3
#define SND_FILENAME 0x20000
#define SND_ASYNC 1
#define SND_NOSTOP 0x10
struct STARTUPINFO { DWORD cb; };
struct PROCESS_INFORMATION { HANDLE hProcess, hThread; };
HANDLE CreateFile(LPCSTR path, DWORD access, DWORD share, void *sec, DWORD disposition, DWORD flags, void *tmpl);
DWORD GetFileSize(HANDLE h, DWORD *high);
BOOL CloseHandle(HANDLE h);
// Runs `cmdline` without a shell (split like the Windows command line); never blocks.
BOOL CreateProcess(LPCSTR app, char *cmdline, void *pa, void *ta, BOOL inherit, DWORD flags, void *env,
                   LPCSTR dir, STARTUPINFO *si, PROCESS_INFORMATION *pi);
BOOL sndPlaySound(LPCSTR file, UINT flags);
