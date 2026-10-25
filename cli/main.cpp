// pdw-cli: decode POCSAG/FLEX pager traffic from FM-discriminator audio.
// Based on PDW by Jason Petty and Peter Hunt (GPL-3.0-or-later). See NOTICE.
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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
        "  --dir DIR        PDW folder: pdw.ini, filters.ini, language.df, Logfiles/, Wavfiles/\n"
        "                   (an existing Windows PDW folder works as it is). Without it nothing is written.\n"
        "  --filters FILE   use this filters.ini\n"
        "  --set KEY=VALUE  override a pdw.ini setting, e.g. --set BlockDuplicate=3 --set FlexGroupMode=2\n"
        "  --save-settings  write pdw.ini (and filters.ini) into --dir at the end\n"
        "  --filtered       print the filter pane instead of the monitor pane\n"
        "  --color[=never]  ANSI colours as PDW paints them (default: only on a terminal)\n"
        "  --json           one JSON object per message instead of pane lines\n"
        "  --notices        also print decoder notices to stderr\n"
        "  --status         also print the title-bar status text to stderr\n"
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
    bool json = false, notices = false, status = false, filtered = false, save = false;
    int color = isatty(STDOUT_FILENO) ? 1 : 0;
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
        else if (a == "--dir") cfg.dir = need("--dir");
        else if (a == "--filters") cfg.filters_file = need("--filters");
        else if (a == "--set") {
            std::string kv = need("--set");
            size_t eq = kv.find('=');
            if (eq == std::string::npos || eq == 0) { fprintf(stderr, "--set needs KEY=VALUE\n"); return 2; }
            cfg.ini[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
        else if (a == "--save-settings") save = true;
        else if (a == "--filtered") filtered = true;
        else if (a == "--color") color = 1;
        else if (a == "--color=never") color = 0;
        else if (a == "--status") status = true;
        else if (a == "--json") json = true;
        else if (a == "--notices") notices = true;
        else if (a == "-" || a[0] != '-') input = a;
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); usage(stderr); return 2; }
    }
    if (input.empty()) { usage(stderr); return 2; }
    if (rate <= 0 || gain <= 0) { fprintf(stderr, "--rate and --gain must be positive\n"); return 2; }

    std::unique_ptr<pdw::Decoder> decp;
    try {
        decp.reset(new pdw::Decoder(cfg));
    } catch (const std::exception &e) {
        fprintf(stderr, "pdw-cli: %s\n", e.what());
        return 1;
    }
    pdw::Decoder &dec = *decp;

    if (json) {
        dec.on_message([&](const pdw::Message &m) {
            printf("{\"capcode\":\"%s\",\"time\":\"%s\",\"date\":\"%s\",\"mode\":\"%s\",\"type\":\"%s\","
                   "\"bitrate\":\"%s\",\"text\":\"%s\",\"monitor\":%s,\"filtered\":%s,\"monitor_only\":%s,"
                   "\"filter\":%d,\"label\":\"%s\"}\n",
                   json_escape(m.capcode).c_str(), json_escape(m.time).c_str(), json_escape(m.date).c_str(),
                   json_escape(m.mode).c_str(), json_escape(m.type).c_str(), json_escape(m.bitrate).c_str(),
                   json_escape(m.text).c_str(), m.in_monitor ? "true" : "false", m.in_filter ? "true" : "false",
                   m.monitor_only ? "true" : "false", m.filter, json_escape(m.label).c_str());
            fflush(stdout);
        });
    } else {
        const int want = filtered ? 1 : 0;
        dec.on_line([&, want](int pane, const std::string &text, const std::vector<uint8_t> &colors) {
            if (pane != want) return;
            if (!color) {
                printf("%s\n", text.c_str());
            } else {
                uint32_t last = 0xFFFFFFFFu;
                for (size_t i = 0; i < text.size(); i++) {
                    uint32_t c = dec.color_rgb(i < colors.size() ? colors[i] : 0);
                    if (c != last) { printf("\x1b[38;2;%u;%u;%um", (c >> 16) & 255, (c >> 8) & 255, c & 255); last = c; }
                    putchar(text[i]);
                }
                printf("\x1b[0m\n");
            }
            fflush(stdout);
        });
    }
    if (notices) dec.on_notice([](const std::string &n) { fprintf(stderr, "[notice]%s\n", n.c_str()); });
    if (status) dec.on_status([](const std::string &t) { fprintf(stderr, "[status]%s\n", t.c_str()); });

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
    if (save && !dec.save_settings()) fprintf(stderr, "pdw-cli: --save-settings needs --dir\n");
    return 0;
}
