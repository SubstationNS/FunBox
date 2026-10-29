#!/usr/bin/env bash
# Flash any FunBox / Daisy firmware .bin, whichever kind it is:
#
#   ./flash_app.sh path/to/app.bin
#
# The .bin's vector table says where it runs:
#   - Internal flash (0x08...): written straight to internal flash over the STM32 ROM DFU.
#     This replaces the Daisy bootloader if one was there.
#   - RAM (0x24... / 0x20..., APP_TYPE = BOOT_SRAM): it lives in QSPI and the Daisy bootloader
#     loads it. The bootloader's own DFU fails on some machines, so this goes the way
#     Ceres/flash.sh --install does: Ceres' loader into internal flash, the app into QSPI over
#     USB serial, then the Daisy bootloader into internal flash.
#
# A running Ceres is sent to ROM DFU by itself; anything else: hold BOOT, tap RESET, release BOOT.
# Run it inside the dev shell (nix develop) so dfu-util and the ARM toolchain are there.
set -uo pipefail

APP=$(realpath "${1:?usage: flash_app.sh app.bin}")
cd "$(dirname "$0")/Ceres"
BOOTLOADER=../../libDaisy/core/dsy_bootloader_v6_2-intdfu-10ms.bin  # 10 ms: no wait at power-up

# The STM32 ROM bootloader calls itself "DFU in FS Mode" (the Daisy bootloader has the same USB ID)
rom_present() {
    for p in /sys/bus/usb/devices/*/product; do [ "$(cat "$p" 2>/dev/null)" = "DFU in FS Mode" ] && return 0; done
    return 1
}
serial_port() {
    for t in /dev/serial/by-id/*Daisy*; do [ -e "$t" ] && echo "$t" && return 0; done
    return 1
}
wait_rom() {
    if ! rom_present; then
        if tty=$(serial_port); then
            printf 'B\n' >"$tty"  # Ceres or its loader: off to ROM DFU
        else
            echo "Waiting for the ROM bootloader: hold BOOT, tap RESET, release BOOT (90 s)"
        fi
    fi
    for _ in $(seq 900); do rom_present && sleep 0.3 && return 0; sleep 0.1; done
    echo "No ROM DFU device showed up." >&2
    exit 1
}
wait_serial() {
    for _ in $(seq 150); do serial_port >/dev/null && sleep 0.5 && return 0; sleep 0.1; done
    echo "The loader didn't show up on USB serial." >&2
    exit 1
}
# dfu-util reports an error after a good download (the chip restarts before it can answer the
# leave request): only the download itself counts
dfu_internal() {
    local out
    out=$(dfu-util -a 0 -s 0x08000000:leave -D "$1" -d ,0483:df11 2>&1) || true
    echo "$out" | grep -q "File downloaded successfully" || { echo "$out" | tail -n 3 >&2; exit 1; }
}

# Second word of the vector table: the reset handler's address
reset=$(od -A n -t x4 -j 4 -N 4 "$APP" | tr -d ' ')
case "$reset" in
    08*)
        echo "$(basename "$APP"): runs from internal flash"
        wait_rom
        dfu_internal "$APP"
        ;;
    24* | 20*)
        echo "$(basename "$APP"): runs from RAM (QSPI + Daisy bootloader)"
        make -s -C loader >/dev/null 2>&1 || { echo "Building the loader failed (in the dev shell?)" >&2; exit 1; }
        echo "== 1/3 loader into internal flash"
        wait_rom
        dfu_internal loader/build/ceres-loader.bin
        wait_serial
        echo "== 2/3 app into QSPI"
        python3 update.py "$(serial_port)" "$APP" || exit 1
        echo "== 3/3 Daisy bootloader into internal flash"
        wait_rom
        dfu_internal "$BOOTLOADER"
        ;;
    *)
        echo "Unknown image (reset handler 0x$reset): not a Daisy app?" >&2
        exit 1
        ;;
esac
echo "Done."
