#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Turn config/AmbientVoiceSubtitles.ini into src/ini_template.h (C string).

Why this step exists: the plugin writes a default .ini when the file is missing.
That default used to be a hand-kept copy of the .ini text inside the C source,
which silently drifted (it kept saying FadeStartM=5 long after the real ini said
10). Now the .ini file is the single source of truth: edit the ini, run this
script, rebuild -- the embedded template follows.

Usage (from the repository root):
    python tools/gen_ini_template.py
    python tools/gen_ini_template.py --ini config/AmbientVoiceSubtitles.ini --out src/ini_template.h

Output: src/ini_template.h (UTF-8, no BOM, CRLF), containing
    static const char AVS_INI_TEMPLATE[]  -- byte-identical to the .ini
    AVS_INI_TEMPLATE_LEN                  -- sizeof - 1
"""
import argparse
import hashlib
import io
import os
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_INI = os.path.join(ROOT, "config", "AmbientVoiceSubtitles.ini")
DEFAULT_OUT = os.path.join(ROOT, "src", "ini_template.h")


def c_escape(data):
    out = []
    for b in bytearray(data):
        if b == 0x0A:
            out.append("\\n")
        elif b == 0x0D:
            out.append("\\r")
        elif b == 0x09:
            out.append("\\t")
        elif b == 0x22:          # "
            out.append('\\"')
        elif b == 0x5C:          # backslash
            out.append("\\\\")
        elif 0x20 <= b < 0x7F:
            out.append(chr(b))
        else:
            # always 3-digit octal: \x would swallow following hex digits
            out.append("\\%03o" % b)
    return "".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ini", default=DEFAULT_INI)
    ap.add_argument("--out", default=DEFAULT_OUT)
    a = ap.parse_args()

    with open(a.ini, "rb") as fh:
        raw = fh.read()
    if not raw:
        print("ERROR: empty ini: %s" % a.ini)
        return 1
    if raw[:3] == b"\xef\xbb\xbf":
        print("WARN: ini starts with a UTF-8 BOM; the template keeps it")

    sha = hashlib.sha256(raw).hexdigest()
    crlf = raw.count(b"\r\n")
    lf = raw.count(b"\n")

    body = []
    for line in raw.split(b"\n")[:-1] if raw.endswith(b"\n") else raw.split(b"\n"):
        body.append(c_escape(line + b"\n"))
    tail = ""
    if not raw.endswith(b"\n"):
        tail = c_escape(raw.split(b"\n")[-1])
        body.pop()

    lines = []
    lines.append("/* ini_template.h -- generated, do not edit by hand")
    lines.append(" * source : %s" % os.path.relpath(a.ini, ROOT).replace("\\", "/"))
    lines.append(" *          %d B, sha256 %s" % (len(raw), sha))
    lines.append(" * newline: CRLF %d / all LF %d" % (crlf, lf))
    lines.append(" * tool   : tools/gen_ini_template.py")
    lines.append(" * when   : %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    lines.append(" * used   : AVS_INI_TEMPLATE / AVS_INI_TEMPLATE_LEN are written out")
    lines.append(" *          when AmbientVoiceSubtitles.ini is missing.")
    lines.append(" */")
    lines.append("#ifndef AVS_INI_TEMPLATE_H")
    lines.append("#define AVS_INI_TEMPLATE_H")
    lines.append("")
    lines.append("static const char AVS_INI_TEMPLATE[] =")
    for s in body:
        lines.append('    "%s"' % s)
    if tail:
        lines.append('    "%s";' % tail)
    else:
        lines.append("    ;")
    lines.append("")
    lines.append("static const unsigned int AVS_INI_TEMPLATE_LEN =")
    lines.append("    (unsigned int)(sizeof(AVS_INI_TEMPLATE) - 1);")
    lines.append("")
    # a template this small means the ini failed to load; fail the build instead
    lines.append("typedef char avs_ini_template_size_check[")
    lines.append("    (sizeof(AVS_INI_TEMPLATE) > 512) ? 1 : -1];")
    lines.append("")
    lines.append("#endif /* AVS_INI_TEMPLATE_H */")
    text = "\r\n".join(lines) + "\r\n"

    with io.open(a.out, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    print("ini    : %s (%d B, sha256 %s)" % (a.ini, len(raw), sha[:16]))
    print("header : %s (%d B)" % (a.out, os.path.getsize(a.out)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
