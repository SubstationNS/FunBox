# Ceres
MIDI-synced dual-track bar looper. Design: [docs/looper-design.md](../../docs/looper-design.md)

Status: **phase 1a, internal clock**. Audio passes straight through; no looping yet.

## Build and flash

Hold BOOT, tap RESET on the Daisy, then:
```
make
make program-dfu
```

## Phase 1a test (no MIDI needed)

| Control | Function |
| --- | --- |
| Knob 2 | Tempo, 60-200 BPM (whole BPM) |
| LED 2 flash | every beat |
| LED 1 + LED 2 flash (longer) | bar 1, every 4 beats |

Optional: USB from the Daisy to the computer, then `tio /dev/ttyACM0` prints once per bar:
```
bar 3  tempo 120 BPM  | midi ticks 0 start 0 stop 0 other 0 uart_restarts 0
```
MIDI is already received and counted there (plug in a clock to see `midi ticks` climb),
but it doesn't steer the clock yet.

What to check:
- Flashes are steady; turning knob 2 changes the tempo smoothly, without the beat stuttering.
- The tempo doesn't flicker between two values when the knob is left alone.
- Against a metronome at the same BPM, the flashes stay together over a few minutes.

## Host tests

The clock is plain C++ and is tested on the computer:
```
cd test
c++ -std=c++17 -O2 -I.. clock_test.cpp -o clock_test && ./clock_test
```
Checks beats land within one sample of where they should over an hour, tempo changes are
continuous, `Reset()` restarts at bar 1, and the sync rate trim works.
