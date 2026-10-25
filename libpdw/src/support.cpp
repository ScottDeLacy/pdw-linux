// Glue between the imported PDW core and the portable libpdw API: the globals the
// Windows GUI normally owns, headless message panes, the timers PDW drives from
// WM_TIMER, and pdw::Decoder.
//
// Copyright and licence: see NOTICE. GPL-3.0-or-later.
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
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
#include "pdw/decoder.hpp"

// ---- globals the Windows front end normally defines -------------------------
PROFILE Profile;
PaneStruct Pane1, Pane2;
char szWindowText[6][1000];
char szPath[MAX_PATH] = ".";
char szLogPathName[MAX_PATH], szWavePathName[MAX_PATH], szIniPathName[MAX_PATH];
char szFilterPathName[MAX_PATH], szFilterBackup[MAX_PATH], szACARSPath[MAX_PATH];
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
extern unsigned long int aMessages[1000][3];
extern int iPanePos;

// Pieces of sound_in.cpp that the audio extract leaves out (no Win32 wave API).
extern int nSamples, preamble_count[3], flex_cnt_1600, crossing, pre_threshold, sync_bit;
extern long double WatchStep, clkt_hi, clkt_lo;
extern double WatchCtr;
extern long BaudRate, last_baud_rate;
extern int skipped_sc, process_acars_bit, config_index;
extern char atb_bit;

#define FINE_CLKT_HI 1.90
#define FINE_CLKT_LO 0.32

// sound_in.cpp Reset_ATB(), minus the Mobitex branch (Mobitex: later slice).
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

    clkt_hi = FINE_CLKT_HI;
    clkt_lo = FINE_CLKT_LO;
    BaudRate = 1600;
    last_baud_rate = 1600;

    WatchStep = (long double)Profile.audioSampleRate / (long double)BaudRate;
}

// ---- utilities (written for this port) --------------------------------------
static long g_clock_offset_s = 0;  // FlexTIME: shift of the program clock, not the OS clock

static time_t now_adjusted() { return time(nullptr) + g_clock_offset_s; }

void GetLocalTime(SYSTEMTIME *st)
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    time_t t = tv.tv_sec + g_clock_offset_s;
    struct tm tmv;
    localtime_r(&t, &tmv);
    st->wYear = tmv.tm_year + 1900;
    st->wMonth = tmv.tm_mon + 1;
    st->wDayOfWeek = tmv.tm_wday;
    st->wDay = tmv.tm_mday;
    st->wHour = tmv.tm_hour;
    st->wMinute = tmv.tm_min;
    st->wSecond = tmv.tm_sec;
    st->wMilliseconds = tv.tv_usec / 1000;
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
    if (target != (time_t)-1) g_clock_offset_s = (long)(target - time(nullptr));
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

// ---- mail, tray: not part of the headless core -------------------------------------
void SystemTrayWindow(bool) {}
int MailInit(char *, char *, char *, char *, char *, char *, int, int) { return 0; }
int SendMail(HWND, bool, bool, int, char *, char *, char *, char *, char *, char *, char *, char *) { return 0; }

// ---- application hooks ----------------------------------------------------------
static pdw::Decoder::MessageCallback g_on_message;
static pdw::Decoder::NoticeCallback g_on_notice;
static pdw::Decoder::LineCallback g_on_line;
static pdw::Decoder::StatusCallback g_on_status;
static std::string g_notice;

void UpdateSigInd(int) {}

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
        check_save_data();
        for (auto &buf : ready) {
            if (Profile.monitor_paging) Audio_To_Bits(reinterpret_cast<char *>(buf.data()), (long)buf.size());
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
                memmove(aMessages[0], aMessages[1], sizeof(aMessages));
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
    read_language_tables();
}

Decoder::~Decoder()
{
    g_on_message = nullptr;
    g_on_notice = nullptr;
    g_on_line = nullptr;
    g_on_status = nullptr;
    pdw_set_sound_hook(nullptr);
    free_lang_tables();
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
    if (bUpdateFilters && impl_->has_dir) {  // as on exit in upstream
        WriteFilters(&Profile, 0);
        bUpdateFilters = false;
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
