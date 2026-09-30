#!/usr/bin/env python3
"""Talk to the Phase 0 probe: probe.py [SECS] [CMD@DELAY ...]

Opening the port resets the Tab5, so the whole session is one invocation:
it prints everything for SECS seconds and sends each CMD after DELAY s.
rts/dtr must be False before open() or the P4 is held in reset.
"""
import glob, sys, time
import serial

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
cmds = []
for a in sys.argv[2:]:
    c, _, d = a.partition("@")
    cmds.append((float(d or 0), c))
s = serial.Serial()
s.port = sorted(glob.glob("/dev/cu.usbmodem*"))[0]
s.baudrate = 115200
s.timeout = 0.2
s.rts = False
s.dtr = False
s.open()
t0 = time.time()
while time.time() - t0 < secs:
    while cmds and time.time() - t0 >= cmds[0][0]:
        s.write((cmds.pop(0)[1] + "\n").encode())
    data = s.read(4096)
    if data:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
