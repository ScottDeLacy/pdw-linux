// Public API of libpdw: feed discriminator audio in, get PDW's on-screen lines and
// decoded pager messages out.
//
// Based on PDW by Jason Petty and Peter Hunt (GPL-3.0-or-later). See NOTICE.
//
// Limitation inherited from upstream: decoder state is process-global, so only one
// Decoder may exist at a time. Input is 44100 Hz mono audio from an FM
// discriminator (e.g. `rtl_fm`); use pdw::Resampler for other rates.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace pdw {

constexpr int kSampleRate = 44100;

struct Message {
    std::string capcode;   // address (7 or 9 digits, or "???????" when corrupt)
    std::string time;      // HH:MM:SS (local, or FlexTIME-corrected)
    std::string date;
    std::string mode;      // "POCSAG-1".."POCSAG-4", "FLEX-A".."FLEX-D"
    std::string type;      // " ALPHA ", "NUMERIC", "SH/TONE", " GROUP ", ...
    std::string bitrate;   // "512", "1200", "2400", "1600", "3200", "6400"
    std::string text;      // message body (UTF-8), with PDW's character mapping applied
    std::vector<uint8_t> colors;  // per-byte PDW colour index (COLOR_BITERRORS etc.)

    // What the PDW pipeline did with it.
    bool in_monitor = true;      // shown in the monitor pane (pane 1)
    bool in_filter = false;      // shown in the filter pane (pane 2)
    bool monitor_only = false;   // matched a "monitor only" filter
    int filter = -1;             // index of the matching filter, or -1
    std::string label;           // filter label (with %-codes expanded), if any
};

struct Config {
    // Directory holding pdw.ini, filters.ini, language.df, Logfiles/ and Wavfiles/, i.e.
    // the folder a Windows PDW install lives in; existing files are used as they are.
    // Empty: built-in defaults and nothing is written to disk (no logging).
    std::string dir;
    // Use this filters.ini instead of <dir>/filters.ini (read-only unless it is inside `dir`).
    std::string filters_file;
    // Clock for message timestamps, statistics and block timers. 0: the wall clock (live use).
    // Otherwise a Unix time at which the audio starts; the clock then advances with the audio,
    // so decoding a recording stamps messages as if it were replayed in real time.
    int64_t clock_start = 0;
    // Folder with the data files shipped with PDW (language.df, base-ids.txt), used for files
    // missing from `dir`.
    std::string resource_dir;

    // Settings applied on top of pdw.ini, keyed by pdw.ini names ("Pocsag512", "FlexGroupMode",
    // "BlockDuplicate", ...), always in the [PDW] section.
    std::map<std::string, std::string> ini;

    // Convenience flags, equivalent to the ini keys noted. They only matter when they differ
    // from PDW's defaults; pdw.ini is overridden by them.
    int audio_config = 5;          // AudioConfiguration: 0 custom, 1-4 discriminator, 5-7 earphone,
                                   // 8-10 speaker, 11-13 tape/rec out (PDW's "Interface" dialog)
    bool decode_pocsag = true, decode_flex = true;                 // DecodePocsag, DecodeFlex
    bool pocsag_512 = true, pocsag_1200 = true, pocsag_2400 = true;  // Pocsag512/1200/2400
    bool flex_1600 = true, flex_3200 = true, flex_6400 = true;       // Flex1600/3200/6400
    bool show_tone = true, show_numeric = true, show_misc = true;    // ShowTone, ShowNumeric, ShowBinary
    bool show_instr = true;        // ShowInstr
    bool convert_si = true;        // ConvertInstr: fold FLEX group calls into text messages
    bool pocsag_function_numeric = false;  // PocsagFNU: "NTTA" function-number typing
    bool pocsag_show_both = false;         // PocsagShowBoth
    bool invert = false;           // InvertData: initial polarity; auto-corrected on sync
    bool flex_time = false;        // FlexTIME: use FLEX time broadcasts as program clock
};

// One entry of filters.ini (PDW's Filters dialog).
struct Filter {
    enum Type { Unused = 0, Flex = 1, Pocsag = 2, Text = 3, Ermes = 4, Acars = 5, Mobitex = 6 };
    int type = Pocsag;
    std::string capcode;            // address; '?' is a wildcard; POCSAG "1234567-3" limits the function number
    std::string label;              // shown below the message; %1..%9 insert digits of the address
    std::string text;               // words to find in the message ('&' separates up to 10; '^' = at start)
    bool match_exact_msg = false;
    bool cmd_enabled = false;       // run the filter command file
    bool reject = false;            // hide matching messages
    bool monitor_only = false;      // keep out of the filter pane
    int wave_number = 0;            // 0 none, 1.. = Sound0.wav..
    bool label_enabled = false;
    int label_color = 0;            // 0..16
    bool smtp = false;              // send e-mail (SMTP "selected filters" mode)
    bool sep_filterfile_en = false;
    std::vector<std::string> sep_filterfiles;   // up to 3 separate filter files
    unsigned hitcounter = 0;
    std::string lasthit_date, lasthit_time;
};

enum class Mode { Paging, Acars, Mobitex, Ermes };   // PDW's Monitor menu

// A column box of the pane title bars (DrawPaneLabels() in upstream Gfx.cpp), in character cells.
struct Column {
    int id;               // 1 address, 2 time, 3 date, 4 mode, 5 type, 6 bitrate, 7 message
    int start, width;     // start 0-based in cells; width 0 for the message column (takes the rest)
    std::string title;    // depends on the monitored mode; the message column's depends on the pane
};

// Running totals, as in PDW's statistics dialog.
struct Counters {
    int messages = 0, groupcalls = 0, rejected = 0, blocked = 0;
    int missed_groupcalls = 0, missed_groupcall_messages = 0;
    int block_buffer = 0, block_buffer_peak = 0;   // entries held by the duplicate-block timer
    int smtp_sessions = 0, smtp_mails = 0, smtp_errors = 0;
    std::string smtp_last_error;
    double rx_quality = 100.0;                     // percent
    // Hourly and daily message/character counts per mode, [mode][0 numeric / 1 alpha];
    // mode order: FLEX 6400, 3200, 1600, POCSAG 2400, 1200, 512, ACARS, Mobitex, ERMES.
    unsigned long hourly_msgs[9][2], hourly_chars[9][2], daily_msgs[9][2], daily_chars[9][2];
};

class Decoder {
public:
    using MessageCallback = std::function<void(const Message &)>;
    // Human-readable events: mode changes, "UNKNOWN SYNC HEADER", etc.
    using NoticeCallback = std::function<void(const std::string &)>;
    // A finished line of PDW's panes. pane 0 = monitor (top), 1 = filter (bottom). `text` is
    // UTF-8; `colors` has one PDW colour index per byte of `text`.
    using LineCallback = std::function<void(int pane, const std::string &text, const std::vector<uint8_t> &colors)>;
    // The title-bar status text (mode, cycle/frame, "Blocked Duplicate Message ..." etc.).
    using StatusCallback = std::function<void(const std::string &)>;
    // Filter sound: path of the .wav to play. Default: paplay/aplay/afplay.
    using SoundCallback = std::function<void(const std::string &)>;

    explicit Decoder(const Config &cfg = {});
    ~Decoder();
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;

    void on_message(MessageCallback cb);
    void on_notice(NoticeCallback cb);
    void on_line(LineCallback cb);
    void on_status(StatusCallback cb);
    void on_sound(SoundCallback cb);

    // Unsigned 8-bit mono @ 44100 Hz (128 = zero), as PDW's sound input.
    void feed_u8(const uint8_t *samples, size_t n);
    // Signed 16-bit mono @ 44100 Hz; `gain` scales before conversion to 8-bit.
    void feed_s16(const int16_t *samples, size_t n, float gain = 1.0f);
    // End of input: flush the last buffer, close logs, save filters.ini hit counters.
    void finish();

    // Character-set tables from language.df; index 0 in set_language() is the default (no
    // remapping), 1.. select languages()[i-1].
    std::vector<std::string> languages() const;
    void set_language(int index);

    // Write pdw.ini (all settings) and filters.ini into `dir`. Needs Config::dir.
    bool save_settings();

    // Colour (0xRRGGBB) PDW would paint a pane character with, for a colour index from
    // Message::colors or LineCallback::colors, using the current settings.
    uint32_t color_rgb(uint8_t color_index) const;

    Counters counters() const;
    bool flextime_detected() const;
    unsigned long seconds_elapsed() const;         // audio time since the start
    void reset_audio();                            // restart the bit slicer (after mode/decoder changes)

    // System tray behaviour (Display > Systemtray): `cb(false)` asks the front end to restore its window.
    using TrayCallback = std::function<void(bool hide)>;
    void on_tray(TrayCallback cb);
    void set_trayed(bool trayed);

    // Sends a test mail with the current account settings and returns the SMTP dialogue.
    std::string mail_test();

    // ---- settings, by pdw.ini name (section "PDW", "SMTP" or "Filter") ----
    // Integers are decimal strings, colours "r,g,b", text as is.
    bool get_setting(const std::string &key, std::string &out, const std::string &section = "PDW") const;
    long long get_int(const std::string &key, const std::string &section = "PDW") const;
    bool set_setting(const std::string &key, const std::string &value, const std::string &section = "PDW");
    bool set_int(const std::string &key, long long value, const std::string &section = "PDW");
    // Call after changing settings: applies what needs more than a store (audio preset, polarity,
    // pane sizes, columns, language, e-mail account).
    std::vector<std::tuple<std::string, std::string, std::string>> all_settings() const;   // section, key, value
    void apply_settings();
    Mode mode() const;
    void set_mode(Mode m);

    // ---- filters ----
    std::vector<Filter> filters() const;
    void set_filters(const std::vector<Filter> &f);
    bool save_filters();      // filters.ini (needs Config::dir or filters_file)
    bool reload_filters();
    void reset_hitcounters(bool all, const std::vector<int> &indices = {});

    // ---- display state ----
    void clear_panes(bool monitor, bool filter);   // also resets the pane buffers
    std::vector<Column> columns(bool filter_pane) const;
    int signal_position() const;                   // signal indicator needle, 0..20
    std::string status_text() const;               // title-bar text
    std::string data_dir() const;

    // Receive quality 0..100 (PDW's bit-error indicator).
    double rx_quality() const;
    const Config &config() const { return cfg_; }
    size_t filter_count() const;

private:
    struct Impl;
    Impl *impl_;
    Config cfg_;
};

// Streaming resampler to 44100 Hz (cubic interpolation).
class Resampler {
public:
    explicit Resampler(double in_rate, double out_rate = kSampleRate);
    // Appends resampled samples to `out`.
    void process(const int16_t *in, size_t n, std::vector<int16_t> &out);

private:
    double step_, pos_ = 0.0;
    int16_t hist_[4] = {0, 0, 0, 0};
    size_t fed_ = 0;
};

// High-pass filter that removes the DC offset of FM-demodulated audio (a tuning error shifts the
// whole signal; the slicer needs it centred). The default corner is slow enough not to disturb
// long runs of equal bits: a 100 ms run loses about 12 %.
class DcBlocker {
public:
    explicit DcBlocker(double sample_rate, double cutoff_hz = 0.2);
    void process(int16_t *samples, size_t n);   // in place

private:
    double r_;
    double x1_ = 0, y1_ = 0;
};

}  // namespace pdw
