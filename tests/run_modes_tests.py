#!/usr/bin/env python3
"""ACARS (and the other non-paging modes) end to end, plus a robustness pass: noise and corrupted
signals in every mode must not crash the decoder (run it against an ASan/UBSan build).

usage: run_modes_tests.py <path-to-pdw-cli>
"""
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen"))
import pagergen as pg  # noqa: E402

FAILURES = []


def check(name, cond, detail=""):
    print(("ok    " if cond else "FAIL  ") + name + ("" if cond else "   " + detail))
    if not cond:
        FAILURES.append(name)


def run(cli, wav, *args):
    r = subprocess.run([cli, *args, wav], capture_output=True, text=True, timeout=300)
    return r


def jmsgs(cli, wav, *args):
    r = run(cli, wav, "--json", *args)
    if r.returncode != 0:
        raise RuntimeError(r.stderr[-2000:])
    return [json.loads(l) for l in r.stdout.splitlines() if l.strip()]


ACARS = ["--set", "PocsagFlex=0", "--set", "Acars=1"]


def main():
    cli = os.path.abspath(sys.argv[1])
    tmp = tempfile.mkdtemp(prefix="pdwmodes_")
    wav = os.path.join(tmp, "a.wav")

    # ---- ACARS ----------------------------------------------------------------------------------
    for rate in (44100, 22050, 48000):
        for invert in (False, True):
            bits = pg.acars_bits(".VH-ABC", "SAMPLE ACARS TEXT 123")
            pg.write_wav(wav, pg.render_acars(bits, rate, invert=invert), rate)
            m = jmsgs(cli, wav, *ACARS)
            ok = len(m) == 1 and m[0]["capcode"] == "VH-ABC" and m[0]["mode"] == "M01A" and m[0]["type"] == "4" \
                and "SAMPLE ACARS TEXT 123" in m[0]["text"] and "Flight Number : QF0001" in m[0]["text"]
            check("acars %d Hz %s" % (rate, "inverted" if invert else "normal"), ok, str(m))

    bits = pg.acars_bits(".VH-ABC", "SAMPLE ACARS TEXT 123")
    pg.write_wav(wav, pg.render_acars(bits, 44100, amp=20000, noise=300.0), 44100)
    m = jmsgs(cli, wav, *ACARS)
    check("acars noisy", len(m) == 1 and "SAMPLE ACARS TEXT 123" in m[0]["text"], str(m))

    # parity checking: one corrupted character is rejected when AcarsParityCheck=1, shown otherwise
    bad = list(bits)
    pos = 16 * 8 + 5 * 8 + 8 * 12 + 2           # a bit inside the message text
    bad[pos] ^= 1
    pg.write_wav(wav, pg.render_acars(bad, 44100), 44100)
    strict = jmsgs(cli, wav, *ACARS, "--set", "AcarsParityCheck=1")
    loose = jmsgs(cli, wav, *ACARS, "--set", "AcarsParityCheck=0")
    check("acars parity check rejects a damaged block", len(strict) == 0 and len(loose) == 1, "%d / %d" % (len(strict), len(loose)))

    # two blocks, a long text, a label
    long_text = "THIS IS A MUCH LONGER ACARS MESSAGE " * 4
    pg.write_wav(wav, pg.render_acars(pg.acars_bits(".N12345 ".strip()[:7].ljust(7, "6"), long_text, msg_no="M02B", flight="VA0123"), 44100), 44100)
    m = jmsgs(cli, wav, *ACARS)
    check("acars long text", len(m) == 1 and long_text.strip() in m[0]["text"].replace("\x17", ""), str(m)[:300])

    # ---- robustness: nothing may crash on garbage, in any mode ------------------------------------
    rng = random.Random(7)
    noise = os.path.join(tmp, "noise.wav")
    samples = []
    for level in (50, 400, 3000, 12000):
        samples += [int(rng.gauss(0, level)) for _ in range(44100 * 3)]
    pg.write_wav(noise, [max(-32768, min(32767, s)) for s in samples], 44100)
    for name, args in (("pocsag/flex", []), ("acars", ACARS), ("mobitex", ["--set", "PocsagFlex=0", "--set", "Mobitex=1"]),
                       ("ermes", ["--set", "PocsagFlex=0", "--set", "Ermes=1"])):
        r = run(cli, noise, *args)
        check("noise in %s mode: no crash" % name, r.returncode == 0, r.stderr[-1500:])

    # valid signals with burst errors, decoded in every mode
    pg.write_wav(wav, pg.render(pg.pocsag_stream([(1234567, 3, "ROBUST TEST", False)]), 1200, 44100), 44100)
    raw = pg.render(pg.pocsag_stream([(1234567, 3, "ROBUST TEST", False)] * 20), 1200, 44100)
    for i in range(0, len(raw), 9000):
        raw[i:i + 700] = [rng.randint(-20000, 20000) for _ in range(len(raw[i:i + 700]))]
    pg.write_wav(wav, raw, 44100)
    for name, args in (("pocsag/flex", []), ("acars", ACARS), ("mobitex", ["--set", "PocsagFlex=0", "--set", "Mobitex=1"])):
        r = run(cli, wav, *args)
        check("burst errors in %s mode: no crash" % name, r.returncode == 0, r.stderr[-1500:])

    shutil.rmtree(tmp, ignore_errors=True)
    print("%d failure(s)" % len(FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
