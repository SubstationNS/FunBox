#!/usr/bin/env bash
# Build and flash Ceres without touching the BOOT/RESET buttons.
# Needs Ceres firmware already running (it reboots into DFU when it receives 'B' over USB serial).
# First flash, or if the firmware hangs: hold BOOT, tap RESET, then run `make program-dfu`.
set -euo pipefail
cd "$(dirname "$0")"

make

if ! lsusb -d 0483:df11 >/dev/null 2>&1; then
    tty=/dev/ttyACM0
    for t in /dev/serial/by-id/*Daisy*; do [ -e "$t" ] && tty=$t && break; done
    echo "Requesting bootloader on $tty"
    printf 'B' > "$tty"
    for _ in $(seq 50); do
        lsusb -d 0483:df11 >/dev/null 2>&1 && break
        sleep 0.1
    done
fi

lsusb -d 0483:df11 >/dev/null 2>&1 || { echo "No DFU device. Hold BOOT, tap RESET, retry." >&2; exit 1; }
# dfu-util reports an error after a successful download: the chip restarts before it can
# acknowledge the leave request. Only fail if the download itself didn't finish.
out=$(make program-dfu 2>&1) || true
echo "$out" | tail -n 4
echo "$out" | grep -q "File downloaded successfully"

# Wait for the new firmware's USB serial port, so a serial reader right after this finds it
for _ in $(seq 150); do
    for t in /dev/serial/by-id/*Daisy*; do [ -e "$t" ] && echo "Running: $t" && exit 0; done
    sleep 0.1
done
echo "Flashed, but no USB serial port showed up (firmware may have crashed early)." >&2
exit 1
