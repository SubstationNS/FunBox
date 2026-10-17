# Ceres
MIDI-synced dual-track bar looper. Design: [docs/looper-design.md](../../docs/looper-design.md)

Status: **phase 1b, internal clock + loop FX test**. No loops yet, so the FX run on the live input
(In L → Out L, In R → Out R).

## Build and flash

Hold BOOT, tap RESET on the Daisy, then:
```
make
make program-dfu
```

## Phase 1b test (no MIDI needed)

| Control | Function |
| --- | --- |
| Knob 2 | Tempo, 60-200 BPM (whole BPM) |
| Knob 5 | FX amount (0 = off) |
| 3-way 2 | FX: Tape / Delay / Repeat |
| LED 2 flash | every beat |
| LED 1 + LED 2 flash (longer) | bar 1, every 4 beats |

Optional: USB from the Daisy to the computer, then `tio /dev/ttyACM0` prints once per bar:
```
bar 3  tempo 120 BPM  fx delay 40%  cpu avg 10% max 11%  | midi ticks 0 start 0 stop 0 other 0 uart_restarts 0
```
MIDI is already received and counted there (plug in a clock to see `midi ticks` climb),
but it doesn't steer the clock yet.

What to check:
- Flashes are steady; turning knob 2 changes the tempo smoothly, without the beat stuttering.
- The tempo doesn't flicker between two values when the knob is left alone.
- Against a metronome at the same BPM, the flashes stay together over a few minutes.

## Flashing without BOOT/RESET

Once Ceres is on the Seed, `./flash.sh` builds, reboots the Seed into DFU over USB serial and flashes.

Dev commands over USB serial (`tio /dev/ttyACM0`): `T<bpm>` pins the tempo, `F<0-2>` the FX
(tape/delay/repeat), `A<0-100>` the FX amount, `S` toggles a stress test (all three FX in series),
`K` hands everything back to the controls, `B` reboots into DFU.

CPU load (both channels, full amount, measured on the Seed): delay 11 %, repeat 3 %, tape 20 %,
all three in series 33 %.

## Host tests

The clock is plain C++ and is tested on the computer:
```
cd test
c++ -std=c++17 -O2 -I.. clock_test.cpp -o clock_test && ./clock_test
```
Checks beats land within one sample of where they should over an hour, tempo changes are
continuous, `Reset()` restarts at bar 1, and the sync rate trim works.

FX demo: renders a plucked riff through each FX (same code as the pedal) to WAV files:
```
c++ -std=c++17 -O2 -I.. fx_render.cpp -o fx_render && ./fx_render /tmp/ceres_fx
```
