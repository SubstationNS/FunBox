#!/usr/bin/env python3
# Dumps Ceres' black-box log (boots, calibration, track and MIDI events, a summary every minute)
# over USB serial. Plug the Seed in after a session and run:
#
#   python3 log.py [/dev/ttyACM0] [--save file.txt]
import glob, os, sys, termios, time

args = [a for a in sys.argv[1:] if not a.startswith("--")]
save = sys.argv[sys.argv.index("--save") + 1] if "--save" in sys.argv else None
if save in args:
    args.remove(save)
port = args[0] if args else (glob.glob("/dev/serial/by-id/*Daisy*") or ["/dev/ttyACM0"])[0]

fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
a = termios.tcgetattr(fd)
a[0] = a[1] = a[3] = 0
a[6][termios.VMIN], a[6][termios.VTIME] = 0, 1
termios.tcsetattr(fd, termios.TCSANOW, a)

# Wait until it's up (any line), then ask for the log
buf, end = b"", time.time() + 10
while time.time() < end and b"\n" not in buf:
    buf += os.read(fd, 4096)
os.write(fd, b"L")

lines, buf, started, end = [], b"", False, time.time() + 30
while time.time() < end:
    buf += os.read(fd, 4096)
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        text = line.decode(errors="replace").rstrip("\r")
        if text == "log begin":
            started = True
        elif text == "log end":
            end = 0
        elif started and text.startswith("#"):
            lines.append(text)
            print(text, flush=True)
os.close(fd)
if save:
    open(save, "w").write("\n".join(lines) + "\n")
    print(f"saved {len(lines)} entries to {save}")
