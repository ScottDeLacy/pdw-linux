// Minimal WAV reader: PCM 8/16/24/32-bit and 32-bit float, any channel count
// (first channel is used). Returns mono int16 samples and the file's rate.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace wav {

struct Audio {
    std::vector<int16_t> samples;
    double rate = 0;
};

inline bool read(const std::string &path, Audio &out, std::string &err)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);

    auto u16 = [&](size_t o) { return (uint16_t)(d[o] | d[o + 1] << 8); };
    auto u32 = [&](size_t o) { return (uint32_t)(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (uint32_t)d[o + 3] << 24); };
    if (d.size() < 12 || memcmp(d.data(), "RIFF", 4) || memcmp(&d[8], "WAVE", 4)) {
        err = "not a RIFF/WAVE file";
        return false;
    }
    int fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    size_t pos = 12;
    while (pos + 8 <= d.size()) {
        uint32_t sz = u32(pos + 4);
        size_t body = pos + 8;
        if (!memcmp(&d[pos], "fmt ", 4) && body + 16 <= d.size()) {
            fmt = u16(body); ch = u16(body + 2); rate = u32(body + 4); bits = u16(body + 14);
            if (fmt == 0xFFFE && sz >= 26 && body + 26 <= d.size()) fmt = u16(body + 24);  // extensible
        } else if (!memcmp(&d[pos], "data", 4)) {
            if (!ch || !bits) { err = "data chunk before fmt chunk"; return false; }
            size_t avail = std::min<size_t>(sz, d.size() - body);
            size_t bps = bits / 8, frame = bps * ch, frames = avail / frame;
            out.rate = rate;
            out.samples.resize(frames);
            for (size_t i = 0; i < frames; i++) {
                const uint8_t *p = &d[body + i * frame];
                int32_t v;
                if (fmt == 1 && bits == 8) v = ((int)p[0] - 128) << 8;
                else if (fmt == 1 && bits == 16) v = (int16_t)(p[0] | p[1] << 8);
                else if (fmt == 1 && bits == 24) v = (int32_t)(p[0] << 8 | p[1] << 16 | p[2] << 24) >> 16;
                else if (fmt == 1 && bits == 32) v = (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24) >> 16;
                else if (fmt == 3 && bits == 32) {
                    float x; memcpy(&x, p, 4);
                    v = (int32_t)(x * 32767.0f);
                } else { err = "unsupported WAV sample format"; return false; }
                out.samples[i] = (int16_t)std::max(-32768, std::min(32767, v));
            }
            return true;
        }
        pos = body + sz + (sz & 1);
    }
    err = "no data chunk";
    return false;
}

}  // namespace wav
