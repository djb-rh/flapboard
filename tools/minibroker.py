#!/usr/bin/env python3
"""A tiny MQTT 3.1.1 broker for testing FlapBoard without touching a real
Home Assistant: CONNECT (optional user/password check), SUBSCRIBE (+/#
wildcards), PUBLISH QoS 0/1 with retained messages, PUBACK, PINGREQ, and the
last will on an unclean drop. Logs every publish. Not for real use.

  tools/minibroker.py [--port 1883] [--user U --password P] [--log FILE]
  Commands can be injected by writing "topic payload" lines to FILE.cmd.
"""
import argparse, os, selectors, socket, struct, sys, time

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=1883)
ap.add_argument("--user"); ap.add_argument("--password")
ap.add_argument("--log", default="/tmp/minibroker.log")
a = ap.parse_args()
log = open(a.log, "a", buffering=1)
def L(*x): log.write(time.strftime("%H:%M:%S ") + " ".join(str(i) for i in x) + "\n")

sel = selectors.DefaultSelector()
clients = {}   # sock -> dict(buf, subs, id, will)
retained = {}

def enc_len(n):
    out = b""
    while True:
        d, n = n % 128, n // 128
        out += bytes([d | (0x80 if n else 0)])
        if not n: return out

def pkt(t, body): return bytes([t]) + enc_len(len(body)) + body
def s16(b): return struct.pack(">H", len(b)) + b

def match(flt, topic):
    f, t = flt.split("/"), topic.split("/")
    for i, p in enumerate(f):
        if p == "#": return True
        if i >= len(t) or (p != "+" and p != t[i]): return False
    return len(f) == len(t)

def deliver(topic, payload, retain=False, origin=None):
    for s, c in list(clients.items()):
        for flt in c["subs"]:
            if match(flt, topic):
                try: s.send(pkt(0x30 | (1 if retain else 0), s16(topic.encode()) + payload))
                except OSError: pass
                break

def publish(topic, payload, retain, origin=None):
    L("PUB", topic, "(retained)" if retain else "", payload[:300].decode("utf-8", "replace"))
    if retain:
        if payload: retained[topic] = payload
        else: retained.pop(topic, None)
    deliver(topic, payload, False, origin)

def drop(s, clean):
    c = clients.pop(s, None)
    try: sel.unregister(s)
    except Exception: pass
    s.close()
    if c:
        L("DROP", c["id"], "clean" if clean else "UNCLEAN")
        if not clean and c["will"]: publish(*c["will"])

def handle(s, c, t, flags, body):
    if t == 1:   # CONNECT
        i = 2 + struct.unpack(">H", body[:2])[0] + 1   # protocol name + level
        cf = body[i]; i += 1 + 2
        def rd():
            nonlocal i
            n = struct.unpack(">H", body[i:i+2])[0]; v = body[i+2:i+2+n]; i += 2 + n; return v
        c["id"] = rd().decode()
        if cf & 4:
            wt = rd().decode(); wm = rd(); c["will"] = (wt, wm, bool(cf & 0x20))
        user = rd().decode() if cf & 0x80 else None
        pw = rd().decode() if cf & 0x40 else None
        ok = a.user is None or (user == a.user and pw == a.password)
        L("CONNECT", c["id"], "user", user, "will", c["will"][0] if c["will"] else None, "OK" if ok else "REFUSED")
        s.send(pkt(0x20, bytes([0, 0 if ok else 5])))
        if not ok: drop(s, True)
    elif t == 3:   # PUBLISH
        qos = (flags >> 1) & 3
        n = struct.unpack(">H", body[:2])[0]; topic = body[2:2+n].decode(); i = 2 + n
        if qos: pid = body[i:i+2]; i += 2; s.send(pkt(0x40, pid))
        publish(topic, body[i:], bool(flags & 1), s)
    elif t == 8:   # SUBSCRIBE
        pid = body[:2]; i = 2; codes = b""
        while i < len(body):
            n = struct.unpack(">H", body[i:i+2])[0]; flt = body[i+2:i+2+n].decode(); i += 2 + n + 1
            c["subs"].append(flt); codes += b"\x01"; L("SUB", c["id"], flt)
            for tp, pl in retained.items():
                if match(flt, tp): s.send(pkt(0x31, s16(tp.encode()) + pl))
        s.send(pkt(0x90, pid + codes))
    elif t == 12: s.send(pkt(0xD0, b""))   # PINGREQ
    elif t == 14: drop(s, True)            # DISCONNECT

srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", a.port)); srv.listen(); srv.setblocking(False)
sel.register(srv, selectors.EVENT_READ)
L("listening on", a.port)
cmdfile = a.log + ".cmd"
open(cmdfile, "w").close()
while True:
    for key, _ in sel.select(0.2):
        if key.fileobj is srv:
            s, addr = srv.accept(); s.setblocking(False); sel.register(s, selectors.EVENT_READ)
            clients[s] = {"buf": b"", "subs": [], "id": "?", "will": None}; L("ACCEPT", addr)
            continue
        s = key.fileobj; c = clients.get(s)
        try: data = s.recv(65536)
        except OSError: data = b""
        if not data: drop(s, False); continue
        c["buf"] += data
        while len(c["buf"]) >= 2:
            b = c["buf"]; mul, n, i = 1, 0, 1
            while True:
                if i >= len(b): break
                n += (b[i] & 127) * mul; mul *= 128; i += 1
                if not b[i-1] & 128: break
            if i > len(b) or len(b) < i + n: break
            t, flags, body = b[0] >> 4, b[0] & 15, b[i:i+n]
            c["buf"] = b[i+n:]
            handle(s, c, t, flags, body)
            if s not in clients: break
    # injected commands: "topic payload"
    try:
        lines = open(cmdfile).read().splitlines()
        if lines:
            open(cmdfile, "w").close()
            for ln in lines:
                tp, _, pl = ln.partition(" "); publish(tp, pl.encode(), False)
    except OSError: pass
