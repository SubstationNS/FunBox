#!/usr/bin/env python3
# Tests pickup, the A/B morph, MIDI CC and saved settings on the pedal over USB serial: knob, pedal
# and DIP 4 readings are injected (X / Y commands), MIDI CC through the M command, and the pedal is
# restarted (R) to check what comes back.
#
#   python3 params_inject.py [/dev/ttyACM0]
import glob, os, sys, termios, threading, time

def find_port():
    return (glob.glob("/dev/serial/by-id/*Daisy*") or [None])[0]

class Pedal:
    def __init__(self):
        self.lines, self.fd = [], None
        self.connect()

    def connect(self):
        for _ in range(100):
            port = find_port()
            if port:
                break
            time.sleep(0.1)
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
        a = termios.tcgetattr(self.fd)
        a[0] = a[1] = a[3] = 0
        a[6][termios.VMIN], a[6][termios.VTIME] = 0, 1
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        threading.Thread(target=self.reader, args=(self.fd,), daemon=True).start()

    def reader(self, fd):
        buf = b""
        while True:
            try:
                buf += os.read(fd, 4096)
            except OSError:
                return
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.lines.append(line.decode(errors="replace").rstrip())

    def send(self, text):
        os.write(self.fd, text.encode())

    def wait_for(self, needle, timeout=15):
        end = time.time() + timeout
        while time.time() < end:
            for i, l in enumerate(self.lines):
                if needle in l:
                    del self.lines[: i + 1]
                    return l
            time.sleep(0.05)
        return None

    def params(self):
        self.lines.clear()
        return self.wait_for("morph", 5)

    def turn(self, k, frm, to, step=2):
        """Move pinned control k from frm % to to % like a hand does."""
        rng = range(frm, to - 1, -step) if to < frm else range(frm, to + 1, step)
        for v in rng:
            self.send(f"X{k}{v:03d}\n")
            time.sleep(0.02)

def show(label, line):
    print(f"{label:52s} {line.strip() if line else '(nothing)'}", flush=True)

p = Pedal()
p.wait_for("bar ")
p.send("K\nN\nT120\nF1\nZ")  # Defaults; pinned controls: levels unity, filters flat, FX 0, pedal 0
time.sleep(0.6)
show("start (pinned knobs)", p.params())

p.turn(0, 71, 30)
show("knob 1 down to 30 %", p.params())
print("waiting for the save (10 s after the last change) ...", flush=True)
show("", p.wait_for("saved settings", 20))

print("restart; knobs float now (not pinned): restored values must stay", flush=True)
p.send("R\n")
os.close(p.fd)
time.sleep(1.5)
p = Pedal()
show("after restart", p.wait_for("restored settings", 15))
show("parameters (owner '-' = restored)", p.params())
time.sleep(4)
show("4 s later, floating knobs (noise) didn't take over", p.params())

p.send("N\nT120\n")  # Pin again: knob 1 reads 71 %, restored 30 %
time.sleep(0.3)
show("pinned at 71 %: still the restored 30 %", p.params())
p.turn(0, 71, 50)
show("knob 1 to 50 %: not crossed 30 yet", p.params())
p.turn(0, 50, 20)
show("knob 1 to 20 %: crossed 30, took over", p.params())

print("morph: DIP 4 on (A), T2 level 71 -> 0 and pedal 0 -> 90, DIP 4 off (B)", flush=True)
p.send("Y1\n")
time.sleep(0.3)
show("learning", p.params())
p.turn(1, 71, 0)
p.turn(5, 0, 90)
p.send("Y0\n")
time.sleep(0.3)
show("morph built (mask 02 = T2 level)", p.params())
p.turn(5, 90, 45)
show("pedal half way back: T2 level ~36", p.params())
p.turn(5, 45, 0)
show("pedal at heel: T2 level back to 71", p.params())

print("MIDI CC 74 = 100 (T1 filter), CC 77 = 0 (delay 1/4), CC 20 (T1 tap)", flush=True)
p.send("Mb04a64\n")
p.send("Mb04d00\n")
p.send("Mb0147f\nMb01400\n")
time.sleep(0.3)
show("CC applied", p.params())
p.lines.clear()
show("T1 armed by CC 20", p.wait_for("T1 ", 5))

print("waiting for the save, then restart: morph and CC values must come back", flush=True)
show("", p.wait_for("saved settings", 25))
p.send("R\n")
os.close(p.fd)
time.sleep(1.5)
p = Pedal()
show("after restart", p.wait_for("restored settings", 15))
show("parameters", p.params())
p.send("K\n")
