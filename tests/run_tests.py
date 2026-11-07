#!/usr/bin/env python3
"""End-to-end tests: synthetic pager audio -> pdw-cli --json -> expected messages.

usage: run_tests.py <path-to-pdw-cli> [--multimon]
With --multimon (and multimon-ng + sox installed) each POCSAG/FLEX case is also
decoded by multimon-ng as an independent cross-check of the generator itself.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen"))
import pagergen as pg  # noqa: E402

POCSAG_MSGS = [
    (1234567, 3, "HELLO WORLD 0123", False),
    (765432, 3, "TEST ALPHA MESSAGE FROM PAGERGEN", False),
    (234567, 0, "5551234 -98", True),
    (1000001, 1, "", False),
]
POCSAG_EXPECT = [
    ("1234567", "ALPHA", "HELLO WORLD 0123"),
    ("0765432", "ALPHA", "TEST ALPHA MESSAGE FROM PAGERGEN"),
    ("0234567", "NUMERIC", "5551234 -98"),
    ("1000001", "NUMERIC", "TONE ONLY"),
]


def run_cli(cli, wav, extra=()):
    r = subprocess.run([cli, "--json", *extra, wav], capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        raise RuntimeError("pdw-cli failed: " + r.stderr)
    return [json.loads(line) for line in r.stdout.splitlines() if line.strip()]


def got_tuples(msgs):
    return [(m["capcode"], m["type"].strip(), m["text"]) for m in msgs]


def case_pocsag(tmp, cli, baud, rate, invert, noise):
    wav = os.path.join(tmp, "p%d_%d_%s.wav" % (baud, rate, "i" if invert else "n"))
    audio = pg.render(pg.pocsag_stream(POCSAG_MSGS), baud, rate, invert=invert, noise=noise)
    pg.write_wav(wav, audio, rate)
    return wav, got_tuples(run_cli(cli, wav)), POCSAG_EXPECT


def case_dc(tmp, cli):
    """FM audio of a mistuned receiver sits on a DC offset; --dc-block must remove it."""
    wav = os.path.join(tmp, "dc.wav")
    audio = pg.render(pg.pocsag_stream(POCSAG_MSGS), 1200, 44100)
    pg.write_wav(wav, [max(-32768, min(32767, v + 4500)) for v in audio], 44100)
    return wav, got_tuples(run_cli(cli, wav, ("--dc-block",))), POCSAG_EXPECT


def case_flex(tmp, cli, rate, invert, noise):
    wav = os.path.join(tmp, "f_%d_%s.wav" % (rate, "i" if invert else "n"))
    segs = pg.flex_frame_symbols("1600/2", {"A": [{"kind": "alpha", "capcode": 1234567, "text": "FLEX TEST MESSAGE"}]})
    audio = pg.render_symbols(segs, rate, invert=invert, noise=noise)
    pg.write_wav(wav, audio, rate)
    return wav, got_tuples(run_cli(cli, wav)), [("1234567", "ALPHA", "FLEX TEST MESSAGE")]


def multimon(wav, kind, baud, invert):
    if not (shutil.which("multimon-ng") and shutil.which("sox")):
        return None
    tmp = wav + ".22k.wav"
    subprocess.run(["sox", wav, "-r", "22050", tmp], check=True, capture_output=True)
    demod = "FLEX" if kind == "flex" else "POCSAG%d" % baud
    r = subprocess.run(["multimon-ng", "-t", "wav", "-a", demod] + (["-i"] if invert and kind != "flex" else []) + [tmp],
                       capture_output=True, text=True)
    return r.stdout


def main():
    cli = sys.argv[1]
    use_mm = "--multimon" in sys.argv
    tmp = tempfile.mkdtemp(prefix="pdwtest_")
    failures = 0
    cases = []
    for baud in (512, 1200, 2400):
        for rate in (44100, 22050, 48000):
            for invert in (False, True):
                cases.append(("pocsag-%d %dHz %s" % (baud, rate, "inverted" if invert else "normal"),
                              lambda b=baud, r=rate, i=invert: case_pocsag(tmp, cli, b, r, i, 0.0), "pocsag", baud, invert))
    for rate in (44100, 22050):
        for invert in (False, True):
            cases.append(("flex-1600 %dHz %s" % (rate, "inverted" if invert else "normal"),
                          lambda r=rate, i=invert: case_flex(tmp, cli, r, i, 0.0), "flex", 1600, invert))
    cases.append(("pocsag-1200 DC offset + --dc-block", lambda: case_dc(tmp, cli), "pocsag", 1200, False))
    cases.append(("pocsag-1200 noisy", lambda: case_pocsag(tmp, cli, 1200, 44100, False, 400.0), "pocsag", 1200, False))
    cases.append(("flex-1600 noisy", lambda: case_flex(tmp, cli, 44100, False, 400.0), "flex", 1600, False))

    for name, fn, kind, baud, invert in cases:
        try:
            wav, got, expect = fn()
        except Exception as e:  # noqa: BLE001
            print("FAIL  %-34s %s" % (name, e))
            failures += 1
            continue
        ok = got == expect
        extra = ""
        if use_mm and ok:
            mm = multimon(wav, kind, baud, invert)
            if mm is not None:
                missing = [t for (_, _, t) in expect if t and t != "TONE ONLY" and t not in mm]
                if missing:
                    extra = "  (multimon missed %s)" % missing
                    ok = False
        print("%s  %-34s%s" % ("ok  " if ok else "FAIL", name, extra))
        if not ok:
            failures += 1
            print("      expected:", expect)
            print("      got:     ", got)
    shutil.rmtree(tmp, ignore_errors=True)
    print("%d case(s), %d failure(s)" % (len(cases), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
