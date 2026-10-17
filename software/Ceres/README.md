# Ceres
MIDI-synced dual-track bar looper. Design: [docs/looper-design.md](../../docs/looper-design.md)

Status: **phase 1c, internal clock synced to MIDI clock + loop FX test**. No loops yet, so the FX
run on the live input (In L → Out L, In R → Out R).

## Build and flash

Hold BOOT, tap RESET on the Daisy, then:
```
make
make program-dfu
```

## Phase 1c test

| Control | Function |
| --- | --- |
| MIDI in | MIDI clock sets tempo and phase; Start = bar 1, Stop / Song Position / Continue like a DAW |
| Knob 2 | Tempo, 60-200 BPM (whole BPM), only without MIDI clock |
| Knob 5 | FX amount (0 = off) |
| 3-way 2 | FX: Tape / Delay / Repeat |
| LED 2 flash | every beat |
| LED 1 + LED 2 flash (longer) | bar 1, every 4 beats |

MIDI in is a Type A TRS jack. When the MIDI clock goes away, the pedal keeps going at the last tempo
and bar position; knob 2 takes over again once you move it.

Optional: USB from the Daisy to the computer, then `tio /dev/ttyACM0` prints two lines per bar:
```
bar 12  120.02 BPM  midi locked  playing  err +0.04 ms  trim +0.011%  jitter 0.82 ms  fit 192
  fx delay 40%  cpu 10/11%  midi clk 4417 start 1 cont 0 stop 0 spp 0 cc 0 other 0 drop 0 uart 0
```
`err` is how far the clock is from the (de-jittered) MIDI clock, `trim` the speed correction
(±0.2 %, ±2 % while the master's tempo is moving), `jitter` how uneven the incoming ticks are,
`fit` how many ticks the tempo is measured over (up to 192 = 2 bars).

What to check:
- With a MIDI clock (DAW, drum machine): `midi locked` within a beat, the beat flashes stay with
  the master, Start puts the bar flash on the master's bar 1.
- Stop, move the song position in the DAW, Continue: the bar number follows.
- Unplug the MIDI cable while playing: `free-running`, nothing jumps; plug it back in: locked
  again, same bar.
- Without MIDI: knob 2 tempo, flashes steady.

MIDI over USB, without a cable: `test/midi_inject.py` plays a MIDI clock into the pedal over the
USB serial port (lock from a wrong tempo, Start, Stop/SPP/Continue, a tempo step, a pulled cable)
and prints the status lines.

## Flashing without BOOT/RESET

Once Ceres is on the Seed, `./flash.sh` builds, reboots the Seed into DFU over USB serial and flashes.

Dev commands over USB serial (`tio /dev/ttyACM0`): `T<bpm>` pins the tempo, `F<0-2>` the FX
(tape/delay/repeat), `A<0-100>` the FX amount, `S` toggles a stress test (all three FX in series),
`K` hands everything back to the controls, `B` reboots into DFU. MIDI injection: raw bytes
0xF8-0xFF (clock, Start, Continue, Stop) and `M<hex>` (e.g. `Mf20001` = Song Position bar 9) go
into the MIDI input as if they came from the jack.

CPU load (both channels, full amount, measured on the Seed): delay 11 %, repeat 3 %, tape 20 %,
all three in series 33 %. Syncing to MIDI clock adds up to ~8 % on the block a tick lands in.

## Host tests

The clock is plain C++ and is tested on the computer:
```
cd test
c++ -std=c++17 -O2 -I.. clock_test.cpp -o clock_test && ./clock_test
```
Checks beats land within one sample of where they should over an hour, tempo changes are
continuous, `Reset()` restarts at bar 1, and the sync rate trim works.

MIDI parser and clock sync, against a simulated master with worst-case USB-MIDI jitter (steady
tempos for up to an hour, tempo steps and ramps, a pulled cable, Stop/SPP/Continue, cold lock-on):
```
c++ -std=c++17 -O2 -I.. midi_test.cpp -o midi_test && ./midi_test
```
Error stays within ±0.6 ms of the average arrival delay (which no receiver can know: set the
DAW's MIDI clock offset for that).

FX demo: renders a plucked riff through each FX (same code as the pedal) to WAV files:
```
c++ -std=c++17 -O2 -I.. fx_render.cpp -o fx_render && ./fx_render /tmp/ceres_fx
```
