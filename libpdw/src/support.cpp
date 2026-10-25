// Glue between the imported PDW core and the portable libpdw API: the globals the
// Windows GUI normally owns, headless message panes, the timers PDW drives from
// WM_TIMER, and pdw::Decoder.
//
// Copyright and licence: see NOTICE. GPL-3.0-or-later.
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <stdexcept>
#include "compat/windows.h"
#include "headers/pdw.h"
#include "headers/gfx.h"
#include "headers/misc.h"
#include "headers/initapp.h"
#include "headers/decode.h"
#include "headers/sound_in.h"
#include "headers/pdw_util.h"
#include "headers/pdw_core.h"
#include "headers/language.h"
#include "headers/mailopts.h"
#include "headers/acars.h"
#include "headers/mobitex.h"
#include "headers/ermes.h"
#include "pdw/decoder.hpp"
#include "settings_table.h"

// ---- globals the Windows front end normally defines -------------------------
PROFILE Profile;
PaneStruct Pane1, Pane2;
char szWindowText[6][1000];
char szPath[MAX_PATH] = ".";
char szLogPathName[MAX_PATH], szWavePathName[MAX_PATH], szIniPathName[MAX_PATH];
char szFilterPathName[MAX_PATH], szFilterBackup[MAX_PATH];
char szFilenameDate[16];
TCHAR szShortAppName[32] = "PDW";
FILE *pLogFile = nullptr, *pFilterFile = nullptr, *pSepFilterFile = nullptr, *pStatFile = nullptr,
     *pClipboardFile = nullptr, *pFiltersFile = nullptr;
unsigned int cxChar = 1, cyChar = 1, cxCaps = 1;  // layout is in character cells
int iMaxWidth = 0;
bool bUpdateFilters = false;
bool bTrayed = false;
int nCount_Messages = 0, nCount_Groupcalls = 0, nCount_Rejected = 0, nCount_Blocked = 0;
int nCount_Missed[2] = {0, 0};
int nCount_BlockBuffer[2] = {0, 0};
double dRX_Quality = 100.0;
extern bool bEmpty_Frame;
extern bool bFlexTIME_detected;
extern std::atomic<int> nSMTPemails, nSMTPerrors, nSMTPsessions;   // mail.cpp
extern std::string g_smtp_last_error;
extern unsigned long int aMessages[1000][3];
extern int iPanePos;

// Pieces of sound_in.cpp that the audio extract leaves out (no Win32 wave API).
extern int nSamples, preamble_count[3], flex_cnt_1600, crossing, pre_threshold, sync_bit;
extern double WatchStep, clkt_hi, clkt_lo;
extern double WatchCtr;
extern long BaudRate, last_baud_rate;
extern int skipped_sc, process_acars_bit, config_index;
extern char atb_bit;

#define FINE_CLKT_HI 1.90
#define FINE_CLKT_LO 0.32

// sound_in.cpp Reset_ATB() (called when the data mode changes).
#define COURSE_CLKT_HI 2.02
#define COURSE_CLKT_LO 0.18
void Reset_ATB(void)
{
    memset(preamble_count, 0, sizeof(preamble_count));
    nSamples = 0;
    flex_cnt_1600 = 0;
    WatchCtr = -1;
    atb_bit = low_audio;
    config_index = INDEX1600;
    cross_over = 0;
    skipped_sc = 0;
    process_acars_bit = 0;

    if (Profile.monitor_mobitex) {
        clkt_hi = COURSE_CLKT_HI;
        clkt_lo = COURSE_CLKT_LO;
        BaudRate = mb.bitrate;
        last_baud_rate = mb.bitrate;
    } else {
        clkt_hi = FINE_CLKT_HI;
        clkt_lo = FINE_CLKT_LO;
        BaudRate = 1600;
        last_baud_rate = 1600;
    }

    // WatchStep is how often to check for bit in buffer
    WatchStep = (double)Profile.audioSampleRate / (double)BaudRate;
}

// ---- utilities (written for this port) --------------------------------------
static long g_clock_offset_s = 0;  // FlexTIME: shift of the program clock, not the OS clock
static int64_t g_clock_start = 0;  // != 0: the clock follows the audio (see Config::clock_start)
static double g_audio_seconds = 0;

static void clock_now(time_t &sec, int &msec)
{
    if (g_clock_start) {
        double t = (double)g_clock_start + g_audio_seconds;
        sec = (time_t)t;
        msec = (int)((t - (double)sec) * 1000);
    } else {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        sec = tv.tv_sec;
        msec = (int)(tv.tv_usec / 1000);
    }
    sec += g_clock_offset_s;
}

static time_t now_adjusted()
{
    time_t s;
    int ms;
    clock_now(s, ms);
    return s;
}

void GetLocalTime(SYSTEMTIME *st)
{
    time_t t;
    int msec;
    clock_now(t, msec);
    struct tm tmv;
    localtime_r(&t, &tmv);
    st->wYear = tmv.tm_year + 1900;
    st->wMonth = tmv.tm_mon + 1;
    st->wDayOfWeek = tmv.tm_wday;
    st->wDay = tmv.tm_mday;
    st->wHour = tmv.tm_hour;
    st->wMinute = tmv.tm_min;
    st->wSecond = tmv.tm_sec;
    st->wMilliseconds = msec;
}

// Upstream sets the Windows system clock from FLEX time broadcasts. We keep an
// offset instead: no root needed and the host clock is never touched.
void SetLocalTime(const SYSTEMTIME *st)
{
    struct tm tmv {};
    tmv.tm_year = st->wYear - 1900;
    tmv.tm_mon = st->wMonth - 1;
    tmv.tm_mday = st->wDay;
    tmv.tm_hour = st->wHour;
    tmv.tm_min = st->wMinute;
    tmv.tm_sec = st->wSecond;
    tmv.tm_isdst = -1;
    time_t target = mktime(&tmv);
    if (target != (time_t)-1) {
        time_t cur;
        int ms;
        g_clock_offset_s = 0;
        clock_now(cur, ms);
        g_clock_offset_s = (long)(target - cur);
    }
}

bool FileExists(const char *file)
{
    struct stat sb;
    return stat(file, &sb) == 0;
}

void Get_Date_Time(void)
{
    time_t t = now_adjusted();
    struct tm tmv;
    localtime_r(&t, &tmv);
    switch (Profile.DateFormat) {
        default:
        case 0: strftime(szCurrentDate, sizeof szCurrentDate, "%d-%m-%y", &tmv); break;
        case 1: strftime(szCurrentDate, sizeof szCurrentDate, "%m-%d-%y", &tmv); break;
        case 2: strftime(szCurrentDate, sizeof szCurrentDate, "%y-%m-%d", &tmv); break;
    }
    strftime(szCurrentTime, sizeof szCurrentTime, "%H:%M:%S", &tmv);
}

static const char *last_sep(const char *p)
{
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    return a > b ? a : b;
}

void GetFileName(const char *path, char *name, int size)
{
    const char *s = last_sep(path);
    snprintf(name, size, "%s", s ? s + 1 : path);
}

void GetFilePath(const char *path, char *dir, int size)
{
    const char *s = last_sep(path);
    if (!s) { dir[0] = 0; return; }
    snprintf(dir, size, "%.*s", (int)(s - path), path);
}

void ChangeFileExtension(char *filename, const char *ext)
{
    char *dot = strrchr(filename, '.');
    const char *s = last_sep(filename);
    if (dot && (!s || dot > s)) *dot = 0;
    strcat(filename, ext);
}

// Settings files written by Windows PDW hold backslash paths (and drive letters).
static void native_path(char *p)
{
    if (isalpha((unsigned char)p[0]) && p[1] == ':') {  // "C:\PDW\Logfiles": not meaningful here
        p[0] = 0;
        return;
    }
    for (; *p; p++)
        if (*p == '\\') *p = '/';
}

// ---- system tray: not part of the headless core ------------------------------------
static pdw::Decoder::TrayCallback g_on_tray;
void SystemTrayWindow(bool hide) { if (g_on_tray) g_on_tray(hide); }

// ---- application hooks ----------------------------------------------------------
static pdw::Decoder::MessageCallback g_on_message;
static pdw::Decoder::NoticeCallback g_on_notice;
static pdw::Decoder::LineCallback g_on_line;
static pdw::Decoder::StatusCallback g_on_status;
static std::string g_notice;

// The signal indicator needle (sigind.cpp UpdateSigInd() without the drawing): moves right while a
// signal is decoded and left in noise; the front end draws position 0..20.
static int g_si_index = 0, g_si_low_hover = 0, g_si_hi_hover = 0;
void UpdateSigInd(int direction)
{
    const int kMax = 20;
    if (direction) {
        g_si_low_hover = 0;
        g_si_index ? g_si_index += 2 : g_si_index++;
        if (g_si_index > kMax) { g_si_hi_hover++; g_si_index = kMax; }
    } else {
        if (g_si_low_hover > 1) g_si_low_hover = 0;
        g_si_hi_hover = 0;
        g_si_index--;
        if (g_si_index < 0) { g_si_low_hover++; g_si_index = 0; }
    }
}

void SetNewWindowText(char *text)
{
    std::string s = szWindowText[0];
    if (text[0]) s += text;
    else {
        for (int i = 1; i < 6; i++)
            if (szWindowText[i][0]) { s += " - "; s += szWindowText[i]; }
        szWindowText[5][0] = 0;
    }
    if (g_on_status) g_on_status(s);
}

static void flush_notice()
{
    if (!g_notice.empty()) {
        if (g_on_notice) g_on_notice(g_notice);
        g_notice.clear();
    }
}

// PDW text is Windows-1252; only 0xBB (linefeed marker) and 0xB7 (NUL marker, Mobitex) survive
// display_show_char(). Latin-1 -> UTF-8, repeating the colour for both bytes of a 2-byte char.
static void to_utf8(const unsigned char *s, const unsigned char *col, std::string &text, std::vector<uint8_t> &colors)
{
    for (int i = 0; s[i]; i++) {
        unsigned char c = s[i], k = col ? col[i] : 0;
        if (c < 0x80) { text.push_back((char)c); colors.push_back(k); }
        else {
            text.push_back((char)(0xC0 | (c >> 6)));
            text.push_back((char)(0x80 | (c & 0x3F)));
            colors.push_back(k);
            colors.push_back(k);
        }
    }
}

// Replaces the GDI part of Misc.cpp display_line(): the buffer handling is the original's,
// but a finished line is handed to the application instead of being painted.
void display_line(PaneStruct *pane)
{
    unsigned int xx;
    char *pchar = pane->buff_char;
    BYTE *pcolor = pane->buff_color;

    pchar[pane->Bottom * (LINE_SIZE + 1) + pane->currentPos] = 0;
    pcolor[pane->Bottom * (LINE_SIZE + 1) + pane->currentPos] = COLOR_UNUSED;

    if (g_on_line) {
        std::string text;
        std::vector<uint8_t> colors;
        to_utf8((const unsigned char *)&pchar[pane->Bottom * (LINE_SIZE + 1)],
                &pcolor[pane->Bottom * (LINE_SIZE + 1)], text, colors);
        g_on_line(pane == &Pane2 ? 1 : 0, text, colors);
    }

    pane->currentPos = 0;
    pane->Bottom++;

    // No space left in the pane's buffer: drop the first line.
    if (pane->Bottom == pane->buff_lines) {
        pane->Bottom--;
        for (xx = 0; xx < pane->Bottom; xx++) {
            memcpy(&pchar[xx * (LINE_SIZE + 1)], &pchar[(xx + 1) * (LINE_SIZE + 1)], LINE_SIZE + 1);
            memcpy(&pcolor[xx * (LINE_SIZE + 1)], &pcolor[(xx + 1) * (LINE_SIZE + 1)], LINE_SIZE + 1);
        }
        pchar[xx * (LINE_SIZE + 1) + 0] = 0;
        pcolor[xx * (LINE_SIZE + 1) + 0] = COLOR_UNUSED;
    }
    iPanePos = 0;
}

// Called from ShowMessage() (see the EMITSED hook in tools/import_upstream.sh) once a message has
// been accepted and drawn: report it, with what the filters made of it, to the application.
void pdw_message_shown(bool monitor, bool filtered, bool monitor_only, int match, const char *label)
{
    if (!g_on_message) return;
    pdw::Message m;
    m.capcode = Current_MSG[MSG_CAPCODE];
    m.time = Current_MSG[MSG_TIME];
    m.date = Current_MSG[MSG_DATE];
    m.mode = Current_MSG[MSG_MODE];
    m.type = Current_MSG[MSG_TYPE];
    m.bitrate = Current_MSG[MSG_BITRATE];
    to_utf8(message_buffer, message_color, m.text, m.colors);
    m.in_monitor = monitor;
    m.in_filter = filtered;
    m.monitor_only = monitor_only;
    m.filter = match;
    m.label = label ? label : "";
    g_on_message(m);
}

// ---- pdw::Decoder -------------------------------------------------------------
namespace pdw {

namespace {
constexpr size_t kBufferSamples = 8192;  // PDW's SIZEOF_AUDIOBUFFER
constexpr size_t kTickSamples = 4410;    // PDW's 100 ms decode timer at 44.1 kHz
bool g_instance_alive = false;

void init_pane(PaneStruct &p, int lines)
{
    memset(&p, 0, sizeof p);
    p.buff_lines = lines;
    size_t sz = (size_t)(lines + 1) * (LINE_SIZE + 1);
    p.buff_char = (char *)calloc(sz, 1);
    p.buff_color = (BYTE *)calloc(sz, 1);
    p.currentColor = COLOR_UNUSED;
    p.cyLines = lines;
}

// Defaults set at the top of upstream WinMain() (PDW.cpp), before pdw.ini is read.
void winmain_defaults()
{
    memset(&Profile, 0, sizeof(Profile));
    Profile.filters.clear();
    Profile.LabelLog = 1; Profile.LabelNewline = 1;
    Profile.ColLogfile[0] = Profile.ColFilterfile[0] = 0;
    Profile.Linefeed = 1; Profile.Separator = 1; Profile.MonthNumber = 1;
    Profile.DateFormat = 0; Profile.BlockDuplicate = 0;
    Profile.FilterWindowColors = 1; Profile.FilterWindowExtra = 1;
    Profile.xSize = 593; Profile.ySize = 442;
    Profile.showtone = Profile.shownumeric = Profile.showmisc = 1;
    Profile.confirmExit = 1;
    Profile.decodepocsag = Profile.decodeflex = 1;
    Profile.showinstr = 1; Profile.convert_si = 1;
    Profile.pocsag_512 = Profile.pocsag_1200 = Profile.pocsag_2400 = 1;
    Profile.flex_1600 = Profile.flex_3200 = Profile.flex_6400 = 1;
    Profile.comPort = 2; Profile.comPortAddr = 0x2F8; Profile.comPortIRQ = 3;
    Profile.percent = 65;
    Profile.fontInfo.lfHeight = -11;
    Profile.fontInfo.lfWeight = FW_NORMAL;
    Profile.fontInfo.lfCharSet = OEM_CHARSET;
    Profile.fontInfo.lfOutPrecision = OUT_STROKE_PRECIS;
    Profile.fontInfo.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    Profile.fontInfo.lfQuality = DEFAULT_QUALITY;
    Profile.fontInfo.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    strcpy(Profile.fontInfo.lfFaceName, "Courier New");
    Profile.logfile_enabled = 1; Profile.logfile_use_date = 1;
    Profile.maximize_flg = 1;
    Profile.filterfile_enabled = 1; Profile.filterfile_use_date = 1;

    auto rgb = [](int i) { return RGB(rgbColor[i][0], rgbColor[i][1], rgbColor[i][2]); };
    Profile.color_background = rgb(BLACK);
    Profile.color_address = rgb(WHITE);
    Profile.color_modetypebit = rgb(RED);
    Profile.color_timestamp = rgb(LTBLUE);
    Profile.color_numeric = rgb(RED);
    Profile.color_message = rgb(LTCYAN);
    Profile.color_misc = rgb(BROWN);
    Profile.color_biterrors = rgb(LTGRAY);
    Profile.color_filtermatch = rgb(LTGREEN);
    Profile.color_instructions = rgb(LTCYAN);
    Profile.color_ac_message_nr = rgb(WHITE);
    Profile.color_ac_dbi = rgb(RED);
    Profile.color_mb_sender = rgb(RED);
    const int lab[] = {LTBLUE, YELLOW, RED, -1, LTBLUE, CYAN, WHITE, LTGREEN, LTGRAY,
                       BROWN, LTCYAN, -2, -3, -4, -5, -6, -7};
    const COLORREF special[] = {RGB(0xFF, 0xAA, 0x00), RGB(0x00, 0x33, 0x99), RGB(0xFF, 0x00, 0xFF),
                                RGB(0x33, 0xCC, 0x99), RGB(0xFF, 0x99, 0xCC), RGB(0x99, 0xFF, 0xFF),
                                RGB(0x99, 0xFF, 0xCC)};
    int sp = 0;
    for (int i = 0; i < 17; i++)
        Profile.color_filterlabel[i] = lab[i] >= 0 ? rgb(lab[i]) : special[sp++];

    Profile.pane1_size = 1000;  // PDW's PANE1_SIZE
    Profile.pane2_size = 200;   // PANE2_SIZE
    Profile.ScrollSpeed = 1;
    Profile.audioEnabled = 1;
    Profile.audioSampleRate = kSampleRate;
    Profile.audioConfig = 5;
    Profile.monitor_paging = true;
}

// The Config convenience flags become pdw.ini overlay entries (only where they leave PDW's default).
void apply_config_overlay(const Config &c)
{
    pdw_ini_overlay_clear();
    auto flag = [](const char *key, bool v, bool dflt) {
        if (v != dflt) pdw_ini_overlay_set("PDW", key, v ? "1" : "0");
    };
    flag("DecodePocsag", c.decode_pocsag, true);
    flag("DecodeFlex", c.decode_flex, true);
    flag("Pocsag512", c.pocsag_512, true);
    flag("Pocsag1200", c.pocsag_1200, true);
    flag("Pocsag2400", c.pocsag_2400, true);
    flag("Flex1600", c.flex_1600, true);
    flag("Flex3200", c.flex_3200, true);
    flag("Flex6400", c.flex_6400, true);
    flag("ShowTone", c.show_tone, true);
    flag("ShowNumeric", c.show_numeric, true);
    flag("ShowBinary", c.show_misc, true);
    flag("ShowInstr", c.show_instr, true);
    flag("ConvertInstr", c.convert_si, true);
    flag("PocsagFNU", c.pocsag_function_numeric, false);
    flag("PocsagShowBoth", c.pocsag_show_both, false);
    flag("InvertData", c.invert, false);
    flag("FlexTIME", c.flex_time, false);
    if (c.audio_config != 5) pdw_ini_overlay_set("PDW", "AudioConfiguration", std::to_string(c.audio_config).c_str());
    for (auto &kv : c.ini) pdw_ini_overlay_set("PDW", kv.first.c_str(), kv.second.c_str());
}

std::string make_scratch_dir()
{
    char tmpl[] = "/tmp/pdw-XXXXXX";
    return mkdtemp(tmpl) ? tmpl : ".";
}

void remove_tree_files(const std::string &dir)
{
    for (const char *f : {"filters.ini", "filters.bak", "missed-groupcalls.txt", "blocked.txt"})
        unlink((dir + "/" + f).c_str());
    rmdir(dir.c_str());
}
}  // namespace

struct Decoder::Impl {
    std::vector<uint8_t> pending;             // samples of the buffer being filled
    std::vector<std::vector<uint8_t>> ready;  // completed buffers awaiting the next tick
    size_t total = 0, next_tick = kTickSamples, next_second = kSampleRate, next_minute = 60 * (size_t)kSampleRate;
    std::string scratch;                      // temp dir when Config::dir is empty
    bool has_dir = false;

    // Process_ReadyBuffers() from sound_in.cpp, POCSAG/FLEX path. PDW calls it from every
    // WM_TIMER, i.e. the 100 ms timer and also the 1 s and 60 s timers.
    void process_ready()
    {
        if (flex_timer) {  // dropping out of FLEX mode: reset and start over
            bMode_IDLE = false;
            flex_timer--;
            if (flex_timer == 0) {
                bMode_IDLE = true;
                if (!pocbit) {  // don't reset if POCSAG signal follows a FLEX signal
                    BaudRate = 1600;
                    config_index = INDEX1600;
                    display_showmo(MODE_IDLE);
                }
            }
        }
        else if (mb.timer) {  // dropped out of Mobitex mode
            mb.timer--;
            if (mb.timer == 0) display_showmo(MODE_IDLE);
        }
        check_save_data();
        for (auto &buf : ready) {
            char *p = reinterpret_cast<char *>(buf.data());
            long n = (long)buf.size();
            if (Profile.monitor_paging) Audio_To_Bits(p, n);
            else if (Profile.monitor_acars) ACARS_To_Bits(p, n);
            else if (Profile.monitor_mobitex) MOBITEX_To_Bits(p, n);
            // ERMES: upstream decodes it from the serial interface only (its audio path is disabled)
        }
        ready.clear();
        flush_notice();
    }

    // SECOND_TIMER in PDWWndProc. Runs on audio time so decoding a file behaves like live.
    void on_second()
    {
        iSecondsElapsed = (unsigned long)(total / kSampleRate);
        int block_timer = (Profile.BlockDuplicate >> 4) * 60;
        if (block_timer) {
            while (aMessages[0][0] && (iSecondsElapsed > (aMessages[0][1] + block_timer))) {
                memmove(aMessages[0], aMessages[1], sizeof(aMessages) - sizeof(aMessages[0]));
                memset(aMessages[999], 0, sizeof(aMessages[999]));
                nCount_BlockBuffer[0]--;
            }
        }
    }

    // MINUTE_TIMER: persist filter hit counters while idle.
    void on_minute()
    {
        if (bMode_IDLE || bEmpty_Frame || Profile.monitor_acars || Profile.monitor_mobitex) {
            if (bUpdateFilters && has_dir) {
                WriteFilters(&Profile, 0);
                bUpdateFilters = false;
            }
        }
    }

    void push(const uint8_t *s, size_t n)
    {
        while (n) {
            size_t take = std::min(n, kBufferSamples - pending.size());
            pending.insert(pending.end(), s, s + take);
            s += take;
            n -= take;
            total += take;
            g_audio_seconds = (double)total / kSampleRate;
            if (pending.size() == kBufferSamples) {
                ready.push_back(std::move(pending));
                pending.clear();
            }
            while (total >= next_tick) {
                process_ready();
                next_tick += kTickSamples;
            }
            while (total >= next_second) {
                process_ready();
                on_second();
                next_second += kSampleRate;
            }
            while (total >= next_minute) {
                process_ready();
                on_minute();
                next_minute += 60 * (size_t)kSampleRate;
            }
        }
    }
};

Decoder::Decoder(const Config &cfg) : impl_(new Impl), cfg_(cfg)
{
    if (g_instance_alive) {
        delete impl_;
        throw std::runtime_error("pdw::Decoder: only one instance may exist (global upstream state)");
    }
    g_instance_alive = true;

    // ---- file layout, as upstream InitApplication() ----
    g_clock_start = cfg.clock_start;
    g_audio_seconds = 0;
    g_clock_offset_s = 0;
    impl_->has_dir = !cfg.dir.empty();
    std::string dir = impl_->has_dir ? cfg.dir : (impl_->scratch = make_scratch_dir());
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    if (impl_->has_dir) mkdir(dir.c_str(), 0777);
    snprintf(szPath, sizeof szPath, "%s", dir.c_str());
    snprintf(szLogPathName, sizeof szLogPathName, "%s/Logfiles", szPath);
    snprintf(szWavePathName, sizeof szWavePathName, "%s/Wavfiles", szPath);
    snprintf(szIniPathName, sizeof szIniPathName, "%s/pdw.ini", szPath);
    snprintf(szFilterPathName, sizeof szFilterPathName, "%s/filters.ini", szPath);
    snprintf(szFilterBackup, sizeof szFilterBackup, "%s/filters.bak", szPath);
    if (impl_->has_dir) CreateDirectory(szWavePathName, nullptr);
    if (!cfg.filters_file.empty()) {
        if (!FileExists(cfg.filters_file.c_str())) {
            g_instance_alive = false;
            delete impl_;
            throw std::runtime_error("filters file not found: " + cfg.filters_file);
        }
        snprintf(szFilterPathName, sizeof szFilterPathName, "%s", cfg.filters_file.c_str());
    }
    if (!FileExists(szFilterPathName)) {  // a fresh install ships an empty filter list
        if (FILE *f = fopen(szFilterPathName, "w")) {
            fputs("[Filter]\n\nFilterCount=0\n\n", f);
            fclose(f);
        }
    }

    // ---- settings: WinMain defaults, then pdw.ini, then the caller's overrides ----
    winmain_defaults();
    if (!impl_->has_dir) {  // nothing is written to disk unless the caller gave a directory
        Profile.logfile_enabled = Profile.filterfile_enabled = Profile.stat_file_enabled = 0;
    }
    apply_config_overlay(cfg);
    cxChar = 1;
    memset(&Pane1, 0, sizeof Pane1);
    memset(&Pane2, 0, sizeof Pane2);
    GetPrivateProfileSettings(szShortAppName, szIniPathName, &Profile);
    if (!impl_->has_dir) Profile.logfile_enabled = Profile.filterfile_enabled = Profile.stat_file_enabled = 0;
    native_path(Profile.LogfilePath);
    native_path(Profile.logfile);
    native_path(Profile.filterfile);
    native_path(Profile.stat_file);
    if (!Profile.LogfilePath[0]) snprintf(Profile.LogfilePath, sizeof Profile.LogfilePath, "%s", szLogPathName);
    for (auto &f : Profile.filters)
        for (int i = 0; i < 3; i++) native_path(f.sep_filterfile[i]);
    Profile.audioSampleRate = kSampleRate;  // the resampler always delivers 44.1 kHz
    if (Profile.pane1_size < 10) Profile.pane1_size = 1000;
    if (Profile.pane2_size < 10) Profile.pane2_size = 200;

    // ---- panes and geometry (character cells; NewLinePoint is the wrap column) ----
    init_pane(Pane1, Profile.pane1_size);
    init_pane(Pane2, Profile.pane2_size);
    NewLinePoint = 120;
    SetMessageItemPositionsWidth();
    snprintf(szWindowText[0], sizeof szWindowText[0], " PDW");

    // ---- decoder state, as upstream WM_CREATE ----
    setupecc();
    phase_A.ppp = 'A'; phase_B.ppp = 'B'; phase_C.ppp = 'C'; phase_D.ppp = 'D';
    for (int i = 0; i < 64; i++) rcver[i] = 0.0;
    rcv_clkt = rcv_clkt_hi;
    ct1600 = 1.0 / (1600.0 * 838.8e-9);
    ct3200 = 1.0 / (3200.0 * 838.8e-9);
    ct_bit = ct1600;
    for (int i = 0; i < NUM_STAT; i++)
        hourly_stat[i][0] = hourly_stat[i][1] = hourly_char[i][0] = hourly_char[i][1] =
            daily_stat[i][0] = daily_stat[i][1] = daily_char[i][0] = daily_char[i][1] = 0;
    GetLocalTime(&statTime);
    prev_statTime = statTime;
    stat_timer = 100 - ((statTime.wSecond % 10) * 10);

    low_audio = Profile.invert ? DEFAULT_HI_AUDIO : DEFAULT_LO_AUDIO;
    high_audio = Profile.invert ? DEFAULT_LO_AUDIO : DEFAULT_HI_AUDIO;
    sync_bit = low_audio;
    SetAudioConfig(Profile.audioConfig);
    Reset_ATB();

    // language.df: from the PDW folder, else from the shipped data files.
    {
        char saved[MAX_PATH];
        snprintf(saved, sizeof saved, "%s", szPath);
        if (!impl_->has_dir || !FileExists((dir + "/language.df").c_str())) {
            if (!cfg.resource_dir.empty()) snprintf(szPath, sizeof szPath, "%s", cfg.resource_dir.c_str());
        }
        read_language_tables();
        snprintf(szPath, sizeof szPath, "%s", saved);
    }
    set_language(Profile.lang_mi_index);
    acars.read_data();   // optional ACARS label/aircraft/airline tables from the PDW folder
}

Decoder::~Decoder()
{
    g_on_message = nullptr;
    g_on_notice = nullptr;
    g_on_line = nullptr;
    g_on_status = nullptr;
    g_on_tray = nullptr;
    pdw_set_sound_hook(nullptr);
    free_lang_tables();
    acars.free_data();
    MailInit(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0);   // stop the mail thread
    free(Pane1.buff_char); free(Pane1.buff_color);
    free(Pane2.buff_char); free(Pane2.buff_color);
    Pane1.buff_char = Pane2.buff_char = nullptr;
    Pane1.buff_color = Pane2.buff_color = nullptr;
    if (!impl_->scratch.empty()) remove_tree_files(impl_->scratch);
    pdw_ini_overlay_clear();
    delete impl_;
    g_instance_alive = false;
}

void Decoder::on_message(MessageCallback cb) { g_on_message = std::move(cb); }
void Decoder::on_notice(NoticeCallback cb) { g_on_notice = std::move(cb); }
void Decoder::on_line(LineCallback cb) { g_on_line = std::move(cb); }
void Decoder::on_status(StatusCallback cb) { g_on_status = std::move(cb); }
void Decoder::on_sound(SoundCallback cb) { pdw_set_sound_hook(std::move(cb)); }

void Decoder::feed_u8(const uint8_t *s, size_t n) { impl_->push(s, n); }

void Decoder::feed_s16(const int16_t *s, size_t n, float gain)
{
    std::vector<uint8_t> tmp(n);
    for (size_t i = 0; i < n; i++) {
        float v = s[i] * gain / 256.0f + 128.0f;
        tmp[i] = (uint8_t)std::max(0.0f, std::min(255.0f, std::round(v)));
    }
    impl_->push(tmp.data(), n);
}

void Decoder::finish()
{
    // Pad out the final partial buffer with silence so it is decoded, then let the
    // timer run so pending state (open POCSAG message) is flushed.
    if (!impl_->pending.empty()) {
        std::vector<uint8_t> pad(kBufferSamples - impl_->pending.size(), 128);
        impl_->push(pad.data(), pad.size());
    }
    std::vector<uint8_t> tail(kTickSamples * 2, 128);
    impl_->push(tail.data(), tail.size());
    flush_notice();
    LogFileHandling(0, nullptr, 1 /* CLOSE_FILES */);
    if (Profile.SMTP) MailFlush(15000);
    if (bUpdateFilters && impl_->has_dir) {  // as on exit in upstream
        WriteFilters(&Profile, 0);
        bUpdateFilters = false;
    }
}

std::vector<std::string> Decoder::languages() const
{
    std::vector<std::string> v;
    for (int i = 0; i < lang_cnt; i++) v.push_back(lang_name[i]);
    return v;
}

// As upstream set_menu_items(): which table remaps characters, and whether text is reversed.
void Decoder::set_language(int index)
{
    Profile.lang_mi_index = (index > lang_cnt || index < 0) ? 0 : index;
    Profile.reverse_msg = FALSE;
    Profile.lang_tbl_index = 0;
    if (Profile.lang_mi_index) {
        Profile.lang_tbl_index = Profile.lang_mi_index - 1;
        Profile.reverse_msg = lang_rev[Profile.lang_tbl_index];
    }
}

bool Decoder::save_settings()
{
    if (!impl_->has_dir) return false;
    WriteSettings();
    WriteFilters(&Profile, 0);
    return true;
}

// Mirrors GetColorRGB() in upstream PDW.cpp; COLORREF is 0x00BBGGRR, we return 0xRRGGBB.
uint32_t Decoder::color_rgb(uint8_t c) const
{
    COLORREF v = Profile.color_message;
    switch (c) {
        case COLOR_BACKGROUND: v = Profile.color_background; break;
        case COLOR_ADDRESS: v = Profile.color_address; break;
        case COLOR_MODETYPEBIT: v = Profile.color_modetypebit; break;
        case COLOR_TIMESTAMP: v = Profile.color_timestamp; break;
        case COLOR_NUMERIC: v = Profile.color_numeric; break;
        case COLOR_MESSAGE: v = Profile.color_message; break;
        case COLOR_MISC: v = Profile.color_misc; break;
        case COLOR_BITERRORS: v = Profile.color_biterrors; break;
        case COLOR_INSTRUCTIONS: v = Profile.color_instructions; break;
        case COLOR_AC_MESSAGE_NR: v = Profile.color_ac_message_nr; break;
        case COLOR_AC_DBI: v = Profile.color_ac_dbi; break;
        case COLOR_MB_SENDER: v = Profile.color_mb_sender; break;
        case COLOR_FILTERMATCH: v = Profile.color_filtermatch; break;
        default:
            if (c >= COLOR_FILTERLABEL && c <= COLOR_FILTERLABEL + 16) v = Profile.color_filterlabel[c - COLOR_FILTERLABEL];
            break;
    }
    return ((uint32_t)GetRValue(v) << 16) | ((uint32_t)GetGValue(v) << 8) | GetBValue(v);
}

Counters Decoder::counters() const
{
    Counters c;
    c.messages = nCount_Messages; c.groupcalls = nCount_Groupcalls;
    c.rejected = nCount_Rejected; c.blocked = nCount_Blocked;
    c.missed_groupcalls = nCount_Missed[0]; c.missed_groupcall_messages = nCount_Missed[1];
    c.block_buffer = nCount_BlockBuffer[0]; c.block_buffer_peak = nCount_BlockBuffer[1];
    c.smtp_sessions = nSMTPsessions; c.smtp_mails = nSMTPemails; c.smtp_errors = nSMTPerrors;
    c.smtp_last_error = g_smtp_last_error;
    c.rx_quality = dRX_Quality;
    for (int i = 0; i < 9; i++)
        for (int k = 0; k < 2; k++) {
            c.hourly_msgs[i][k] = hourly_stat[i][k]; c.hourly_chars[i][k] = hourly_char[i][k];
            c.daily_msgs[i][k] = daily_stat[i][k]; c.daily_chars[i][k] = daily_char[i][k];
        }
    return c;
}

// ---- settings by name ------------------------------------------------------------------------
static const SettingEntry *find_setting(const std::string &key, const std::string &section)
{
    for (const SettingEntry *e = g_setting_table; e->key; e++)
        if (strcasecmp(e->key, key.c_str()) == 0 && strcasecmp(e->section, section.c_str()) == 0) return e;
    return nullptr;
}

bool Decoder::get_setting(const std::string &key, std::string &out, const std::string &section) const
{
    const SettingEntry *e = find_setting(key, section);
    if (!e) return false;
    if (e->type == SettingEntry::Int) {
        long long v = 0;
        e->i(false, v);
        out = std::to_string(v);
    } else if (e->type == SettingEntry::String) {
        e->s(false, out);
    } else {
        unsigned c = 0;
        e->c(false, c);
        out = std::to_string(GetRValue(c)) + "," + std::to_string(GetGValue(c)) + "," + std::to_string(GetBValue(c));
    }
    return true;
}

std::vector<std::tuple<std::string, std::string, std::string>> Decoder::all_settings() const
{
    std::vector<std::tuple<std::string, std::string, std::string>> v;
    for (const SettingEntry *e = g_setting_table; e->key; e++) {
        std::string val;
        get_setting(e->key, val, e->section);
        v.emplace_back(e->section, e->key, val);
    }
    return v;
}

long long Decoder::get_int(const std::string &key, const std::string &section) const
{
    std::string v;
    return get_setting(key, v, section) ? atoll(v.c_str()) : 0;
}

bool Decoder::set_setting(const std::string &key, const std::string &value, const std::string &section)
{
    const SettingEntry *e = find_setting(key, section);
    if (!e) return false;
    if (e->type == SettingEntry::Int) {
        long long v = atoll(value.c_str());
        e->i(true, v);
    } else if (e->type == SettingEntry::String) {
        std::string v = value;
        e->s(true, v);
    } else {
        int r, g, b;
        if (sscanf(value.c_str(), "%d,%d,%d", &r, &g, &b) != 3) return false;
        unsigned c = RGB(r, g, b);
        e->c(true, c);
    }
    return true;
}

bool Decoder::set_int(const std::string &key, long long value, const std::string &section)
{
    return set_setting(key, std::to_string(value), section);
}

void Decoder::apply_settings()
{
    Profile.audioSampleRate = kSampleRate;
    SetAudioConfig(Profile.audioConfig);
    mb.frsync_rev = ~mb.frsync;
    mb.bitsync_rev = ~mb.bitsync;
    low_audio = Profile.invert ? DEFAULT_HI_AUDIO : DEFAULT_LO_AUDIO;
    high_audio = Profile.invert ? DEFAULT_LO_AUDIO : DEFAULT_HI_AUDIO;
    set_language(Profile.lang_mi_index);
    SetMessageItemPositionsWidth();
    // pane scrollback sizes
    if (Profile.pane1_size < 10) Profile.pane1_size = 10;
    if (Profile.pane2_size < 10) Profile.pane2_size = 10;
    if ((unsigned)Profile.pane1_size != Pane1.buff_lines) { free(Pane1.buff_char); free(Pane1.buff_color); init_pane(Pane1, Profile.pane1_size); }
    if ((unsigned)Profile.pane2_size != Pane2.buff_lines) { free(Pane2.buff_char); free(Pane2.buff_color); init_pane(Pane2, Profile.pane2_size); }
    MailInit(Profile.szMailHost, Profile.szMailHeloDomain, Profile.szMailFrom, Profile.szMailTo, Profile.szMailUser,
             Profile.szMailPassword, Profile.iMailPort, Profile.nMailOptions);
}

Mode Decoder::mode() const
{
    if (Profile.monitor_acars) return Mode::Acars;
    if (Profile.monitor_mobitex) return Mode::Mobitex;
    if (Profile.monitor_ermes) return Mode::Ermes;
    return Mode::Paging;
}

// ChangeDataMode() from upstream PDW.cpp.
void Decoder::set_mode(Mode m)
{
    Profile.monitor_paging = Profile.monitor_acars = Profile.monitor_mobitex = Profile.monitor_ermes = false;
    switch (m) {
        case Mode::Paging: Profile.monitor_paging = true; strcpy(szWindowText[2], "POCSAG/FLEX..."); break;
        case Mode::Acars: Profile.monitor_acars = true; strcpy(szWindowText[2], "ACARS..."); break;
        case Mode::Mobitex: Profile.monitor_mobitex = true; strcpy(szWindowText[2], "MOBITEX..."); break;
        case Mode::Ermes: Profile.monitor_ermes = true; strcpy(szWindowText[2], "ERMES..."); break;
    }
    SetMessageItemPositionsWidth();
    Reset_ATB();
    acars.reset();
    SetNewWindowText((char *)"");
}

// ---- filters ----------------------------------------------------------------------------------
static Filter to_api(const FILTER &f)
{
    Filter o;
    o.type = f.type; o.capcode = f.capcode; o.label = f.label; o.text = f.text;
    o.match_exact_msg = f.match_exact_msg; o.cmd_enabled = f.cmd_enabled; o.reject = f.reject;
    o.monitor_only = f.monitor_only; o.wave_number = f.wave_number; o.label_enabled = f.label_enabled;
    o.label_color = f.label_color; o.smtp = f.smtp; o.sep_filterfile_en = f.sep_filterfile_en;
    for (int i = 0; i < f.sep_filterfiles && i < 3; i++) o.sep_filterfiles.push_back(f.sep_filterfile[i]);
    o.hitcounter = f.hitcounter; o.lasthit_date = f.lasthit_date; o.lasthit_time = f.lasthit_time;
    return o;
}

static FILTER from_api(const Filter &o)
{
    FILTER f;
    memset(&f, 0, sizeof f);
    f.type = (FILTER_TYPE)o.type;
    snprintf(f.capcode, sizeof f.capcode, "%s", o.capcode.c_str());
    snprintf(f.label, sizeof f.label, "%s", o.label.c_str());
    snprintf(f.text, sizeof f.text, "%s", o.text.c_str());
    f.match_exact_msg = o.match_exact_msg; f.cmd_enabled = o.cmd_enabled; f.reject = o.reject;
    f.monitor_only = o.monitor_only; f.wave_number = o.wave_number; f.label_enabled = o.label_enabled;
    f.label_color = o.label_color; f.smtp = o.smtp; f.sep_filterfile_en = o.sep_filterfile_en;
    f.sep_filterfiles = (int)std::min<size_t>(o.sep_filterfiles.size(), 3);
    for (int i = 0; i < f.sep_filterfiles; i++)
        snprintf(f.sep_filterfile[i], sizeof f.sep_filterfile[i], "%s", o.sep_filterfiles[i].c_str());
    f.hitcounter = o.hitcounter;
    snprintf(f.lasthit_date, sizeof f.lasthit_date, "%s", o.lasthit_date.c_str());
    snprintf(f.lasthit_time, sizeof f.lasthit_time, "%s", o.lasthit_time.c_str());
    return f;
}

std::vector<Filter> Decoder::filters() const
{
    std::vector<Filter> v;
    for (auto &f : Profile.filters) v.push_back(to_api(f));
    return v;
}

void Decoder::set_filters(const std::vector<Filter> &f)
{
    Profile.filters.clear();
    for (auto &x : f) Profile.filters.push_back(from_api(x));
    bUpdateFilters = true;
}

bool Decoder::save_filters()
{
    if (!impl_->has_dir && cfg_.filters_file.empty()) return false;
    WriteFilters(&Profile, 0);
    bUpdateFilters = false;
    return true;
}

// IDM_RELOAD in upstream: re-read filters.ini, falling back to the backup file if it is damaged.
bool Decoder::reload_filters()
{
    if (!FileExists(szFilterPathName)) return false;
    Profile.filters.clear();
    if (ReadFilters(szFilterPathName, &Profile, false)) return true;
    Profile.filters.clear();
    return ReadFilters(szFilterBackup, &Profile, false);
}

void Decoder::reset_hitcounters(bool all, const std::vector<int> &indices)
{
    auto reset = [](FILTER &f) { f.hitcounter = 0; f.lasthit_date[0] = f.lasthit_time[0] = 0; };
    if (all) for (auto &f : Profile.filters) reset(f);
    else for (int i : indices) if (i >= 0 && (size_t)i < Profile.filters.size()) reset(Profile.filters[i]);
    bUpdateFilters = true;
}

// ---- display state ----------------------------------------------------------------------------
void Decoder::clear_panes(bool monitor, bool filter)
{
    auto clear = [](PaneStruct &p) {
        p.Bottom = 0;
        p.currentPos = 0;
        p.currentColor = COLOR_UNUSED;
        for (unsigned x = 0; x < p.buff_lines; x++) { p.buff_char[x * (LINE_SIZE + 1)] = 0; p.buff_color[x * (LINE_SIZE + 1)] = COLOR_UNUSED; }
    };
    if (monitor) clear(Pane1);
    if (filter) clear(Pane2);
    SetNewWindowText((char *)"");
}

std::vector<Column> Decoder::columns(bool filter_pane) const
{
    std::vector<Column> v;
    int left = 0;
    for (int i = 0; i < 7; i++) {
        int id = Profile.ScreenColumns[i];
        if (id == 0) break;
        Column c{id, left, 0, ""};
        int w = (i == 0 ? (int)cxChar : 0);
        switch (id) {
            case 1:
                c.title = Profile.monitor_acars ? "Aircraft Reg"
                          : Profile.monitor_mobitex ? "MAN" : "Address";
                if (Profile.monitor_acars || Profile.monitor_mobitex || Profile.monitor_ermes || Profile.FlexGroupMode) {}
                break;
            case 2: c.title = "Time"; break;
            case 3: c.title = "Date"; break;
            case 4: c.title = Profile.monitor_acars ? "Msg.No." : Profile.monitor_mobitex ? "Sender" : "Mode"; break;
            case 5: c.title = Profile.monitor_acars ? "DBI" : "Type"; break;
            case 6: c.title = Profile.monitor_acars ? "Mode" : "Bitrate"; break;
            case 7: c.title = filter_pane ? "Filtered Messages" : "Monitored Messages"; break;
        }
        if (id == 7) {
            c.width = 0;
        } else {
            c.width = w + iItemWidths[id] / (int)cxChar;
            left += iItemWidths[id] / (int)cxChar;
            if (i == 0) left += (int)cxChar;
        }
        v.push_back(c);
    }
    return v;
}

int Decoder::signal_position() const { return g_si_index; }
void Decoder::on_tray(TrayCallback cb) { g_on_tray = std::move(cb); }
void Decoder::set_trayed(bool t) { bTrayed = t; }
unsigned long Decoder::seconds_elapsed() const { return iSecondsElapsed; }
void Decoder::reset_audio() { Reset_ATB(); }
bool Decoder::flextime_detected() const { return bFlexTIME_detected; }
std::string Decoder::mail_test()
{
    return MailSendTest(Profile.szMailHost, Profile.szMailHeloDomain, Profile.szMailFrom, Profile.szMailTo, Profile.szMailUser,
                        Profile.szMailPassword, Profile.iMailPort, Profile.nMailOptions);
}
std::string Decoder::data_dir() const { return szPath; }

std::string Decoder::status_text() const
{
    std::string s = szWindowText[0];
    for (int i = 1; i < 6; i++)
        if (szWindowText[i][0]) { s += " - "; s += szWindowText[i]; }
    return s;
}

double Decoder::rx_quality() const { return dRX_Quality; }
size_t Decoder::filter_count() const { return Profile.filters.size(); }

// ---- Resampler ------------------------------------------------------------------
Resampler::Resampler(double in_rate, double out_rate) : step_(in_rate / out_rate) {}

void Resampler::process(const int16_t *in, size_t n, std::vector<int16_t> &out)
{
    // Catmull-Rom over a 4-sample window; hist_[1] is the sample at "pos_ == 0".
    for (size_t i = 0; i < n; i++) {
        hist_[0] = hist_[1]; hist_[1] = hist_[2]; hist_[2] = hist_[3]; hist_[3] = in[i];
        fed_++;
        if (fed_ < 4) continue;
        while (pos_ < 1.0) {
            double t = pos_;
            double p0 = hist_[0], p1 = hist_[1], p2 = hist_[2], p3 = hist_[3];
            double v = 0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t +
                              (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
            out.push_back((int16_t)std::max(-32768.0, std::min(32767.0, std::round(v))));
            pos_ += step_;
        }
        pos_ -= 1.0;
    }
}

}  // namespace pdw
