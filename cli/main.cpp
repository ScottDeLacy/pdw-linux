// pdw-cli: decode POCSAG/FLEX pager traffic from FM-discriminator audio.
// Based on PDW by Jason Petty and Peter Hunt (GPL-3.0-or-later). See NOTICE.
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "pdw/decoder.hpp"
#include "wav.hpp"

static void usage(FILE *o)
{
    fputs(
        "usage: pdw-cli [options] <input.wav | ->\n"
        "  '-' reads raw signed 16-bit little-endian mono audio from stdin, e.g.\n"
        "      rtl_fm -f 152.4M -M fm -s 22050 -g 40 | pdw-cli --rate 22050 -\n"
        "options:\n"
        "  --rate HZ        sample rate of raw stdin input (default 22050)\n"
        "  --gain X         input gain multiplier (default 1.0)\n"
        "  --config N       audio level preset 1-13, as PDW's Interface dialog (default 5)\n"
        "  --no-pocsag      disable POCSAG        --no-flex  disable FLEX\n"
        "  --no-512 --no-1200 --no-2400   disable POCSAG baud rates\n"
        "  --no-1600 --no-3200 --no-6400  disable FLEX speeds\n"
        "  --invert         start with inverted polarity\n"
        "  --no-group       do not convert FLEX group calls to text\n"
        "  --json           one JSON object per message\n"
        "  --notices        also print decoder notices to stderr\n"
        "  -h, --help\n",
        o);
}

static std::string json_escape(const std::string &s)
{
    std::string r;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { r += '\\'; r += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); r += b; }
        else r += (char)c;
    }
    return r;
}

int main(int argc, char **argv)
{
    pdw::Config cfg;
    double rate = 22050, gain = 1.0;
    bool json = false, notices = false;
    std::string input;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](const char *name) -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", name); exit(2); }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(stdout); return 0; }
        else if (a == "--rate") rate = atof(need("--rate"));
        else if (a == "--gain") gain = atof(need("--gain"));
        else if (a == "--config") cfg.audio_config = atoi(need("--config"));
        else if (a == "--no-pocsag") cfg.decode_pocsag = false;
        else if (a == "--no-flex") cfg.decode_flex = false;
        else if (a == "--no-512") cfg.pocsag_512 = false;
        else if (a == "--no-1200") cfg.pocsag_1200 = false;
        else if (a == "--no-2400") cfg.pocsag_2400 = false;
        else if (a == "--no-1600") cfg.flex_1600 = false;
        else if (a == "--no-3200") cfg.flex_3200 = false;
        else if (a == "--no-6400") cfg.flex_6400 = false;
        else if (a == "--invert") cfg.invert = true;
        else if (a == "--no-group") cfg.convert_si = false;
        else if (a == "--json") json = true;
        else if (a == "--notices") notices = true;
        else if (a == "-" || a[0] != '-') input = a;
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); usage(stderr); return 2; }
    }
    if (input.empty()) { usage(stderr); return 2; }
    if (rate <= 0 || gain <= 0) { fprintf(stderr, "--rate and --gain must be positive\n"); return 2; }

    pdw::Decoder dec(cfg);
    dec.on_message([&](const pdw::Message &m) {
        if (json) {
            printf("{\"capcode\":\"%s\",\"time\":\"%s\",\"date\":\"%s\",\"mode\":\"%s\",\"type\":\"%s\","
                   "\"bitrate\":\"%s\",\"text\":\"%s\"}\n",
                   json_escape(m.capcode).c_str(), json_escape(m.time).c_str(), json_escape(m.date).c_str(),
                   json_escape(m.mode).c_str(), json_escape(m.type).c_str(), json_escape(m.bitrate).c_str(),
                   json_escape(m.text).c_str());
        } else {
            printf("%s %s %s %s %s %s %s\n", m.capcode.c_str(), m.time.c_str(), m.date.c_str(), m.mode.c_str(),
                   m.type.c_str(), m.bitrate.c_str(), m.text.c_str());
        }
        fflush(stdout);
    });
    if (notices) dec.on_notice([](const std::string &n) { fprintf(stderr, "[notice]%s\n", n.c_str()); });

    pdw::Resampler rs(rate);
    std::vector<int16_t> chunk, out;
    auto feed = [&](const int16_t *p, size_t n) {
        if (rate == pdw::kSampleRate) { dec.feed_s16(p, n, (float)gain); return; }
        out.clear();
        rs.process(p, n, out);
        dec.feed_s16(out.data(), out.size(), (float)gain);
    };

    if (input == "-") {
        int16_t buf[4096];
        size_t carry = 0;
        uint8_t raw[8192];
        ssize_t n;
        while ((n = read(STDIN_FILENO, raw + carry, sizeof raw - carry)) > 0) {
            size_t have = carry + (size_t)n, samples = have / 2;
            for (size_t i = 0; i < samples; i++) buf[i] = (int16_t)(raw[2 * i] | raw[2 * i + 1] << 8);
            feed(buf, samples);
            carry = have - samples * 2;
            if (carry) raw[0] = raw[have - 1];
        }
    } else {
        wav::Audio a;
        std::string err;
        if (!wav::read(input, a, err)) { fprintf(stderr, "pdw-cli: %s\n", err.c_str()); return 1; }
        rate = a.rate;
        rs = pdw::Resampler(rate);
        feed(a.samples.data(), a.samples.size());
    }
    dec.finish();
    return 0;
}
