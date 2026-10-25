#!/usr/bin/env python3
"""Pipeline tests: filters, labels, reject, duplicate blocking, logging, command file, pdw.ini.

usage: run_pipeline_tests.py <path-to-pdw-cli>
Audio comes from the independent generator in tests/gen; expectations follow the behaviour of
the original PDW (filters.ini format, ShowMessage semantics).
"""
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen"))
import pagergen as pg  # noqa: E402

MSGS = [
    (1234567, 3, "HELLO WORLD 0123", False),
    (765432, 3, "TEST ALPHA MESSAGE", False),
    (234567, 0, "5551234 -98", True),
    (1234567, 3, "DUPLICATE ME", False),
    (1234567, 3, "DUPLICATE ME", False),
    (1234500, 3, "WILD CARD HIT", False),
]

FAILURES = []


def check(name, cond, detail=""):
    print(("ok    " if cond else "FAIL  ") + name + ("" if cond else "   " + detail))
    if not cond:
        FAILURES.append(name)


def run(cli, wav, *args, cwd=None):
    r = subprocess.run([cli, *args, wav], capture_output=True, text=True, timeout=120, cwd=cwd)
    if r.returncode != 0:
        raise RuntimeError("pdw-cli failed: " + r.stderr)
    return r.stdout, r.stderr


def jrun(cli, wav, *args, **kw):
    out, _ = run(cli, wav, "--json", *args, **kw)
    return [json.loads(l) for l in out.splitlines() if l.strip()]


def write_filters(path, lines):
    with open(path, "w", newline="") as f:
        f.write("[Filter]\n\nFilterCount=%d\n\n" % len(lines))
        for i, l in enumerate(lines, 1):
            f.write("Filter%d=%s\n" % (i, l))


def main():
    cli = os.path.abspath(sys.argv[1])
    tmp = tempfile.mkdtemp(prefix="pdwpipe_")
    wav = os.path.join(tmp, "p.wav")
    pg.write_wav(wav, pg.render(pg.pocsag_stream(MSGS), 1200, 44100), 44100)

    # --- pane layout: PDW's columns, blank separator line between different messages
    out, _ = run(cli, wav)
    lines = out.split("\n")
    first = lines[0]
    check("pane line layout", re.fullmatch(r" +1234567 +\d\d:\d\d:\d\d \d\d-\d\d-\d\d POCSAG-\d +ALPHA +1200 +HELLO WORLD 0123", first) is not None, repr(first))
    check("separator line after a new message", lines[1] == " ", repr(lines[1]))
    dup = [l for l in lines if "DUPLICATE ME" in l]
    idx = [i for i, l in enumerate(lines) if "DUPLICATE ME" in l]
    check("identical repeat is not separated", len(dup) == 2 and idx[1] == idx[0] + 1, str(idx))

    # --- filters: address match + label, reject, monitor-only, wildcard, text, label %-codes
    f = os.path.join(tmp, "filters.ini")
    write_filters(f, [
        '2,"1234567","Station %1","",0,0,0,2,0,""',        # hit, label colour 2 -> '1'... label with %1
        '2,"0765432","","",1,0,0,0,0,""',                    # reject
        '2,"0234567","Numbers only","",2,0,0,3,0,""',        # monitor only
        '2,"12345??","Wild","",0,0,0,5,0,""',                # wildcard
        '3,"","","DUPLICATE",0,0,0,0,0,""',                  # text filter
    ])
    msgs = jrun(cli, wav, "--filters", f)
    by = {}
    for m in msgs:
        by.setdefault(m["capcode"], []).append(m)
    check("reject filter hides the message", "0765432" not in by, str(list(by)))
    hello = by["1234567"][0]
    check("address filter: filtered pane + label", hello["filtered"] and hello["filter"] == 0 and hello["label"].startswith("Station "), str(hello))
    check("label %-code expansion", hello["label"] == "Station 1", hello["label"])
    num = by["0234567"][0]
    check("monitor-only filter", num["monitor_only"] and not num["filtered"] and num["filter"] == 2 and num["label"] == "Numbers only", str(num))
    wild = by["1234500"][0]
    check("wildcard filter (12345??)", wild["filtered"] and wild["filter"] == 3, str(wild))
    dupm = [m for m in by["1234567"] if m["text"] == "DUPLICATE ME"]
    check("filters are checked in order (address filter wins over text filter)", all(m["filter"] == 0 for m in dupm), str(dupm))

    out, _ = run(cli, wav, "--filters", f, "--filtered")
    check("filter pane carries only filtered messages", "HELLO WORLD" in out and "TEST ALPHA" not in out and "5551234" not in out, out)

    # --- duplicate blocking
    n_all = len(jrun(cli, wav))
    n_blocked = len(jrun(cli, wav, "--set", "BlockDuplicate=1"))
    check("BlockDuplicate=1 drops the repeated message", n_blocked == n_all - 1, "%d vs %d" % (n_blocked, n_all))

    # --- --dir: logging, blocked.txt, pdw.ini compat (Windows CRLF + backslash paths), save
    d = os.path.join(tmp, "pdwdir")
    os.makedirs(d)
    shutil.copy(f, os.path.join(d, "filters.ini"))
    ini = "\r\n".join([
        "[PDW]", "LogFileEnabled=1", "LogFileUseDate=0", "LogFile=test.log", "LogfilePath=C:\\PDW\\Logfiles",
        "Pocsag2400=0", "BlockDuplicate=5", "ColLogfile=1267",
        "", "[Filter]", "FilterFileEnabled=1", "FilterFile=hits.flt", "FilterFileUseDate=0", ""])
    with open(os.path.join(d, "pdw.ini"), "w", newline="") as fh:
        fh.write(ini)
    base = jrun(cli, wav, "--dir", d, "--set", "BlockDuplicate=0")
    for fn in ("blocked.txt", "Logfiles", "filters.ini"):  # keep the baseline run from polluting the checks below
        pass
    shutil.copy(f, os.path.join(d, "filters.ini"))
    shutil.rmtree(os.path.join(d, "Logfiles"), ignore_errors=True)
    msgs = jrun(cli, wav, "--dir", d)
    check("pdw.ini is honoured (BlockDuplicate=5 drops the repeat)", len(msgs) == len(base) - 1, "%d vs %d" % (len(msgs), len(base)))
    logdir = os.path.join(d, "Logfiles")
    log = os.path.join(logdir, "test.log")
    check("logfile written under Logfiles/ (backslash drive path ignored)", os.path.isfile(log), str(os.listdir(d)))
    if os.path.isfile(log):
        txt = open(log).read()
        check("logfile columns follow ColLogfile", re.search(r"^1234567 \d\d:\d\d:\d\d 1200  HELLO WORLD 0123$", txt, re.M) is not None, txt)
        check("logfile has no date column (ColLogfile=1267)", not re.search(r"\d\d-\d\d-\d\d", txt), txt)
    hits = os.path.join(logdir, "hits.flt")
    check("filter file written", os.path.isfile(hits) and "HELLO WORLD" in open(hits).read() if os.path.isfile(hits) else False)
    blocked = os.path.join(d, "blocked.txt")
    check("blocked.txt written (BlockDuplicate bit 4)", os.path.isfile(blocked) and "DUPLICATE ME" in open(blocked).read() if os.path.isfile(blocked) else False)
    flt = open(os.path.join(d, "filters.ini")).read()
    check("filters.ini hit counters saved", re.search(r'^2,"1234567","Station %1".*,\d+,\d\d-\d\d-\d\d,\d\d:\d\d:\d\d$', flt, re.M) is not None, flt)

    # save + reload round trip
    run(cli, wav, "--dir", d, "--save-settings")
    saved = open(os.path.join(d, "pdw.ini")).read()
    check("--save-settings writes pdw.ini", saved.startswith("[PDW]") and "Pocsag2400=0" in saved and "BlockDuplicate=5" in saved, saved[:200])
    a = jrun(cli, wav, "--dir", d)
    check("saved settings reload identically", [m["text"] for m in a] == [m["text"] for m in msgs])

    # --- command file: runs without a shell, message words arrive as separate arguments
    d2 = os.path.join(tmp, "cmddir")
    os.makedirs(d2)
    outfile = os.path.join(tmp, "cmd.out")
    script = os.path.join(tmp, "cmd.sh")
    with open(script, "w") as fh:
        fh.write('#!/bin/sh\nprintf "%s|" "$@" >> ' + outfile + '\n')
    os.chmod(script, os.stat(script).st_mode | stat.S_IXUSR)
    write_filters(os.path.join(d2, "filters.ini"), ['2,"0765432","","",0,0,1,0,0,""'])
    with open(os.path.join(d2, "pdw.ini"), "w") as fh:
        fh.write("[PDW]\nLogFileEnabled=0\n[Filter]\nFilterFileEnabled=0\nFilterCmdFileEnabled=1\n"
                 "FilterCmdFile=%s\nFilterCmdArgs=\"%%1 \\\"%%7\\\"\"\n" % script)
    run(cli, wav, "--dir", d2)
    import time
    for _ in range(30):
        if os.path.exists(outfile):
            break
        time.sleep(0.1)
    got = open(outfile).read() if os.path.exists(outfile) else ""
    check("command file runs with %-arguments", got.startswith("0765432|") and "TEST ALPHA MESSAGE" in got, repr(got))

    # shell metacharacters in a message must stay data
    marker = os.path.join(tmp, "pwned")
    pg.write_wav(wav, pg.render(pg.pocsag_stream([(765432, 3, "X; touch %s" % marker, False)]), 1200, 44100), 44100)
    run(cli, wav, "--dir", d2)
    time.sleep(0.5)
    check("no shell injection through message text", not os.path.exists(marker))

    shutil.rmtree(tmp, ignore_errors=True)
    print("%d failure(s)" % len(FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
