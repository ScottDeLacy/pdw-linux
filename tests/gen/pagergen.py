#!/usr/bin/env python3
"""Synthetic pager-signal generator for testing pdw-cli.

Builds POCSAG and FLEX (1600/3200/6400, 2- and 4-level) bit streams from the public protocol
descriptions and renders them as FM-discriminator-like audio, so decoders can be
checked end to end without a radio or real (privacy-sensitive) captures.

Written for this project; no upstream PDW code is used, so the output is an
independent check on the port (multimon-ng is used as a third opinion).
"""
import argparse
import math
import struct
import sys
import wave

# ---------------------------------------------------------------- BCH(31,21)
BCH_POLY = 0x769  # x^10 + x^9 + x^8 + x^6 + x^5 + x^3 + 1


def bch_parity_word(data21):
    """21 data bits (first transmitted = MSB) -> 32-bit codeword (BCH + even parity)."""
    rem = data21 << 10
    for bit in range(30, 9, -1):
        if rem & (1 << bit):
            rem ^= BCH_POLY << (bit - 10)
    cw = (data21 << 10) | (rem & 0x3FF)       # 31 bits
    parity = bin(cw).count("1") & 1
    return (cw << 1) | parity                 # 32 bits, MSB transmitted first


def word_bits(w32):
    return [(w32 >> (31 - i)) & 1 for i in range(32)]


def bits_of(value, n):
    return [(value >> (n - 1 - i)) & 1 for i in range(n)]


# ---------------------------------------------------------------- POCSAG
SYNC_CW = 0x7CD215D8
IDLE_CW = 0x7A89C197
NUMERIC = "0123456789*U -]["  # PDW's table; index = nibble value


def pocsag_text_bits(text, numeric):
    bits = []
    if numeric:
        for ch in text:
            v = NUMERIC.index(ch)
            bits += [(v >> i) & 1 for i in range(4)]       # LSB first
    else:
        for ch in text:
            v = ord(ch) & 0x7F
            bits += [(v >> i) & 1 for i in range(7)]       # LSB first
    return bits


def pocsag_message_words(text, numeric):
    bits = pocsag_text_bits(text, numeric)
    # pad the last codeword: numeric with spaces (0xC), alpha with NULs
    pad = [0, 0, 1, 1] if numeric else [0]
    i = 0
    while len(bits) % 20:
        bits.append(pad[i % len(pad)])
        i += 1
    words = []
    for i in range(0, len(bits), 20):
        data = (1 << 20) | int("".join(map(str, bits[i:i + 20])), 2)
        words.append(bch_parity_word(data))
    return words


def pocsag_stream(messages, baud_preamble=576):
    """messages: list of (capcode 0..2097151, function 0-3, text, numeric?) -> bit list."""
    codewords = []  # flat list of codewords following the first sync
    for cap, func, text, numeric in messages:
        assert 0 <= cap < (1 << 21), "POCSAG capcodes are 21 bits"
        while len(codewords) % 16 != (cap & 7) * 2:
            codewords.append(IDLE_CW)
        codewords.append(bch_parity_word(((cap >> 3) << 2) | (func & 3)))
        if text:
            codewords += pocsag_message_words(text, numeric)
    while len(codewords) % 16:
        codewords.append(IDLE_CW)
    bits = [1, 0] * (baud_preamble // 2)
    for i in range(0, len(codewords), 16):
        bits += word_bits(SYNC_CW)
        for cw in codewords[i:i + 16]:
            bits += word_bits(cw)
    return bits


# ---------------------------------------------------------------- FLEX
# Modes: sync code "A" (the 16 bit outer code of the sync header), symbol rate of sync2+data,
# FSK levels, and the phases carried. 1600/2 is one phase; 3200/2 alternates phases A and C
# symbol by symbol; 1600/4 carries phases A and B in the two bits of each symbol; 3200/4
# (called FLEX-6400 by PDW) carries A,B and C,D on alternating symbols.
FLEX_MODES = {
    "1600/2": (0x870C, 1600, 2, "A"),
    "3200/2": (0x7B18, 3200, 2, "AC"),
    "3200/4": (0xB068, 1600, 4, "AB"),
    "6400/4": (0xDEA0, 3200, 4, "ABCD"),
}
FLEX_IDLE_WORD = 0x1FFFFF
FLEX_PAGE_ALPHA, FLEX_PAGE_SHORT_INSTR, FLEX_PAGE_TONE, FLEX_PAGE_STD_NUM = 5, 1, 2, 3


def flex_word(w21):
    """21-bit value as PDW/FLEX words: transmitted bits are the inverted value, LSB first."""
    data = 0
    for j in range(21):
        data = (data << 1) | (0 if (w21 >> j) & 1 else 1)
    return bch_parity_word(data)


def nibble_sum_fix(w21):
    """Set the low 4 bits so the 4-bit chunk sum of the word is 0x0F (BIW/vector checksum)."""
    rest = w21 & ~0xF
    s = ((rest >> 4) & 0xF) + ((rest >> 8) & 0xF) + ((rest >> 12) & 0xF) + ((rest >> 16) & 0xF) + ((rest >> 20) & 1)
    c = (0xF - s) & 0xF
    return rest | c


def flex_alpha_words(text, frag=3, cont=0):
    """Message header word + 3 chars per word. The first char of the first content word is
    ignored by receivers when frag == 3 (it carries the signature), so a space goes there."""
    chars = ([0x20] if frag == 3 else []) + [ord(c) & 0x7F for c in text] + [3]   # ETX ends the text
    while len(chars) % 3:
        chars.append(3)                       # ETX filler is dropped by receivers
    words = [(frag << 11) | (cont << 10)]
    for i in range(0, len(chars), 3):
        words.append(chars[i] | chars[i + 1] << 7 | chars[i + 2] << 14)
    return words


def flex_numeric_words(digits):
    """Standard numeric: 2 header bits, then 4-bit digits, LSB first, 21 bits per word."""
    idx = {c: i for i, c in enumerate("0123456789*U -][")}
    bits = [0, 0]
    for d in digits:
        v = idx[d]
        bits += [(v >> i) & 1 for i in range(4)]
    while len(bits) % 21:
        bits += [0, 0, 1, 1]                  # fill digit 0xC
    bits = bits[: (len(bits) // 21) * 21]
    return [sum(b << i for i, b in enumerate(bits[k:k + 21])) for k in range(0, len(bits), 21)]


def flex_phase_words(pages):
    """pages: list of dicts for one phase -> the 88 21-bit words of that phase's frame.
    kinds: alpha(capcode,text[,frag,cont]), numeric(capcode,digits), tone(capcode),
    short_numeric(capcode,digits3), short_instr(capcode,group,frame)."""
    n = len(pages)
    base = 1 + 2 * n                          # first message word
    addrs, vecs, msgs = [], [], []
    for p in pages:
        k = p["kind"]
        addrs.append((p["capcode"] + 0x8000) & 0x1FFFFF)
        w1 = base + len(msgs)
        if k == "alpha":
            mw = flex_alpha_words(p["text"], p.get("frag", 3), p.get("cont", 0))
            vec = (FLEX_PAGE_ALPHA << 4) | (w1 << 7) | (len(mw) << 14)
            msgs += mw
        elif k == "numeric":
            mw = flex_numeric_words(p["digits"])
            assert 1 <= len(mw) <= 8
            vec = (FLEX_PAGE_STD_NUM << 4) | (w1 << 7) | ((len(mw) - 1) << 14)
            msgs += mw
        elif k == "tone":
            vec = (FLEX_PAGE_TONE << 4) | (1 << 7)
        elif k == "short_numeric":
            idx = "0123456789*U -]["
            d = p["digits"]
            assert len(d) == 3
            vec = (FLEX_PAGE_TONE << 4) | (idx.index(d[0]) << 9) | (idx.index(d[1]) << 13) | (idx.index(d[2]) << 17)
        elif k == "short_instr":
            vec = (FLEX_PAGE_SHORT_INSTR << 4) | (p["frame"] << 10) | (p["group"] << 17)
        else:
            raise ValueError(k)
        vecs.append(nibble_sum_fix(vec))
    vsa = 1 + n
    biw = nibble_sum_fix(vsa << 10) if n else FLEX_IDLE_WORD
    words = [biw] + addrs + vecs + msgs
    assert len(words) <= 88, "frame overflow"
    return words + [FLEX_IDLE_WORD] * (88 - len(words))


def flex_alpha_frame(capcode, text):
    return flex_phase_words([{"kind": "alpha", "capcode": capcode, "text": text}])


# Symbol (0 = most negative level .. 3 = most positive) from the channel bits of phases (a, b).
# PDW: phase A bit is 1 for symbols 0,1; phase B bit is 1 for symbols 0,3.
_SYM_FROM_BITS = {(1, 1): 0, (1, 0): 1, (0, 0): 2, (0, 1): 3}


def flex_frame_symbols(mode, phase_pages, cycle=0, frame_no=1):
    """One FLEX frame as [(symbol list, symbols/second)] segments. phase_pages maps a phase
    letter to its page list (missing phases are idle)."""
    code, symrate, levels, phases = FLEX_MODES[mode]
    seg = []

    def two_level(bits):                       # channel bit 1 -> symbol 0, bit 0 -> symbol 3
        return [0 if b else 3 for b in bits]

    head = [1, 0] * 32                                        # dotting
    head += bits_of(code, 16) + bits_of(0xA6C6, 16) + bits_of(0xAAAA, 16) + bits_of(code ^ 0xFFFF, 16)
    head += [1, 0] * 8                                        # 16 bit gap
    head += word_bits(flex_word(nibble_sum_fix((cycle << 4) | (frame_no << 8))))   # FIW
    seg.append((two_level(head), 1600))
    seg.append((two_level([1, 0] * (symrate * 25 // 1000 // 2)), symrate))           # sync2: 25 ms idle

    words32 = {}
    for ph in phases:
        words = flex_phase_words(phase_pages.get(ph, []))
        words32[ph] = [word_bits(flex_word(w)) for w in words]
    steps = {ph: [] for ph in phases}
    for blk in range(11):
        for j in range(32):                                   # 8 words interleaved bit by bit
            for w in range(8):
                for ph in phases:
                    steps[ph].append(words32[ph][blk * 8 + w][j])
    syms = []
    for i in range(len(steps[phases[0]])):
        if levels == 2:
            for ph in phases:
                syms += two_level([steps[ph][i]])
        else:
            for pair in (phases[0:2], phases[2:4]):
                if pair:
                    syms.append(_SYM_FROM_BITS[(steps[pair[0]][i], steps[pair[1]][i])])
    seg.append((syms, symrate))
    return seg


def flex_stream(mode, frames):
    """frames: list of (cycle, frame_no, {phase: pages}) sent back to back -> segments."""
    segs = []
    for cycle, frame_no, phase_pages in frames:
        segs += flex_frame_symbols(mode, phase_pages, cycle, frame_no)
    return segs


# ---------------------------------------------------------------- ACARS
def _odd_parity(c):
    c &= 0x7F
    return c | (0x80 if bin(c).count("1") % 2 == 0 else 0)


def acars_crc(data):
    """CRC-16/KERMIT over the bytes from the mode character to ETX/ETB (the block check sequence)."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc


def acars_bits(reg, text, mode="2", ack="\x15", label="H1", block_id="4", msg_no="M01A", flight="QF0001"):
    """ACARS downlink block as the data bit sequence (characters LSB first, odd parity)."""
    assert len(reg) == 7 and len(label) == 2 and len(msg_no) == 4 and len(flight) == 6
    body = [mode] + list(reg) + [ack] + list(label) + [block_id, "\x02"] + list(msg_no) + list(flight) + list(text) + ["\x03"]
    body_bytes = [_odd_parity(ord(c)) for c in body]
    crc = acars_crc(body_bytes)
    frame = [0xFF] * 16 + [_odd_parity(0x2B), _odd_parity(0x2A), _odd_parity(0x16), _odd_parity(0x16), _odd_parity(0x01)]
    frame += body_bytes + [crc & 0xFF, crc >> 8, 0x7F]
    bits = []
    for b in frame:
        bits += [(b >> i) & 1 for i in range(8)]
    return bits


def render_acars(bits, rate=44100, amp=9000, invert=False, lead_s=0.2, tail_s=0.3, noise=0.0, lead_blip=True):
    """MSK audio, 2400 baud: a bit that repeats the previous polarity is one 2400 Hz cycle, a bit
    that changes it is a 1200 Hz half cycle, so every bit boundary is a zero crossing and the
    polarity of the half wave that starts there is the data bit.
    lead_blip: a short pulse before the key-up, as receiver noise would leave PDW's zero-crossing
    tracker in the polarity it needs: the original locks onto the first crossing it sees, so a
    signal whose first half wave has the polarity the tracker already holds is read half a bit
    late (a limitation of the original, not tested here)."""
    import random
    rng = random.Random(2)
    spb = rate / 2400.0
    out = [0] * int(lead_s * rate)
    n = len(bits)
    if lead_blip:
        first = (1 if bits[0] else -1) * (-1 if invert else 1)
        out += [-first * math.sin(math.pi * i / 20) for i in range(20)]
    noise_from = len(out)
    for k in range(n):
        d = bits[k]
        nxt = bits[k + 1] if k + 1 < n else d
        a = (0 if (k * spb) % 1 == 0 else 0)
        i0, i1 = int(math.ceil(k * spb)), int(math.ceil((k + 1) * spb))
        sign = (1 if d else -1) * (-1 if invert else 1)
        for i in range(i0, i1):
            t = (i - k * spb) / spb
            v = math.sin(math.pi * t) if nxt != d else math.sin(2 * math.pi * t)
            out.append(sign * v)
    out += [0] * int(tail_s * rate)
    # (noise only from the key-up on: noise in the silence before would set the tracker's polarity at random)
    return [int(max(-32768, min(32767, round(v * amp + (rng.gauss(0, noise) if noise and i >= noise_from else 0)))))
            for i, v in enumerate(out)]


# ---------------------------------------------------------------- audio
def render(bits, baud, rate=44100, amp=9000, invert=False, lead_s=0.3, tail_s=0.5, noise=0.0):
    """NRZ FSK -> discriminator-like audio (soft edges), 16-bit samples."""
    import random
    rng = random.Random(1)
    spb = rate / baud
    n = int(len(bits) * spb)
    raw = []
    for i in range(n):
        b = bits[min(int(i / spb), len(bits) - 1)]
        v = (-1 if b else 1) * (-1 if invert else 1)   # real discriminators: 1 = negative
        raw.append(v)
    # one-pole low pass, corner about 0.75 * baud
    a = 1 - math.exp(-2 * math.pi * 0.75 * baud / rate)
    y, out = 0.0, []
    for v in [0] * int(lead_s * rate) + raw + [0] * int(tail_s * rate):
        y += a * (v - y)
        s = y * amp + (rng.gauss(0, noise) if noise else 0)
        out.append(int(max(-32768, min(32767, round(s)))))
    return out


_LEVEL = {0: -1.0, 1: -1.0 / 3, 2: 1.0 / 3, 3: 1.0}


def render_symbols(segments, rate=44100, amp=9000, invert=False, lead_s=0.3, tail_s=0.5, noise=0.0, phase=0.0):
    """[(symbols, symbols_per_second)] -> discriminator-like audio. Symbols 0..3 are the FSK
    levels from most negative to most positive (2-level FLEX uses 0 and 3). One continuous
    clock runs through all segments; `phase` shifts the symbol grid by a fraction of a sample."""
    import random
    rng = random.Random(1)
    sign = -1.0 if invert else 1.0
    raw = []
    pos = phase                                  # start of the current symbol, in samples
    for syms, sr in segments:
        spb = rate / sr
        for s in syms:
            pos += spb
            raw += [sign * _LEVEL[s]] * (max(0, int(math.ceil(pos))) - len(raw))
    fastest = max(sr for _, sr in segments)
    a = 1 - math.exp(-2 * math.pi * 0.75 * fastest / rate)      # one-pole low pass
    y, out = 0.0, []
    for v in [0] * int(lead_s * rate) + raw + [0] * int(tail_s * rate):
        y += a * (v - y)
        out.append(int(max(-32768, min(32767, round(y * amp + (rng.gauss(0, noise) if noise else 0))))))
    return out


def write_wav(path, samples, rate=44100):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack("<%dh" % len(samples), *samples))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("kind", choices=["pocsag", "flex"])
    ap.add_argument("out")
    ap.add_argument("--baud", type=int, default=1200, help="POCSAG baud (512/1200/2400)")
    ap.add_argument("--mode", default="1600/2", choices=sorted(FLEX_MODES), help="FLEX mode")
    ap.add_argument("--rate", type=int, default=44100)
    ap.add_argument("--invert", action="store_true")
    ap.add_argument("--noise", type=float, default=0.0)
    ap.add_argument("--amp", type=int, default=9000)
    a = ap.parse_args()
    if a.kind == "pocsag":
        msgs = [(1234567, 3, "HELLO WORLD 0123", False),
                (765432, 3, "TEST ALPHA MESSAGE FROM PAGERGEN", False),
                (234567, 0, "5551234 -98", True),
                (1000001, 1, "", False)]
        bits = pocsag_stream(msgs)
        audio = render(bits, a.baud, a.rate, a.amp, a.invert, noise=a.noise)
    else:
        segs = flex_frame_symbols(a.mode, {"A": [{"kind": "alpha", "capcode": 1234567, "text": "FLEX TEST MESSAGE"}]})
        audio = render_symbols(segs, a.rate, a.amp, a.invert, noise=a.noise)
    write_wav(a.out, audio, a.rate)


if __name__ == "__main__":
    main()
