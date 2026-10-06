#!/usr/bin/env python3
"""Install a firmware .bin on a FlapBoard over Wi-Fi, the way the Status
page's "Update firmware" does: 16 KB pieces to /api/ota, each retried, the
whole update started again from the top if the sign restarts mid-way.

    tools/ota_push.py HOST [.pio/build/app/firmware.bin]
"""
import json, random, string, sys, time, urllib.error, urllib.request

host = sys.argv[1]
path = sys.argv[2] if len(sys.argv) > 2 else ".pio/build/app/firmware.bin"
data = open(path, "rb").read()
total, piece = len(data), 16384


def send():
    uid = "".join(random.choices(string.ascii_lowercase, k=10))
    off, t0, last = 0, time.time(), None
    while off < total:
        body = data[off:off + piece]
        for attempt in range(5):
            try:
                req = urllib.request.Request(f"http://{host}/api/ota?id={uid}&offset={off}&total={total}", data=body, method="POST")
                last = json.load(urllib.request.urlopen(req, timeout=30))
                break
            except urllib.error.HTTPError as e:
                if e.code == 409:   # the sign restarted and forgot this update: start again
                    return None
                if e.code < 500:
                    sys.exit(f"HTTP {e.code}: {e.read().decode(errors='replace')}")
            except Exception as e:
                print("retry", off, e, flush=True)
            time.sleep(1.5 * (attempt + 1))
        else:
            sys.exit("the sign stopped answering")
        off += len(body)
    print(last, f"{total / (time.time() - t0) / 1024:.0f} KB/s", flush=True)
    return last


for run in range(3):
    if send():
        break
    print("restarting the update from the beginning", flush=True)
    time.sleep(20)
else:
    sys.exit("update failed three times")
