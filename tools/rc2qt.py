#!/usr/bin/env python3
"""Turns upstream's Windows resource script (Rsrc.rc) into data tables for the Qt front end:
dialogs (controls with their dialog-unit geometry and styles), the main menu and the
accelerator table, plus the numeric resource IDs from Headers/Resource.h.

Output: gui/generated/rc_data.cpp and gui/generated/rc_ids.h (checked in; regenerate with
tools/import_upstream.sh). The dialogs are then built at run time by gui/rcdialog.cpp, so
every dialog keeps PDW's layout.
"""
import re
import sys

RC = "upstream/Rsrc.rc"
RES = "upstream/Headers/Resource.h"
OUT_DIR = "gui/generated"


def read(path):
    return open(path, encoding="cp1252").read().replace("\r", "")


def tokenize_args(s):
    """Split a statement's argument list on top-level commas, keeping quoted strings."""
    out, cur, inq, i = [], "", False, 0
    while i < len(s):
        c = s[i]
        if c == '"':
            if inq and i + 1 < len(s) and s[i + 1] == '"':   # "" inside a string
                cur += '"'
                i += 2
                continue
            inq = not inq
            cur += c
        elif c == "," and not inq:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
        i += 1
    if cur.strip():
        out.append(cur.strip())
    return out


def unquote(s):
    s = s.strip()
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
        return s[1:-1].replace('""', '"')
    return s


def cstr(s):
    """Windows-1252 text -> UTF-8 C string literal."""
    s = s.encode("utf-8").decode("utf-8")
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def join_statements(lines):
    """Continuation lines of one control (ends with ',') are merged."""
    out, cur = [], ""
    for l in lines:
        t = l.strip()
        if not t or t.startswith("//"):
            continue
        cur = (cur + " " + t) if cur else t
        if not cur.endswith(","):
            out.append(cur)
            cur = ""
    if cur:
        out.append(cur)
    return out


def main():
    res = read(RES)
    ids = {}
    for m in re.finditer(r"^#define\s+(\w+)\s+(-?\d+)\s*(?://.*)?$", res, re.M):
        ids[m.group(1)] = int(m.group(2))
    ids.setdefault("IDOK", 1)
    ids.setdefault("IDCANCEL", 2)
    ids.setdefault("IDC_STATIC", -1)

    rc = read(RC).split("\n")
    dialogs, menu_lines, accel_lines = [], [], []
    i = 0
    while i < len(rc):
        line = rc[i]
        m = re.match(r"^(\w+)\s+(DIALOG(?:EX)?)\s+(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)", line)
        if m:
            d = {"name": m.group(1), "x": int(m.group(3)), "y": int(m.group(4)), "w": int(m.group(5)),
                 "h": int(m.group(6)), "caption": "", "font": ("Verdana", 8), "style": "", "controls": []}
            i += 1
            body = []
            while rc[i].strip() != "BEGIN":
                t = rc[i].strip()
                if t.startswith("CAPTION"):
                    d["caption"] = unquote(t[len("CAPTION"):])
                elif t.startswith("FONT"):
                    a = tokenize_args(t[len("FONT"):])
                    d["font"] = (unquote(a[1]), int(a[0]))
                elif t.startswith("STYLE"):
                    d["style"] = t[len("STYLE"):].strip()
                i += 1
            i += 1
            while rc[i].strip() != "END":
                body.append(rc[i])
                i += 1
            for st in join_statements(body):
                kw, _, rest = st.partition(" ")
                a = tokenize_args(rest)
                kw = kw.strip()
                c = None
                if kw in ("LTEXT", "CTEXT", "RTEXT", "GROUPBOX", "PUSHBUTTON", "DEFPUSHBUTTON"):
                    c = dict(kind=kw, text=unquote(a[0]), id=a[1], x=a[2], y=a[3], w=a[4], h=a[5],
                             style=a[6] if len(a) > 6 else "", exstyle=a[7] if len(a) > 7 else "", cls="")
                elif kw in ("EDITTEXT", "COMBOBOX", "LISTBOX", "SCROLLBAR"):
                    c = dict(kind=kw, text="", id=a[0], x=a[1], y=a[2], w=a[3], h=a[4],
                             style=a[5] if len(a) > 5 else "", exstyle="", cls="")
                elif kw == "CONTROL":
                    c = dict(kind="CONTROL", text=unquote(a[0]), id=a[1], cls=unquote(a[2]), style=a[3],
                             x=a[4], y=a[5], w=a[6], h=a[7], exstyle=a[8] if len(a) > 8 else "")
                elif kw in ("ICON", "BITMAP"):
                    c = dict(kind=kw, text="", id=a[1] if len(a) > 1 else "0", x=a[2], y=a[3],
                             w=a[4] if len(a) > 4 else "0", h=a[5] if len(a) > 5 else "0", style="", exstyle="", cls="")
                if c:
                    d["controls"].append(c)
            dialogs.append(d)
            continue
        if line.startswith("PDWMENU MENU"):
            i += 1
            depth = 0
            while True:
                t = rc[i].strip()
                menu_lines.append(t)
                if t == "BEGIN":
                    depth += 1
                elif t == "END":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
        if line.startswith("PDWACCEL ACCELERATORS"):
            i += 2
            while rc[i].strip() != "END":
                accel_lines.append(rc[i].strip())
                i += 1
        i += 1

    used = set()

    def idnum(tok):
        tok = tok.strip()
        if re.fullmatch(r"-?\d+", tok):
            return int(tok)
        used.add(tok)
        return ids.get(tok, 0)

    import os
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(OUT_DIR + "/rc_data.cpp", "w") as o:
        o.write("// GENERATED by tools/rc2qt.py from upstream Rsrc.rc. Do not edit. GPL-3.0-or-later, see NOTICE.\n")
        o.write('#include "rc_data.h"\n\nnamespace rc {\n\n')
        o.write("const DialogDef kDialogs[] = {\n")
        for d in dialogs:
            o.write('  {%s, %s, %d, %d, %d, %d, %s, %d, %s, {\n' % (
                cstr(d["name"]), cstr(d["caption"]), d["x"], d["y"], d["w"], d["h"], cstr(d["font"][0]),
                d["font"][1], cstr(d["style"])))
            for c in d["controls"]:
                o.write('    {%s, %s, %d, %s, %s, %s, %s, %s, %s, %s},\n' % (
                    cstr(c["kind"]), cstr(c["text"]), idnum(c["id"]), c["x"], c["y"], c["w"], c["h"],
                    cstr(c["style"]), cstr(c["exstyle"]), cstr(c["cls"])))
            o.write("  }},\n")
        o.write("  {nullptr, nullptr, 0, 0, 0, 0, nullptr, 0, nullptr, {}}};\n\n")

        # menu: nested BEGIN/END with POPUP "x" and MENUITEM "text", ID | SEPARATOR
        o.write("const MenuItem kMenu[] = {\n")
        depth = 0
        for t in menu_lines:
            if t.startswith("POPUP"):
                o.write('  {%s, 0, MenuItem::Popup},\n' % cstr(unquote(t[len("POPUP"):])))
            elif t == "BEGIN":
                depth += 1
                if depth > 1:
                    o.write("  {nullptr, 0, MenuItem::Begin},\n")
            elif t == "END":
                if depth > 1:
                    o.write("  {nullptr, 0, MenuItem::End},\n")
                depth -= 1
            elif t.startswith("MENUITEM SEPARATOR"):
                o.write("  {nullptr, 0, MenuItem::Separator},\n")
            elif t.startswith("MENUITEM"):
                a = tokenize_args(t[len("MENUITEM"):])
                o.write('  {%s, %d, MenuItem::Item},\n' % (cstr(unquote(a[0])), idnum(a[1])))
        o.write("  {nullptr, 0, MenuItem::Eof}};\n\n")

        o.write("const AccelDef kAccel[] = {\n")
        for t in accel_lines:
            a = tokenize_args(t)
            if len(a) < 3:
                continue
            key = unquote(a[0])
            mods = " ".join(a[3:])
            o.write('  {%s, %d, %s, %s, %s},\n' % (
                cstr(key), idnum(a[1]), "true" if "CONTROL" in mods else "false",
                "true" if "ALT" in mods else "false", "true" if "SHIFT" in mods else "false"))
        o.write('  {nullptr, 0, false, false, false}};\n\n}  // namespace rc\n')

    # IDs: every one referenced, plus all the IDM_/IDT_ ones for the front end
    for k in ids:
        if k.startswith(("IDM_", "IDT_", "IDC_", "IDD_", "IDS_")):
            used.add(k)
    with open(OUT_DIR + "/rc_ids.h", "w") as o:
        o.write("// GENERATED by tools/rc2qt.py from upstream Headers/Resource.h. Do not edit.\n#pragma once\n\nnamespace rc {\nenum Id : int {\n")
        for k in sorted(used):
            if k in ids and k not in ("IDC_STATIC", "IDOK", "IDCANCEL"):
                o.write("  %s = %d,\n" % (k, ids[k]))
        o.write("  IDC_STATIC = -1, IDOK = 1, IDCANCEL = 2,\n};\n}  // namespace rc\n")
    print("rc2qt: %d dialogs, %d menu lines, %d accelerators" % (len(dialogs), len(menu_lines), len(accel_lines)))


if __name__ == "__main__":
    main()
