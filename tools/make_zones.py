#!/usr/bin/env python3
"""Builds assets/zones.txt: one "Area/City<TAB>POSIX TZ rule" line per zone.

The ESP32 keeps time zones as POSIX TZ strings, not the IANA database. Every
TZif (v2+) file ends with exactly that string as a footer, so the table comes
straight from the Mac's own /usr/share/zoneinfo (the tz database is public
domain) -- nothing to download. Re-run after a macOS update to refresh rules.
"""
import os

ZI = "/usr/share/zoneinfo"
names = set()
for tab in ("zone1970.tab", "zone.tab"):
    p = os.path.join(ZI, tab)
    if os.path.exists(p):
        for line in open(p, encoding="utf-8"):
            if line.startswith("#") or not line.strip():
                continue
            names.add(line.split("\t")[2].strip())
names.add("UTC")
rows = []
for n in sorted(names):
    path = os.path.join(ZI, n)
    try:
        data = open(path, "rb").read()
    except OSError:
        continue
    if not data.startswith(b"TZif") or not data.endswith(b"\n"):
        continue
    footer = data[data.rstrip(b"\n").rfind(b"\n") + 1:].strip().decode("ascii", "replace")
    if footer:
        rows.append("%s\t%s" % (n, footer))
os.makedirs("assets", exist_ok=True)
open("assets/zones.txt", "w").write("\n".join(rows) + "\n")
print("wrote %d zones to assets/zones.txt" % len(rows))
