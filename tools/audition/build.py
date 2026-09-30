#!/usr/bin/env python3
"""Build the clack audition page: tools/audition/template.html with every
sounds/<set>/*.wav embedded as base64 (one self-contained file).
Usage: tools/audition/build.py <out.html> [sound dir ...]  (default: sounds/generated)"""
import base64, glob, json, os, re, sys
out = sys.argv[1]
dirs = sys.argv[2:] or ["sounds/generated"]
sounds = {}
for d in dirs:
    for path in sorted(glob.glob(os.path.join(d, "*.wav"))):
        style = re.sub(r"_\d+$", "", os.path.splitext(os.path.basename(path))[0])
        sounds.setdefault(style, []).append(base64.b64encode(open(path, "rb").read()).decode())
tpl = open(os.path.join(os.path.dirname(__file__), "template.html")).read()
open(out, "w").write(tpl.replace("__SOUNDS__", json.dumps(sounds)))
print("wrote %s: %d styles, %d clips, %d KB" % (out, len(sounds), sum(map(len, sounds.values())), os.path.getsize(out) // 1024))
