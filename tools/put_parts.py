#!/usr/bin/env python3
"""Upload a file to FlapBoard in 32 KB pieces (/library/upload_part), the way
the web page does. Usage: tools/put_parts.py HOST FILE FOLDER [NAME]"""
import os, random, string, sys, time, urllib.error, urllib.parse, urllib.request

host, path, folder = sys.argv[1], sys.argv[2], sys.argv[3]
name = sys.argv[4] if len(sys.argv) > 4 else os.path.basename(path)
data = open(path, "rb").read()
uid = "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(10))
piece, off, t0 = int(os.environ.get("PIECE", "32768")), 0, time.time()
while off < len(data) or (off == 0 and not data):
    body = data[off:off + piece]
    q = urllib.parse.urlencode({"path": folder, "name": name, "id": uid, "offset": off, "total": len(data)})
    for attempt in range(4):
        try:
            r = urllib.request.urlopen(urllib.request.Request(f"http://{host}/library/upload_part?{q}", body, method="POST"), timeout=15)
            out = r.read().decode()
            break
        except Exception as e:
            if attempt == 3:
                print("FAILED at", off, e); sys.exit(1)
            time.sleep(2 * (attempt + 1))
    off += len(body)
    if not body:
        break
print("%s: %d bytes in %.1f s -> %s" % (name, len(data), time.time() - t0, out))
