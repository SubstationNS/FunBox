# Ceres
MIDI-synced dual-track bar looper. Design: [docs/looper-design.md](../../docs/looper-design.md)

Status: **two tracks, MIDI clock sync, loop FX** (design phases 1–5, minus the MIDI CC map).
Still to come: expression morph + saved state, then time-stretch.

## Build and flash

Hold BOOT, tap RESET on the Daisy, then:
```
make
make program-dfu
```

## Controls

```
  K1 T1 level    K2 Tempo      K3 T2 level
  K4 T1 filter   K5 FX amount  K6 T2 filter
  Tog1 Grid      Tog2 FX type  Tog3 Stop mode
  LED1 / FS1 Track 1           LED2 / FS2 Track 2
```

| Control | Function |
| --- | --- |
| Footswitch 1 / 2 | Track 1 / 2, see below |
| Both footswitches | Stop all (if anything plays) / start all (from the next bar) |
| Knob 1 / 3 | Track 1 / 2 level: silent → unity at about 2 o'clock → +6 dB |
| Knob 4 / 6 | Track 1 / 2 filter: low-pass ← flat at noon → high-pass |
| Knob 2 | Tempo, 60-200 BPM; only without MIDI clock and while both tracks are empty |
| Knob 5 | FX amount on the loops (0 = off) |
| 3-way 1 | Grid for double-tap endings: 1/4 / 1/8 / 1/16 |
| 3-way 2 | FX: Tape / Delay / Repeat |
| 3-way 3 | Stop mode: now / end of bar / end of loop |
| DIP 1 | Split: In L → Track 1 → Out L, In R → Track 2 → Out R. Off: In L feeds both, both outs the same |
| DIP 2 | Loop lengths round up to 1, 2, 4, 8, 16 ... bars |
| MIDI in | Clock sets tempo and phase; Start = bar 1, Stop / Song Position / Continue like a DAW |

Per footswitch:

| Track is | Tap | Double tap | Hold 1 s | Hold 2 s |
| --- | --- | --- | --- | --- |
| empty | arm: records from the next bar | | | |
| armed | cancel | | discard | |
| recording | end at the next bar line (DIP 2: power of two) | end at the next grid step | discard | clear |
| playing | overdub | stop (stop mode) | undo / redo | clear |
| overdubbing | back to playing | take the overdub back, stop | take it back, undo / redo | clear |
| stopped | play from the next bar | | undo / redo | clear |

- A tap up to 80 ms after a bar line (or grid step) ends the loop on it: being a little late is fine.
- Overdubs can be taken back for 1.2 s (double tap / hold), without losing the previous undo.
- Dry always passes at unity; loops (and the FX) are added; a limiter catches hot stacks.
- Loop memory: 75 s per track.

LEDs: recording = bright, overdub = fast blink, playing = dim with a dip on every beat (longer on
bar 1), armed = blinks on the beat, stopping = dim blink, stopped = faint glow, empty = faint beat flash.

MIDI in is a Type A TRS jack. When the MIDI clock goes away, the pedal keeps going at the last tempo
and bar position; knob 2 takes over again once you move it.

Optional: USB from the Daisy to the computer, then `tio /dev/ttyACM0` prints three lines per bar:
```
bar 12  120.02 BPM  midi locked  playing  err +0.04 ms  trim +0.011%  jitter 0.82 ms  fit 192
  T1 playing 4 bars undo -18dB | T2 RECORDING 2+48/96 ->4 -99dB | mono
  fx delay 40%  cpu 20/26%  midi clk 4417 start 1 cont 0 stop 0 spp 0 cc 0 other 0 drop 0 uart 0
```
`err` is how far the clock is from the (de-jittered) MIDI clock, `trim` the speed correction
(±0.2 %, ±2 % while the master's tempo is moving), `jitter` how uneven the incoming ticks are,
`fit` how many ticks the tempo is measured over (up to 192 = 2 bars).

What to check:
- Record, overdub, undo on both tracks; loops wrap without a click; the loop lands on the bar.
- Split mode with two instruments (DIP 1), both footswitches together.
- With a MIDI clock (DAW, drum machine): `midi locked` within a beat, the beat flashes stay with
  the master, Start puts the bar flash on the master's bar 1.
- Stop, move the song position in the DAW, Continue: the bar number follows.
- Unplug the MIDI cable while playing: `free-running`, nothing jumps; plug it back in: locked
  again, same bar.
- Without MIDI: knob 2 tempo, flashes steady.

Footswitches and audio over USB: `test/track_inject.py` puts a test tone on the inputs and taps the
footswitches over USB serial (record, overdub, undo/redo, stop, stop all / start all, clear).

MIDI over USB, without a cable: `test/midi_inject.py` plays a MIDI clock into the pedal over the
USB serial port (lock from a wrong tempo, Start, Stop/SPP/Continue, a tempo step, a pulled cable)
and prints the status lines.

## Flashing without BOOT/RESET

Once Ceres is on the Seed, `./flash.sh` builds, reboots the Seed into DFU over USB serial and flashes.

Dev commands over USB serial (`tio /dev/ttyACM0`): `T<bpm>` pins the tempo, `F<0-2>` the FX
(tape/delay/repeat), `A<0-100>` the FX amount, `S` toggles a stress test (all three FX in series),
`K` hands everything back to the controls, `B` reboots into DFU. MIDI injection: raw bytes
0xF8-0xFF (clock, Start, Continue, Stop) and `M<hex>` (e.g. `Mf20001` = Song Position bar 9) go
into the MIDI input as if they came from the jack. `D1`/`U1` (`D2`/`U2`) press and release a
footswitch, `I1`/`I0` a test tone on the inputs, `N` pins levels to unity and filters flat (the bare
Seed's knobs float).

CPU load (both channels, full amount, measured on the Seed): delay 11 %, repeat 3 %, tape 20 %,
all three in series 33 %. Syncing to MIDI clock adds up to ~8 % on the block a tick lands in.
Worst case, both tracks playing and one overdubbing with all three FX in series: 37 %, up to 60 %
for a moment when an overdub is committed (its undo copy runs in the background).

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

Tracks and footswitch gestures (recording lengths, snapping, power of two, grid endings, seams,
overdub / undo / redo / cancel, stop modes, following a nudged clock, other tempos, MIDI transport):
```
c++ -std=c++17 -O2 -I.. track_test.cpp -o track_test && ./track_test
```

FX demo: renders a plucked riff through each FX (same code as the pedal) to WAV files:
```
c++ -std=c++17 -O2 -I.. fx_render.cpp -o fx_render && ./fx_render /tmp/ceres_fx
```
