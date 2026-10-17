#!/usr/bin/env python3
# Plays a MIDI clock into Ceres over its USB serial port (no MIDI cable needed) and prints what
# the pedal reports once per bar. The bytes go into the same queue as the MIDI jack's, so the
# parser and sync run exactly as on the jack. The computer's scheduling + USB frames make this a
# realistically jittery clock source, like a DAW through a USB-MIDI interface.
#
#   python3 midi_inject.py [/dev/ttyACM0]
#
# Script: pedal at 100 BPM -> clock at 128 BPM (no Start) -> Start -> Stop / SPP bar 9 / Continue
#         -> tempo step to 140 -> cable "pulled" for 4 s -> back -> done (knob control restored)
import glob, os, sys, termios, threading, time

port = sys.argv[1] if len(sys.argv) > 1 else (glob.glob("/dev/serial/by-id/*Daisy*") or ["/dev/ttyACM0"])[0]
fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
attrs = termios.tcgetattr(fd)
attrs[0] = attrs[1] = attrs[3] = 0          # Raw: no input/output processing, no echo
attrs[2] |= termios.CLOCAL | termios.CREAD
attrs[6][termios.VMIN], attrs[6][termios.VTIME] = 1, 0
termios.tcsetattr(fd, termios.TCSANOW, attrs)

t0 = time.perf_counter()
booted = threading.Event()

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
                booted.set()  # Freshly flashed: wait until it runs before sending anything
            print(f"{time.perf_counter() - t0:7.2f}  {text}", flush=True)

threading.Thread(target=reader, daemon=True).start()

def send(data):
    os.write(fd, data)

def say(text):
    print(f"{time.perf_counter() - t0:7.2f}  >>> {text}", flush=True)

next_tick = time.perf_counter()

def clock(bpm, seconds, connected=True):
    """Send 24 PPQN ticks for `seconds`, on an absolute schedule (no drift)."""
    global next_tick
    period = 60.0 / (bpm * 24)
    end = time.perf_counter() + seconds
    while next_tick < end:
        while True:  # Sleep most of the way, then spin for the last bit
            left = next_tick - time.perf_counter()
            if left <= 0:
                break
            time.sleep(left - 0.002 if left > 0.003 else 0)
        if connected:
            send(b"\xf8")
        next_tick += period

if not booted.wait(10):
    sys.exit("no status lines from the pedal")
send(b"K\nF1\nA0\nT100\n")  # Controls to defaults, FX off, pedal at 100 BPM (so the lock is visible)
time.sleep(0.5)
say("pedal at 100 BPM, no clock")
time.sleep(3)

say("clock at 128 BPM, no Start: tempo + sub-tick phase lock, bar position stays the pedal's")
next_tick = time.perf_counter()
clock(128, 8)

say("Start: bar 1 on the next tick")
send(b"\xfa")
clock(128, 8)

say("Stop (clock keeps running), SPP bar 9, Continue: the next bar printed should be bar 9")
send(b"\xfc")
clock(128, 3)
send(b"Mf20001\n")  # SPP 128 sixteenths = bar 9 (0x00 0x01: lsb, msb)
clock(128, 0.1)
send(b"\xfb")
clock(128, 6)

say("tempo step to 140 BPM")
clock(140, 8)

say("cable pulled for 4 s: keeps 140, free-running")
clock(140, 4, connected=False)
say("cable back")
clock(140, 6)

say("clock stops for good: stays at 140 until the knob moves")
time.sleep(4)
send(b"K\n")
say("done (K: controls handed back)")
time.sleep(0.5)
os.close(fd)
