#!/usr/bin/env python3
# Sends a firmware image to a running Ceres (or the loader) over USB serial (the W command), which writes it to
# QSPI and restarts into it. Used by flash.sh.
#
#   python3 update.py <serial port> <image.bin>
#
# Exit code 0 when Ceres reports the image written and verified.
import os, sys, termios, time, zlib

port, path = sys.argv[1], sys.argv[2]
image = open(path, "rb").read()

fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
attrs = termios.tcgetattr(fd)
attrs[0] = attrs[1] = attrs[3] = 0          # Raw: binary-safe, no echo
attrs[2] |= termios.CLOCAL | termios.CREAD
attrs[6][termios.VMIN], attrs[6][termios.VTIME] = 0, 1
termios.tcsetattr(fd, termios.TCSANOW, attrs)
termios.tcflush(fd, termios.TCIFLUSH)

# Wait for it to be up (any line: Ceres' status, the loader's "ready"); commands sent right
# after boot get lost
buf, end = b"", time.time() + 10
while time.time() < end and b"\n" not in buf:
    try:
        buf += os.read(fd, 4096)
    except OSError:
        time.sleep(0.1)

os.write(fd, b"W%x,%08x\n" % (len(image), zlib.crc32(image)))
for i in range(0, len(image), 4096):
    os.write(fd, image[i:i + 4096])
print(f"sent {len(image)} bytes, crc {zlib.crc32(image):08x}", flush=True)

buf, end = b"", time.time() + 30
while time.time() < end:
    try:
        buf += os.read(fd, 4096)
    except OSError:
        break  # Port gone: it restarted
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        text = line.decode(errors="replace").strip()
        if text.startswith("update:"):
            print(text, flush=True)
            if "verified" in text:
                sys.exit(0)
            if "FAILED" in text or "mismatch" in text:
                sys.exit(1)
sys.exit("no answer from Ceres")
