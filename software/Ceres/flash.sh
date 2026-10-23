#!/usr/bin/env bash
# Build and flash Ceres without touching the BOOT/RESET buttons.
#
#   ./flash.sh            build, send the firmware to the running Ceres over USB serial; it writes
#                         itself to QSPI and restarts (no DFU involved)
#   ./flash.sh --install  first time, or to recover: Ceres into QSPI + the Daisy bootloader
#
# Ceres runs from RAM and lives in the Seed's QSPI flash (APP_TYPE = BOOT_SRAM); the Daisy
# bootloader in internal flash loads it at power-up. The bootloader's own DFU mode isn't used:
# erasing QSPI through it fails on this machine.
#
# --install goes through the STM32's ROM bootloader (always there, in internal flash's place):
#   1. the small loader (loader/) into internal flash, via ROM DFU
#   2. it writes Ceres into QSPI (same update command as above)
#   3. ROM DFU again: the Daisy bootloader replaces it, and loads Ceres from QSPI from now on
# It reaches ROM DFU by itself if some Ceres is running; otherwise hold BOOT, tap RESET, release.
set -euo pipefail
cd "$(dirname "$0")"

BOOTLOADER=../../libDaisy/core/dsy_bootloader_v6_2-intdfu-10ms.bin  # 10 ms: no wait at power-up

serial_port() {
    for t in /dev/serial/by-id/*Daisy*; do [ -e "$t" ] && echo "$t" && return 0; done
    return 1
}

wait_running() {
    for _ in $(seq 150); do
        if t=$(serial_port); then echo "Running: $t"; return 0; fi
        sleep 0.1
    done
    echo "No USB serial port showed up (firmware may have crashed early)." >&2
    exit 1
}

wait_gone() {
    for _ in $(seq 50); do serial_port >/dev/null || return 0; sleep 0.1; done
}

# The STM32 ROM bootloader calls itself "DFU in FS Mode" (the Daisy bootloader has the same
# USB ID, so check the name)
rom_dfu_present() {
    local p
    for p in /sys/bus/usb/devices/*/product; do
        [ "$(cat "$p" 2>/dev/null)" = "DFU in FS Mode" ] && return 0
    done
    return 1
}

# ROM DFU: from a running Ceres ('B'), or by hand (hold BOOT, tap RESET, release BOOT)
rom_dfu() {
    if ! rom_dfu_present; then
        if tty=$(serial_port); then
            printf 'B\n' > "$tty"
        else
            echo "Waiting for the ROM bootloader: hold BOOT, tap RESET, release BOOT (90 s)"
        fi
    fi
    for _ in $(seq 900); do rom_dfu_present && sleep 0.3 && return 0; sleep 0.1; done
    echo "No ROM DFU device showed up." >&2
    exit 1
}

# dfu-util reports an error after a successful download: the chip restarts before it can
# acknowledge the leave request. Only fail if the download itself didn't finish.
dfu_internal() {
    local out
    out=$(dfu-util -a 0 -s 0x08000000:leave -D "$1" -d ,0483:df11 2>&1) || true
    echo "$out" | tail -n 2
    echo "$out" | grep -q "File downloaded successfully"
}

update() {
    local tty
    tty=$(serial_port) || { echo "No Ceres running. First time? ./flash.sh --install" >&2; exit 1; }
    python3 update.py "$tty" build/ceres.bin
}

# Builds quietly (newlib's linker warnings are noise); shows the log if it fails
quiet_make() {
    local log
    log=$(make "$@" 2>&1) || { echo "$log" >&2; exit 1; }
    echo "$log" | grep -E "^ +(SRAM|FLASH):" || true
}

quiet_make

if [ "${1:-}" = "--install" ]; then
    quiet_make -C loader
    echo "== 1/3 loader into internal flash (ROM DFU)"
    rom_dfu
    dfu_internal loader/build/ceres-loader.bin
    wait_running
    echo "== 2/3 Ceres into QSPI"
    update
    echo "== 3/3 Daisy bootloader into internal flash (ROM DFU)"
    rom_dfu
    dfu_internal "$BOOTLOADER"
    wait_running
    exit 0
fi

update
wait_gone
wait_running
