#!/usr/bin/env python3
"""Synthetic pager-signal generator for testing pdw-cli.

Builds POCSAG and FLEX (1600 bps, 2-level) bit streams from the public protocol
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


# ---------------------------------------------------------------- FLEX 1600/2
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


def flex_alpha_frame(capcode, text, cycle=0, frame=1):
    """One FLEX frame (88 words) carrying a single short-address alpha message."""
    chars = [0x20] + [ord(c) & 0x7F for c in text] + [3]       # 1st char of 1st content word is skipped by decoders
    # first message word: fragment bits (11..12)=3 marks a complete message; PDW skips
    # the character in bits 0..6 of that word.
    msg_words = []
    first = (3 << 11)
    msg_words.append(first)
    stream = chars[:]
    # remaining chars: header word carries two chars at bits 7..13/14..20 in PDW's reader
    # which overlaps the fragment field, so keep the header's char slots empty and start
    # the text in the following words (3 chars/word).
    while len(stream) % 3:
        stream.append(3)
    for i in range(0, len(stream), 3):
        msg_words.append(stream[i] | stream[i + 1] << 7 | stream[i + 2] << 14)
    biw_addr_start = 1
    n_addr = 1
    vsa = biw_addr_start + n_addr
    w1 = vsa + n_addr                                       # first message word index
    length = len(msg_words)
    biw = nibble_sum_fix((vsa << 10) | ((biw_addr_start - 1) << 8))
    addr = (capcode + 32768) & 0x1FFFFF
    vec = nibble_sum_fix((5 << 4) | (w1 << 7) | (length << 14))
    words21 = [biw, addr, vec] + msg_words
    words21 += [0x1FFFFF] * (88 - len(words21))             # idle fill
    return words21


def flex_bits(frames_words21, cycle=0, frame_no=1):
    bits = []
    bits += [1, 0] * 32                                       # dotting
    bits += bits_of(0x870C, 16) + bits_of(0xA6C6, 16) + bits_of(0xAAAA, 16) + bits_of(0x78F3, 16)
    # Frame information word: same word coding as data words; value is
    # checksum(0..3) | cycle(4..7) | frame(8..14) | 6 more bits (zero)
    bits += [1, 0] * 8                                        # 16 bit gap before the FIW
    bits += word_bits(flex_word(nibble_sum_fix((cycle << 4) | (frame_no << 8))))
    bits += [1, 0] * 20                                       # sync 2 (40 bits)
    # 11 blocks of 8 words, 32 bit words interleaved bit-by-bit
    words32 = [flex_word(w) for w in frames_words21]
    for blk in range(11):
        group = words32[blk * 8:(blk + 1) * 8]
        for j in range(32):
            for w in group:
                bits.append((w >> (31 - j)) & 1)
    return bits


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
        words = flex_alpha_frame(1234567, "FLEX TEST MESSAGE")
        bits = flex_bits(words)
        audio = render(bits, 1600, a.rate, a.amp, a.invert, noise=a.noise)
    write_wav(a.out, audio, a.rate)


if __name__ == "__main__":
    main()
