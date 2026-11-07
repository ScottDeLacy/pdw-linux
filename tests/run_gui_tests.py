#!/usr/bin/env python3
"""Front-end tests (offscreen Qt): play a WAV, and take audio the way rtl_fm delivers it.

usage: run_gui_tests.py <path-to-pdw>
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "gen"))
import pagergen as pg  # noqa: E402

FAILURES = []


def check(name, cond, detail=""):
    print(("ok    " if cond else "FAIL  ") + name + ("" if cond else "   " + detail))
    if not cond:
        FAILURES.append(name)


def main():
    pdw = os.path.abspath(sys.argv[1])
    tmp = tempfile.mkdtemp(prefix="pdwgui_")
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    msgs = [(1234567, 3, "GUI HELLO 123", False), (234567, 0, "5551234", True)]
    audio = pg.render(pg.pocsag_stream(msgs), 1200, 22050)
    wav = os.path.join(tmp, "g.wav")
    pg.write_wav(wav, audio, 22050)
    pcm = os.path.join(tmp, "g.raw")
    with open(pcm, "wb") as f:
        import struct
        f.write(struct.pack("<%dh" % len(audio), *audio))

    out = os.path.join(tmp, "panes.txt")
    r = subprocess.run([pdw, "--dir", os.path.join(tmp, "d1"), "--play", wav, "--fast", "--dump-panes", out],
                       env=env, capture_output=True, text=True, timeout=60)
    txt = open(out).read() if os.path.exists(out) else ""
    check("window plays a WAV file", r.returncode == 0 and "GUI HELLO 123" in txt and "5551234" in txt, r.stderr[-500:] + txt)
    check("pane lines use PDW's column layout", "POCSAG-" in txt and "ALPHA" in txt and "NUMERIC" in txt, txt)

    # rtl_fm as a child process (a stand-in that replays the audio at 22.05 kHz)
    out2 = os.path.join(tmp, "panes2.txt")
    env2 = dict(env, FAKE_RTL_PCM=pcm)
    r = subprocess.run([pdw, "--dir", os.path.join(tmp, "d2"), "--rtl", "152.4", "--rtl-program", os.path.join(HERE, "fake_rtl_fm.py"),
                        "--dump-panes", out2], env=env2, capture_output=True, text=True, timeout=60)
    txt = open(out2).read() if os.path.exists(out2) else ""
    check("rtl_fm child process feeds the decoder", "GUI HELLO 123" in txt, r.stderr[-500:] + txt)

    # raw audio on stdin, as `rtl_fm ... | pdw --stdin`
    out3 = os.path.join(tmp, "panes3.txt")
    with open(pcm, "rb") as f:
        r = subprocess.run([pdw, "--dir", os.path.join(tmp, "d3"), "--stdin", "--rate", "22050", "--dump-panes", out3],
                           env=env, stdin=f, capture_output=True, text=True, timeout=60)
    txt = open(out3).read() if os.path.exists(out3) else ""
    check("raw audio on stdin", "GUI HELLO 123" in txt, r.stderr[-500:] + txt)

    # settings written at exit are read back (window state, filters hit counters)
    ini = os.path.join(tmp, "d1", "pdw.ini")
    check("pdw.ini is written when the window closes", os.path.exists(ini) and "[PDW]" in open(ini).read())

    print("%d failure(s)" % len(FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
