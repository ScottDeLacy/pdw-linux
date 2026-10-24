// Public API of libpdw: feed discriminator audio in, get decoded pager messages out.
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
#include <string>
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
    std::string text;      // message body, with PDW's character mapping applied
    std::vector<uint8_t> colors;  // per-character PDW colour index (COLOR_BITERRORS etc.)
};

struct Config {
    // Audio level preset, same list as PDW's "Interface" dialog: 0 custom,
    // 1-4 discriminator, 5-7 earphone, 8-10 speaker, 11-13 tape/rec out.
    int audio_config = 5;

    bool decode_pocsag = true, decode_flex = true;
    bool pocsag_512 = true, pocsag_1200 = true, pocsag_2400 = true;
    bool flex_1600 = true, flex_3200 = true, flex_6400 = true;

    bool show_tone = true, show_numeric = true, show_misc = true;
    bool show_instr = true;
    bool convert_si = true;       // fold FLEX group calls into text messages (PDW default)
    bool pocsag_function_numeric = false;  // "NTTA" function-number typing
    bool pocsag_show_both = false;
    bool invert = false;          // initial polarity; auto-corrected on sync
    bool flex_time = false;       // use FLEX time/date broadcasts as program clock
    std::string data_dir = ".";   // where missed-groupcalls.txt etc. go
};

class Decoder {
public:
    using MessageCallback = std::function<void(const Message &)>;
    // Human-readable events: mode changes, "UNKNOWN SYNC HEADER", etc.
    using NoticeCallback = std::function<void(const std::string &)>;

    explicit Decoder(const Config &cfg = {});
    ~Decoder();
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;

    void on_message(MessageCallback cb);
    void on_notice(NoticeCallback cb);

    // Unsigned 8-bit mono @ 44100 Hz (128 = zero), as PDW's sound input.
    void feed_u8(const uint8_t *samples, size_t n);
    // Signed 16-bit mono @ 44100 Hz; `gain` scales before conversion to 8-bit.
    void feed_s16(const int16_t *samples, size_t n, float gain = 1.0f);
    // Flush the final partial buffer through the decoder.
    void finish();

    // Receive quality 0..100 (PDW's bit-error indicator).
    double rx_quality() const;
    const Config &config() const { return cfg_; }

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

}  // namespace pdw
