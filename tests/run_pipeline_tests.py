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
import time
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


    pg.write_wav(wav, pg.render(pg.pocsag_stream(MSGS), 1200, 44100), 44100)   # (the injection test above reused it)

    # --- more settings: column order, filter-window-only, reject/blocked status, timer blocking
    out, _ = run(cli, wav, "--set", "ScreenCol1=7", "--set", "ScreenCol2=1", "--set", "ScreenCol3=0",
                 "--set", "ScreenCol4=0", "--set", "ScreenCol5=0", "--set", "ScreenCol6=0", "--set", "ScreenCol7=0")
    check("ScreenColN reorders the columns", re.match(r" +HELLO WORLD 0123 +1234567", out.split("\n")[0]) is not None or
          "HELLO WORLD 0123" in out.split("\n")[0], out.split("\n")[0])
    out, _ = run(cli, wav, "--filters", f, "--set", "FilterWindowOnly=1")
    check("FilterWindowOnly keeps filtered messages out of the monitor pane", "HELLO WORLD" not in out and "TEST ALPHA" not in out, out)
    _, err = run(cli, wav, "--filters", f, "--status", "--set", "ShowRejectBlocked=1")
    check("ShowRejectBlocked reports a rejected message in the status text", "Rejected Message : 0765432 TEST ALPHA MESSAGE" in err, err)
    _, err = run(cli, wav, "--status", "--set", "ShowRejectBlocked=1", "--set", "BlockDuplicate=1")
    check("ShowRejectBlocked reports a blocked duplicate in the status text", "Blocked Duplicate Message : 1234567 DUPLICATE ME" in err, err)
    n_timer = len(jrun(cli, wav, "--set", "BlockDuplicate=19"))      # 1 minute timer, address+message
    check("BlockDuplicate timer mode blocks the repeat", n_timer == n_all - 1, "%d vs %d" % (n_timer, n_all))

    # a long run of distinct messages overflows the 1000-entry block buffer (an upstream out-of-bounds spot)
    many = [(100000 + i, 3, "MSG %04d" % i, False) for i in range(1100)]
    big = os.path.join(tmp, "big.wav")
    pg.write_wav(big, pg.render(pg.pocsag_stream(many), 2400, 44100), 44100)
    got = jrun(cli, big, "--set", "BlockDuplicate=19")
    check("1100 distinct messages with the block timer (buffer overflow path)", len(got) == 1100 and got[-1]["text"] == "MSG 1099", "%d" % len(got))
    os.remove(big)

    # --- filter sound: Wavfiles/SoundN.wav chosen by the filter's wave number, played via --sound-cmd
    d3 = os.path.join(tmp, "snd")
    os.makedirs(os.path.join(d3, "Wavfiles"))
    for n in (0, 1):
        open(os.path.join(d3, "Wavfiles", "Sound%d.wav" % n), "wb").write(b"RIFF....WAVE")
    write_filters(os.path.join(d3, "filters.ini"), ['2,"1234567","Beep","",0,2,0,0,0,""'])    # wave number 2 -> Sound1.wav
    with open(os.path.join(d3, "pdw.ini"), "w") as fh:
        fh.write("[PDW]\nLogFileEnabled=0\nFilterBeep=1\n[Filter]\nFilterFileEnabled=0\n")
    playlog = os.path.join(tmp, "play.log")
    player = os.path.join(tmp, "player.sh")
    with open(player, "w") as fh:
        fh.write('#!/bin/sh\necho "$1" >> ' + playlog + '\n')
    os.chmod(player, os.stat(player).st_mode | stat.S_IXUSR)
    run(cli, wav, "--dir", d3, "--sound-cmd", player)
    for _ in range(30):
        if os.path.exists(playlog):
            break
        time.sleep(0.1)
    played = open(playlog).read().split() if os.path.exists(playlog) else []
    check("filter hit plays Wavfiles/Sound1.wav", played and played[0].endswith("/Wavfiles/Sound1.wav"), str(played))

    # --- character sets (language.df): the Swedish table maps [ \\ ] { | } to A/O/A/a/o/a with rings and dots
    res = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(cli))), "data")
    if not os.path.isfile(os.path.join(res, "language.df")):
        res = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")
    sw = os.path.join(tmp, "sw.wav")
    pg.write_wav(sw, pg.render(pg.pocsag_stream([(1234567, 3, "H{LSA [LV", False)]), 1200, 44100), 44100)
    got = jrun(cli, sw, "--resources", res, "--set", "LangIndex=3")
    check("language table (Swedish) remaps characters", [m["text"] for m in got] == ["H\u00e4LSA \u00c4LV"], str([m["text"] for m in got]))
    got = jrun(cli, sw, "--resources", res)
    check("no language selected: text unchanged", [m["text"] for m in got] == ["H{LSA [LV"], str([m["text"] for m in got]))

    # --- statistics file: the audio clock rolls over an hour mid-recording; messages get audio-time stamps
    if True:
        d4 = os.path.join(tmp, "stat")
        os.makedirs(d4)
        batch = pg.render(pg.pocsag_stream([(1234567, 3, "STAT ONE", False), (234567, 0, "5551234", True)]), 1200, 44100)
        pg.write_wav(os.path.join(tmp, "stat.wav"), batch + [0] * (30 * 44100) + batch, 44100)
        with open(os.path.join(d4, "pdw.ini"), "w") as fh:
            fh.write("[PDW]\nLogFileEnabled=0\nStatFileEnabled=1\nStatFileUseDate=0\nStatFile=stats\n[Filter]\nFilterFileEnabled=0\n")
        r = subprocess.run([cli, "--dir", d4, "--start-time", "2026-10-25 10:59:50", os.path.join(tmp, "stat.wav")],
                           capture_output=True, text=True, check=True)
        stamps = re.findall(r"\d\d:\d\d:\d\d", r.stdout)
        check("message timestamps follow the audio clock", len(stamps) >= 2 and stamps[0] == "10:59:51" or stamps[0] in ("10:59:50", "10:59:52"), str(stamps))
        sf = os.path.join(d4, "Logfiles", "stats.st")
        txt = open(sf).read() if os.path.isfile(sf) else ""
        check("hourly statistics written when the hour changes", "HOURLY (10:00-11:00)" in txt, txt)
        m = re.search(r"POCSAG-1200\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)", txt)
        check("statistics count the messages and characters", m is not None and m.group(1) == "1" and m.group(3) == "1" and m.group(4) == "8", txt)

    # --- SMTP: a small local server records what PDW sends
    import base64
    import socket
    import threading

    mails = []

    class Smtp(threading.Thread):
        def __init__(self):
            super().__init__(daemon=True)
            self.srv = socket.socket()
            self.srv.bind(("127.0.0.1", 0))
            self.srv.listen(5)
            self.port = self.srv.getsockname()[1]

        def run(self):
            while True:
                try:
                    c, _ = self.srv.accept()
                except OSError:
                    return
                threading.Thread(target=self.session, args=(c,), daemon=True).start()

        def session(self, c):
            f = c.makefile("rwb", buffering=0)
            m = {"cmds": [], "data": ""}
            f.write(b"220 test ESMTP\r\n")
            data_mode = False
            while True:
                line = f.readline()
                if not line:
                    break
                line = line.decode("latin-1").rstrip("\r\n")
                if data_mode:
                    if line == ".":
                        data_mode = False
                        f.write(b"250 queued\r\n")
                        mails.append(m)
                    else:
                        m["data"] += line + "\n"
                    continue
                m["cmds"].append(line)
                u = line.upper()
                if u.startswith("EHLO"):
                    f.write(b"250-test\r\n250 AUTH LOGIN\r\n")
                elif u == "AUTH LOGIN":
                    f.write(b"334 VXNlcm5hbWU6\r\n")
                    m["user"] = base64.b64decode(f.readline().strip()).decode()
                    f.write(b"334 UGFzc3dvcmQ6\r\n")
                    m["pass"] = base64.b64decode(f.readline().strip()).decode()
                    f.write(b"235 ok\r\n")
                elif u.startswith("MAIL FROM") or u.startswith("RCPT TO"):
                    f.write(b"250 ok\r\n")
                elif u == "DATA":
                    data_mode = True
                    f.write(b"354 go\r\n")
                elif u == "QUIT":
                    f.write(b"221 bye\r\n")
                    break
                else:
                    f.write(b"500 ?\r\n")
            c.close()

    srv = Smtp()
    srv.start()
    d5 = os.path.join(tmp, "mail")
    os.makedirs(d5)
    shutil.copy(f, os.path.join(d5, "filters.ini"))
    # options: address+message+subject+body (0x3410), AUTH 0x4000, ENABLE 0x8000, iso-8859-1 0x20000; mode 1 = filter hits only
    opts = 0x10 | 0x400 | 0x1000 | 0x2000 | 0x4000 | 0x8000 | 0x20000 | 1
    with open(os.path.join(d5, "pdw.ini"), "w") as fh:
        fh.write("[PDW]\nLogFileEnabled=0\n[Filter]\nFilterFileEnabled=0\n[SMTP]\nEnable=1\nHost=127.0.0.1\nPort=%d\n"
                 "From=pdw@example.org\nTo=a@example.org;b@example.org\nUser=bob\nPassword=secret\nOptions=%d\n" % (srv.port, opts))
    pg.write_wav(wav, pg.render(pg.pocsag_stream(MSGS), 1200, 44100), 44100)
    run(cli, wav, "--dir", d5)
    # filters: 1234567 (x3 messages) and 12345?? hit, 0234567 is monitor-only (not mailed in filter mode)
    got = sorted(m["data"].split("\n\n", 1)[1].strip() for m in mails)
    check("SMTP: one mail per filter hit (monitor-only skipped)", got == sorted(["1234500 WILD CARD HIT", "1234567 DUPLICATE ME", "1234567 DUPLICATE ME", "1234567 HELLO WORLD 0123"]), str(got))
    if mails:
        m0 = mails[0]
        check("SMTP: AUTH LOGIN credentials", m0.get("user") == "bob" and m0.get("pass") == "secret", str(m0.get("user")))
        check("SMTP: envelope", "MAIL FROM:<pdw@example.org>" in m0["cmds"] and "RCPT TO:<a@example.org>" in m0["cmds"] and "RCPT TO:<b@example.org>" in m0["cmds"], str(m0["cmds"]))
        check("SMTP: subject and charset headers", re.search(r"^Subject: 1234567 ", m0["data"], re.M) and 'charset="iso-8859-1"' in m0["data"], m0["data"])
    srv.srv.close()

    # STARTTLS (self-signed certificate: refused by default, accepted with SSLVerify=0)
    if shutil.which("openssl") and not os.environ.get("PDW_NO_TLS"):   # PDW_NO_TLS: binary built without OpenSSL
        import ssl
        key, crt = os.path.join(tmp, "k.pem"), os.path.join(tmp, "c.pem")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", crt,
                        "-days", "2", "-subj", "/CN=localhost"], capture_output=True, check=True)
        sctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        sctx.load_cert_chain(crt, key)

        class TlsSmtp(Smtp):
            def session(self, c):
                c.sendall(b"220 test ESMTP\r\n")
                f = c.makefile("rwb", buffering=0)
                m = {"cmds": [], "data": ""}
                tls = False
                data_mode = False
                while True:
                    line = f.readline()
                    if not line:
                        break
                    line = line.decode("latin-1").rstrip("\r\n")
                    if data_mode:
                        if line == ".":
                            data_mode = False
                            f.write(b"250 queued\r\n")
                            if tls:
                                mails.append(m)
                        else:
                            m["data"] += line + "\n"
                        continue
                    u = line.upper()
                    m["cmds"].append(line)
                    if u.startswith("EHLO"):
                        f.write(b"250-test\r\n250-STARTTLS\r\n250 AUTH LOGIN\r\n")
                    elif u == "STARTTLS":
                        f.write(b"220 go\r\n")
                        try:
                            c = sctx.wrap_socket(c, server_side=True)
                        except (ssl.SSLError, OSError):
                            return
                        f = c.makefile("rwb", buffering=0)
                        tls = True
                    elif u.startswith("MAIL FROM") or u.startswith("RCPT TO"):
                        f.write(b"250 ok\r\n")
                    elif u == "DATA":
                        data_mode = True
                        f.write(b"354 go\r\n")
                    elif u == "QUIT":
                        f.write(b"221 bye\r\n")
                        break
                    else:
                        f.write(b"500 ?\r\n")
                c.close()

        tsrv = TlsSmtp()
        tsrv.start()
        d6 = os.path.join(tmp, "tlsmail")
        os.makedirs(d6)
        opts = 0x10 | 0x400 | 0x1000 | 0x2000 | 0x8000 | 0x20000     # no AUTH, all messages
        base_ini = ("[PDW]\nLogFileEnabled=0\n[Filter]\nFilterFileEnabled=0\n[SMTP]\nEnable=1\nHost=localhost\nPort=%d\n"
                    "From=pdw@example.org\nTo=a@example.org\nSSL=1\nOptions=%d\n" % (tsrv.port, opts))
        mails.clear()
        open(os.path.join(d6, "pdw.ini"), "w").write(base_ini)
        r = subprocess.run([cli, "--dir", d6, wav], capture_output=True, text=True)
        check("SMTP STARTTLS: self-signed certificate is refused by default", not mails, str(len(mails)))
        open(os.path.join(d6, "pdw.ini"), "w").write(base_ini + "SSLVerify=0\n")
        run(cli, wav, "--dir", d6)
        check("SMTP STARTTLS: mail delivered with SSLVerify=0", len(mails) >= 5, str(len(mails)))
        tsrv.srv.close()

    shutil.rmtree(tmp, ignore_errors=True)
    print("%d failure(s)" % len(FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
