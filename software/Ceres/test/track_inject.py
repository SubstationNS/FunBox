#!/usr/bin/env python3
# Drives the Ceres tracks over USB serial (no footswitches, no audio cables needed): a test tone
# goes into the inputs, footswitch presses are injected, and the per-bar status shows what the
# tracks do and how loud their loops are.
#
#   python3 track_inject.py [/dev/ttyACM0]
#
# Script (120 BPM, mono mode, levels at unity):
#   T1: record 2 bars of tone -> plays (~ -13 dB) -> overdub 2 bars -> louder -> undo -> redo
#   T2: record 1 bar, double tap -> stop, tap -> plays from the next bar
#   both footswitches -> stop all -> both again -> start all
#   hold T1 2 s -> clear
import glob, os, sys, termios, threading, time

port = sys.argv[1] if len(sys.argv) > 1 else (glob.glob("/dev/serial/by-id/*Daisy*") or ["/dev/ttyACM0"])[0]
fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
attrs = termios.tcgetattr(fd)
attrs[0] = attrs[1] = attrs[3] = 0
attrs[2] |= termios.CLOCAL | termios.CREAD
attrs[6][termios.VMIN], attrs[6][termios.VTIME] = 1, 0
termios.tcsetattr(fd, termios.TCSANOW, attrs)

t0 = time.perf_counter()
booted = threading.Event()
bar = [0]
lines = []

def reader():
    buf = b""
    while True:
        try:
            buf += os.read(fd, 4096)
        except OSError:
            return
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode(errors="replace").rstrip()
            if text.startswith("bar "):
                booted.set()
                try:
                    bar[0] = int(text.split()[1])
                except ValueError:
                    pass
            if text.startswith("bar ") or text.strip().startswith("T1"):
                print(f"{time.perf_counter() - t0:7.2f}  {text}", flush=True)
                lines.append(text)

threading.Thread(target=reader, daemon=True).start()

def send(data):
    os.write(fd, data)

def say(text):
    print(f"{time.perf_counter() - t0:7.2f}  >>> {text}", flush=True)

def wait_bars(n):
    target = bar[0] + n
    while bar[0] < target:
        time.sleep(0.01)

def tap(n, hold=0.05):
    send(f"D{n}\n".encode())
    time.sleep(hold)
    send(f"U{n}\n".encode())

if not booted.wait(10):
    sys.exit("no status lines from the pedal")
send(b"K\nN\nT120\nF1\nA0\nI1\n")  # Unity levels, flat filters, 120 BPM, FX off, test tone on
time.sleep(2.5)

say("T1: arm (records from the next bar)")
tap(1)
wait_bars(1)
time.sleep(1.0)
say("T1: tap mid bar 2 of the recording -> 2-bar loop")
wait_bars(1)
time.sleep(1.0)
tap(1)
wait_bars(3)

say("T1: overdub for 2 bars (tone still on: loop gets louder)")
tap(1)
wait_bars(2)
tap(1)
wait_bars(2)

say("T1: hold 1 s -> undo")
tap(1, hold=1.2)
wait_bars(2)
say("T1: hold 1 s again -> redo")
tap(1, hold=1.2)
wait_bars(2)

say("T2: record 1 bar")
tap(2)
wait_bars(1)
time.sleep(0.5)
tap(2)
wait_bars(2)
say("T2: double tap -> stop (stop mode: end of bar, 3-way 3 centre)")
tap(2)
time.sleep(0.15)
tap(2)
wait_bars(2)
say("T2: tap -> plays again from the next bar")
tap(2)
wait_bars(2)

say("both footswitches -> stop all")
send(b"D1\nD2\n")
time.sleep(0.1)
send(b"U1\nU2\n")
wait_bars(2)
say("both again -> start all")
send(b"D1\nD2\n")
time.sleep(0.1)
send(b"U1\nU2\n")
wait_bars(3)

say("hold T1 2 s -> clear; hold T2 2 s -> clear")
tap(1, hold=2.2)
tap(2, hold=2.2)
wait_bars(2)
send(b"I0\nK\n")
say("done (tone off, controls back)")
time.sleep(0.3)
os.close(fd)
