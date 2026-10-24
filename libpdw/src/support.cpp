// Glue between the imported PDW core and the portable libpdw API: the globals the
// Windows GUI normally owns, a message sink in place of ShowMessage()'s pane/log
// pipeline (full pipeline: later slice), and pdw::Decoder.
//
// Copyright and licence: see NOTICE. GPL-3.0-or-later.
#include <sys/stat.h>
#include <sys/time.h>
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
#include "pdw/decoder.hpp"

// ---- globals the Windows front end normally defines -------------------------
PROFILE Profile;
PaneStruct Pane1, Pane2;
char szWindowText[6][1000];
char szPath[MAX_PATH] = ".";
char szFilenameDate[16];
bool bUpdateFilters = false;
bool bTrayed = false;
int nCount_Messages = 0, nCount_Groupcalls = 0, nCount_Rejected = 0, nCount_Blocked = 0;
int nCount_Missed[2] = {0, 0};
int nCount_BlockBuffer[2] = {0, 0};
double dRX_Quality = 100.0;

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

char szCurrentDate_unused_guard;  // (szCurrentDate/szCurrentTime live in the decode extract)

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

// ---- display layer replaced by event hooks ----------------------------------
static pdw::Decoder::MessageCallback g_on_message;
static pdw::Decoder::NoticeCallback g_on_notice;
static std::string g_notice;

void UpdateSigInd(int) {}
void SetNewWindowText(char *) {}

static void flush_notice()
{
    if (!g_notice.empty()) {
        if (g_on_notice) g_on_notice(g_notice);
        g_notice.clear();
    }
}

void build_show_line(PaneStruct *, char cin, int option)
{
    if (option == 1) flush_notice();
    else g_notice.push_back(cin);
}

void display_line(PaneStruct *) { flush_notice(); }

// Replaces Misc.cpp ShowMessage(): same buffer hygiene, but hands the message to the
// application instead of drawing/filtering/logging it.
void ShowMessage()
{
    bool bGroupcode = memcmp(Current_MSG[MSG_CAPCODE], "20295", 5) == 0;

    if (!iConvertingGroupcall) {
        message_buffer[iMessageIndex] = 0;
        message_color[iMessageIndex] = COLOR_UNUSED;
        iMessageIndex = 0;
    }
    memcpy(Current_MSG[MSG_MESSAGE], message_buffer, MAX_STR_LEN);

    nCount_Messages++;
    if (iConvertingGroupcall && bGroupcode) nCount_Groupcalls++;

    if (g_on_message) {
        pdw::Message m;
        m.capcode = Current_MSG[MSG_CAPCODE];
        m.time = Current_MSG[MSG_TIME];
        m.date = Current_MSG[MSG_DATE];
        m.mode = Current_MSG[MSG_MODE];
        m.type = Current_MSG[MSG_TYPE];
        m.bitrate = Current_MSG[MSG_BITRATE];
        // PDW's text is Windows-1252; only 0xBB (linefeed marker) and 0xB7 (NUL marker, Mobitex)
        // can occur. Transcode to UTF-8, keeping one colour entry per output byte.
        for (int i = 0; message_buffer[i]; i++) {
            unsigned char c = message_buffer[i];
            if (c < 0x80) { m.text.push_back((char)c); m.colors.push_back(message_color[i]); }
            else {
                m.text.push_back((char)(0xC0 | (c >> 6)));
                m.text.push_back((char)(0x80 | (c & 0x3F)));
                m.colors.push_back(message_color[i]);
                m.colors.push_back(message_color[i]);
            }
        }
        g_on_message(m);
    }

    iMessageIndex = 0;
    if (!iConvertingGroupcall || bGroupcode) memset(message_buffer, 0, sizeof(message_buffer));
    if (Current_MSG[MSG_MOBITEX][0]) Current_MSG[MSG_MOBITEX][0] = '\0';
}

// ---- pdw::Decoder -------------------------------------------------------------
namespace pdw {

namespace {
constexpr size_t kBufferSamples = 8192;  // PDW's SIZEOF_AUDIOBUFFER
constexpr size_t kTickSamples = 4410;    // PDW's 100 ms decode timer at 44.1 kHz
bool g_instance_alive = false;
}  // namespace

struct Decoder::Impl {
    std::vector<uint8_t> pending;       // samples of the buffer being filled
    std::vector<std::vector<uint8_t>> ready;  // completed buffers awaiting the next tick
    size_t total = 0, next_tick = kTickSamples;

    // Process_ReadyBuffers() from sound_in.cpp, POCSAG/FLEX path.
    void tick()
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
        for (auto &buf : ready) Audio_To_Bits(reinterpret_cast<char *>(buf.data()), (long)buf.size());
        ready.clear();
        flush_notice();
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
                tick();
                next_tick += kTickSamples;
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

    // Defaults as set in upstream WinMain() (PDW.cpp), then overridden by cfg.
    memset(&Profile, 0, sizeof(Profile));
    Profile.LabelLog = 1; Profile.LabelNewline = 1; Profile.Linefeed = 1; Profile.Separator = 1;
    Profile.MonthNumber = 1; Profile.DateFormat = 0; Profile.BlockDuplicate = 0;
    Profile.showtone = cfg.show_tone; Profile.shownumeric = cfg.show_numeric;
    Profile.showmisc = cfg.show_misc; Profile.showinstr = cfg.show_instr;
    Profile.convert_si = cfg.convert_si;
    Profile.decodepocsag = cfg.decode_pocsag; Profile.decodeflex = cfg.decode_flex;
    Profile.pocsag_512 = cfg.pocsag_512; Profile.pocsag_1200 = cfg.pocsag_1200; Profile.pocsag_2400 = cfg.pocsag_2400;
    Profile.pocsag_fnu = cfg.pocsag_function_numeric; Profile.pocsag_showboth = cfg.pocsag_show_both;
    Profile.flex_1600 = cfg.flex_1600; Profile.flex_3200 = cfg.flex_3200; Profile.flex_6400 = cfg.flex_6400;
    Profile.FlexTIME = cfg.flex_time;
    Profile.invert = cfg.invert ? 1 : 0;
    Profile.audioEnabled = 1;
    Profile.audioSampleRate = kSampleRate;
    Profile.audioConfig = cfg.audio_config;
    Profile.monitor_paging = true;
    Profile.ScreenColumns[0] = MSG_CAPCODE;
    snprintf(szPath, sizeof szPath, "%s", cfg.data_dir.c_str());

    // Mirrors the init in upstream WM_CREATE / WinMain.
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

    low_audio = Profile.invert ? DEFAULT_HI_AUDIO : DEFAULT_LO_AUDIO;
    high_audio = Profile.invert ? DEFAULT_LO_AUDIO : DEFAULT_HI_AUDIO;
    sync_bit = low_audio;
    SetAudioConfig(Profile.audioConfig);
    Reset_ATB();
    stat_timer = 0;
}

Decoder::~Decoder()
{
    g_on_message = nullptr;
    g_on_notice = nullptr;
    delete impl_;
    g_instance_alive = false;
}

void Decoder::on_message(MessageCallback cb) { g_on_message = std::move(cb); }
void Decoder::on_notice(NoticeCallback cb) { g_on_notice = std::move(cb); }

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
}

double Decoder::rx_quality() const { return dRX_Quality; }

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
