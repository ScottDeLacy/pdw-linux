#!/usr/bin/env python3
"""FLEX tests: all four speeds/level modes, numeric/tone/short-numeric pages, group calls
(short instructions), fragments, missed group calls, polarity and sample-rate variants.

usage: run_flex_tests.py <path-to-pdw-cli> [--multimon]

Audio comes from the independent generator in tests/gen. The expectations follow the original
PDW, which on a sound card slices FSK into two levels only: of the four phases a 4-level
signal carries it decodes A (and C for 6400). Phases B and D are therefore not expected here;
multimon-ng (with --multimon) agrees on everything it can decode.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen"))
import pagergen as pg  # noqa: E402

FAILURES = []
USE_MM = "--multimon" in sys.argv


def check(name, cond, detail=""):
    print(("ok    " if cond else "FAIL  ") + name + ("" if cond else "   " + detail))
    if not cond:
        FAILURES.append(name)


def decode(cli, wav, *args):
    r = subprocess.run([cli, "--json", *args, wav], capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        raise RuntimeError(r.stderr)
    return [json.loads(l) for l in r.stdout.splitlines() if l.strip()]


def tup(m):
    return (m["capcode"], m["type"].strip(), m["mode"], m["text"].strip())


def write(tmp, name, segs, rate=44100, **kw):
    path = os.path.join(tmp, name)
    pg.write_wav(path, pg.render_symbols(segs, rate, **kw), rate)
    return path


def multimon(wav):
    if not (USE_MM and shutil.which("multimon-ng") and shutil.which("sox")):
        return None
    tmp = wav + ".22k.wav"
    subprocess.run(["sox", wav, "-r", "22050", tmp], check=True, capture_output=True)
    r = subprocess.run(["multimon-ng", "-t", "wav", "-a", "FLEX", tmp], capture_output=True, text=True)
    return r.stdout


PAGES_A = [
    {"kind": "alpha", "capcode": 1234567, "text": "ALPHA ONE PHASE A"},
    {"kind": "numeric", "capcode": 1500001, "digits": "5551234 -98"},
    {"kind": "tone", "capcode": 1500002},
    {"kind": "short_numeric", "capcode": 1500003, "digits": "123"},
]
PAGES_C = [{"kind": "alpha", "capcode": 1111113, "text": "PHASE C TEXT"}]


def main():
    cli = os.path.abspath(sys.argv[1])
    tmp = tempfile.mkdtemp(prefix="pdwflex_")

    # ---- speeds / levels ------------------------------------------------------------------
    # (mode, phases decoded by the original on a sound card, PDW's bitrate column)
    for mode, decoded, bitrate in (("1600/2", "A", "1600"), ("3200/4", "A", "3200")):
        pages = {"A": PAGES_A, "B": [{"kind": "alpha", "capcode": 1111112, "text": "PHASE B TEXT"}],
                 "C": PAGES_C, "D": [{"kind": "alpha", "capcode": 1111114, "text": "PHASE D TEXT"}]}
        pages = {p: pages[p] for p in "ABCD" if p in pg.FLEX_MODES[mode][3]}
        for invert in (False, True):
            wav = write(tmp, "m.wav", pg.flex_frame_symbols(mode, pages, 3, 45), invert=invert)
            got = [tup(m) for m in decode(cli, wav)]
            expect = [("1234567", "ALPHA", "FLEX-A", "ALPHA ONE PHASE A"),
                      ("1500001", "StNUM", "FLEX-A", "5551234 -98"),
                      ("1500002", "SH/TONE", "FLEX-A", "TONE-ONLY"),
                      ("1500003", "SH/TONE", "FLEX-A", "123")]
            if "C" in decoded:
                expect.append(("1111113", "ALPHA", "FLEX-C", "PHASE C TEXT"))
            check("flex %s %s" % (mode, "inverted" if invert else "normal"), got == expect, "\n  got    %s\n  expect %s" % (got, expect))
            check("  bitrate column %s" % bitrate, all(m["bitrate"] == bitrate for m in decode(cli, wav)))
            if not invert and mode != "3200/4":    # multimon-ng does not decode 1600-baud 4-level
                mm = multimon(wav)
                if mm is not None:
                    need = ["ALPHA ONE PHASE A"] + (["PHASE C TEXT"] if "C" in decoded else [])
                    check("  multimon agrees (%s)" % mode, all(t in mm for t in need), mm)

    # ---- 3200/2 and 6400/4: phase A and C both decoded -----------------------------------------
    # Which of A/C comes out first depends on the sub-sample timing of the input (the original
    # skips a fixed 1089 samples at the phase crossover), so sweep ten alignments at two rates
    # and require most of them to decode both phases with the right labels.
    for mode, bitrate in (("3200/2", "3200"), ("6400/4", "6400")):
        pages = {p: v for p, v in (("A", PAGES_A), ("C", PAGES_C)) if p in pg.FLEX_MODES[mode][3]}
        for rate in (44100, 48000):
            ok = 0
            for i in range(10):
                wav = write(tmp, "s.wav", pg.flex_frame_symbols(mode, pages, 3, 45), rate=rate, phase=i / 10.0)
                got = {(m["mode"], m["text"].strip()) for m in decode(cli, wav)}
                ok += {("FLEX-A", "ALPHA ONE PHASE A"), ("FLEX-C", "PHASE C TEXT")} <= got
            check("flex %s at %d Hz: both phases in %d/10 alignments (need 6)" % (mode, rate, ok), ok >= 6)
        # fixed alignment: bitrate column and the clean decode of the A-phase traffic
        wav = write(tmp, "m.wav", pg.flex_frame_symbols(mode, pages, 3, 45), phase=0.5)
        check("  bitrate column %s" % bitrate, all(m["bitrate"] == bitrate for m in decode(cli, wav)))

    # ---- sample rates, noise ----------------------------------------------------------------
    # (3200/6400 are not swept over rates: their phase assignment depends on sub-sample timing,
    # see "Known limitations" in the README.)
    for rate in (22050, 48000):
        wav = write(tmp, "r.wav", pg.flex_frame_symbols("1600/2", {"A": PAGES_A}, 3, 45), rate=rate)
        got = [tup(m) for m in decode(cli, wav)]
        check("flex 1600/2 at %d Hz" % rate, len(got) == 4 and got[0][3] == "ALPHA ONE PHASE A", str(got))
    wav = write(tmp, "n.wav", pg.flex_frame_symbols("1600/2", {"A": PAGES_A}, 3, 45), noise=400.0)
    got = [tup(m) for m in decode(cli, wav)]
    check("flex 1600/2 noisy", len(got) == 4 and got[0][3] == "ALPHA ONE PHASE A", str(got))

    # ---- group calls -----------------------------------------------------------------------------
    G = 3
    frames = [
        (0, 10, {"A": [{"kind": "short_instr", "capcode": 1111111, "group": G, "frame": 11},
                       {"kind": "short_instr", "capcode": 1500000, "group": G, "frame": 11},
                       {"kind": "alpha", "capcode": 1333333, "text": "BYSTANDER"}]}),
        (0, 11, {"A": [{"kind": "alpha", "capcode": 2029568 + G, "text": "GROUP MESSAGE TEXT"}]}),
        (0, 12, {"A": [{"kind": "alpha", "capcode": 1444444, "text": "AFTER"}]}),
    ]
    wav = write(tmp, "g.wav", pg.flex_stream("1600/2", frames))
    got = [tup(m) for m in decode(cli, wav)]
    expect = [("1333333", "ALPHA", "FLEX-A", "BYSTANDER"),
              ("1111111", "GROUP", "FLEX-A", "GROUP MESSAGE TEXT"),
              ("1500000", "GROUP", "FLEX-A", "GROUP MESSAGE TEXT"),
              ("2029571", "ALPHA", "FLEX-A", "GROUP MESSAGE TEXT"),
              ("1444444", "ALPHA", "FLEX-A", "AFTER")]
    check("group call converted to per-capcode messages", got == expect, "\n  got    %s\n  expect %s" % (got, expect))
    mm = multimon(wav)
    if mm is not None:
        check("  multimon sees the same group", "002029571 001111111 001500000" in mm and "GROUP MESSAGE TEXT" in mm, mm)

    got = [tup(m) for m in decode(cli, wav, "--no-group")]
    texts = [(c, t, x) for c, t, _, x in got]
    check("ConvertInstr off: instructions shown, group message alone",
          ("1111111", "INSTR", "TEMPORARY ADDRESS: 2029571 -> FRAME 011") in texts and
          ("2029571", "ALPHA", "GROUP MESSAGE TEXT") in texts and not any(t == "GROUP" for _, t, _ in texts), str(texts))

    # FlexGroupMode: one block for the whole group, capcodes listed below it
    out = subprocess.run([cli, "--set", "FlexGroupMode=2", wav], capture_output=True, text=True).stdout
    check("FlexGroupMode shows the group as one entry", re.search(r"GROUP-4 .*GROUP MESSAGE TEXT\n +1111111\n +1500000\n +2029571\n", out) is not None, out)
    out = subprocess.run([cli, "--set", "FlexGroupMode=8", wav], capture_output=True, text=True).stdout
    check("FlexGroupMode hide-groupcodes omits the group address", "2029571" not in out and "1111111" in out, out)

    # ---- missed group call ---------------------------------------------------------------------
    d = os.path.join(tmp, "missed")
    os.makedirs(d)
    frames = [
        (0, 10, {"A": [{"kind": "short_instr", "capcode": 1111111, "group": G, "frame": 11}]}),
        (0, 11, {"A": [{"kind": "alpha", "capcode": 1444444, "text": "NOT THE GROUP"}]}),
        (0, 12, {"A": [{"kind": "alpha", "capcode": 1444445, "text": "LATER"}]}),
    ]
    wav = write(tmp, "miss.wav", pg.flex_stream("1600/2", frames))
    got = [tup(m) for m in decode(cli, wav, "--dir", d, "--set", "LogFileEnabled=0", "--set", "FilterFileEnabled=0")]
    mg = os.path.join(d, "missed-groupcalls.txt")
    check("missed group call is recorded", os.path.isfile(mg) and "1111111" in open(mg).read() and "2029571" in open(mg).read(),
          open(mg).read() if os.path.isfile(mg) else "no file")
    check("other traffic unaffected by the missed group call", [g[3] for g in got] == ["NOT THE GROUP", "LATER"], str(got))

    # ---- fragments ---------------------------------------------------------------------------------
    frames = [
        (0, 20, {"A": [{"kind": "alpha", "capcode": 1234567, "text": "PART ONE ", "frag": 0, "cont": 1}]}),
        (0, 21, {"A": [{"kind": "alpha", "capcode": 1234567, "text": "PART TWO", "frag": 1, "cont": 0}]}),
    ]
    wav = write(tmp, "frag.wav", pg.flex_stream("1600/2", frames))
    msgs = decode(cli, wav)
    check("fragments: bitrate column carries the fragment number",
          [(m["bitrate"], m["text"].strip()) for m in msgs] == [("1601", "PART ONE"), ("1602", "PART TWO")],
          str([(m["bitrate"], m["text"]) for m in msgs]))
    out = subprocess.run([cli, wav], capture_output=True, text=True).stdout
    check("fragments: continuation note shown", "[Continued message - Fragment #2]" in out, out)

    shutil.rmtree(tmp, ignore_errors=True)
    print("%d failure(s)" % len(FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
