# Ceres — User Manual

**Ceres** is a two-track looper for the FunBox v3 that thinks in bars. Loops start and end on the
beat, lock to MIDI clock, follow tempo changes without changing pitch, and come back exactly as
you left them after a power cycle.

---

## 1. At a glance

- **Two tracks**, one footswitch each, up to **75 seconds per track**.
- **Bar-quantized recording**: loops always start on a bar line and end on a bar line (or a grid
  step you choose), so they never drift against each other or the band.
- **MIDI clock sync** with a jitter-proof "flywheel": the pedal keeps perfect time even when the
  incoming clock is sloppy or disappears.
- **Time stretch**: change the tempo (knob or MIDI) and loops follow — pitch stays the same.
- **Undo / redo** for every overdub, and a 1.2-second "oops" window to take back an overdub you
  just started.
- **DJ filter and level per track**, plus **loop effects** (tape, synced delay, beat repeat).
- **Expression pedal morph**: record two sounds, sweep between them with your foot.
- **Dual mono**: one instrument through both tracks, or two instruments on separate tracks.
- **Remembers everything** (levels, filters, tempo, morph) and never jumps when you touch a knob.

---

## 2. The panel

```
        [ T1 LEVEL ]   [  TEMPO  ]   [ T2 LEVEL ]
        [ T1 FILTER]   [   FX    ]   [ T2 FILTER]

         GRID            FX TYPE        STOP
       1/4 1/8 1/16   TAPE DLY RPT   NOW BAR LOOP

           (LED 1)                      (LED 2)
        [ TRACK 1 ]                  [ TRACK 2 ]
```

The layout is **mirrored**: everything for Track 1 is on the left, Track 2 on the right, shared
controls in the middle.

| Control | What it does |
| --- | --- |
| **T1 Level / T2 Level** (knobs 1, 3) | Loop volume. Silent at zero, **unity at about 2 o'clock**, +6 dB at full. |
| **Tempo** (knob 2) | 60–200 BPM, in whole BPM. Ignored while a MIDI clock is running. Loops follow it (time-stretched). Held while a recording is in progress. |
| **T1 Filter / T2 Filter** (knobs 4, 6) | DJ-style filter on the loop: low-pass to the left of noon, flat at noon, high-pass to the right. Only affects playback — the recording stays untouched. |
| **FX** (knob 5) | Amount of the loop effect chosen with the FX TYPE toggle. Fully left = off. |
| **GRID** toggle | Step size for *double-tap* endings: 1/4, 1/8 or 1/16 note. |
| **FX TYPE** toggle | Tape / Delay / Repeat (see section 8). |
| **STOP** toggle | What a stop does: stop **now**, at the end of the **bar**, or at the end of the **loop**. |
| **Track 1 / Track 2** footswitches | Record, overdub, play, stop, undo, clear (see section 3). |

**Side jacks and switches** (FunBox v3):

| Where | What |
| --- | --- |
| Left side, top | **Expression** pedal input (1/8" TRS) |
| Right side, top | **MIDI** input (1/8" TRS, **Type A** — use a Type A TRS-to-DIN adapter) |
| Right side | **USB** (Daisy Seed) — firmware updates and the session log |
| Right side, lower | **DIP switches** |
| Top | Audio **In L / In R**, **Out L / Out R**, 9 V DC center negative |

**DIP switches**

| DIP | Off | On |
| --- | --- | --- |
| 1 | **Mono**: In L feeds both tracks, both outputs carry the same mix | **Split**: In L → Track 1 → Out L, In R → Track 2 → Out R |
| 2 | Loops end on the next bar line | **Power of two**: loops round up to 1, 2, 4, 8, 16 ... bars |
| 3 | (unused) | (unused) |
| 4 | Normal | **Expression learn** (see section 9) |

Your dry signal always passes through at unity — Ceres works in series like any looper; the loops
are added on top. A limiter on the outputs catches hot overdub stacks.

---

## 3. Footswitches

Each footswitch controls its own track. What a press does depends on what the track is doing:

| Track is… | **Tap** | **Double tap** | **Hold 1 s** | **Hold 2 s** |
| --- | --- | --- | --- | --- |
| Empty | Arm: recording starts on the next bar line | | | |
| Armed | Cancel | | Discard | |
| Recording | End on the next bar line | End on the next **grid** step | Discard | Clear |
| Playing | Overdub | Stop (per STOP toggle) | Undo / redo | Clear |
| Overdubbing | Back to playing | Take this overdub back and stop | Take it back, undo / redo | Clear |
| Stopped | Play again from the next bar line | | Undo / redo | Clear |

**Both footswitches together**: stop all tracks (if anything is playing), or start all stopped
tracks together on the next bar line.

A tap acts the moment the switch goes down, so punching in is instant. If the press turns out to
be part of a double tap, a hold or a both-switch press, Ceres quietly takes its first action back.

---

## 4. Recording a loop

1. **Tap** — the track is armed (its LED blinks on the beat).
2. Recording starts **on the next bar line**. Play.
3. End the loop:
   - **Tap**: it ends on the next bar line. With **DIP 2 on** it instead runs to the next
     power-of-two length (stop during bar 5 → 8 bars, during bar 14 → 16 bars).
   - **Double tap**: it ends on the next **grid** step (GRID toggle: 1/4, 1/8, 1/16). This is how
     you make odd lengths — 7 beats, 15 eighths… Two tracks with different lengths drift against
     each other and meet again later (polymeter).
4. Playback starts right away, seamlessly.

**A little late is fine**: a tap up to 80 ms after a bar line ends the loop *on* that bar line.

**Countdown**: once the end is set, the LED counts down — at each bar line one short dip per bar
still to go, then a dip on every beat in the last bar (4-3-2-1).

**Preset length** (MIDI CC 29): recordings end by themselves after a set number of bars.

Loop seams are crossfaded automatically — loops wrap without a click.

---

## 5. Overdub, undo, redo

- **Tap** while playing to overdub; tap again to stop overdubbing. The existing loop stays at full
  level, your new playing is added.
- **Hold 1 second** to undo the last overdub. Hold again to redo it.
- **Changed your mind right away?** For the first 1.2 seconds an overdub can be taken back
  completely with a double tap or a hold — and your *previous* undo is still there.

---

## 6. Stopping and starting

- **Double tap** stops a track according to the **STOP** toggle: *now* (a quick 5 ms fade), at
  the end of the *bar*, or at the end of the *loop*. While a stop is pending the LED blinks dim;
  tap to cancel it.
- **Tap** a stopped track to play it again from the top on the next bar line.
- **Both footswitches** stop everything, or start everything together.

---

## 7. Two tracks: mono or split

- **Mono (DIP 1 off)**: one instrument. In L feeds both tracks; both outputs carry the same mix.
  Great for layering two loops of different lengths.
- **Split (DIP 1 on)**: two instruments or a stereo source. In L → Track 1 → Out L, In R →
  Track 2 → Out R. Use two Y cables or a stereo setup; each side has its own loop effect.

---

## 8. Loop effects (FX knob + FX TYPE)

The effects only touch the loops — your live signal stays clean. Knob fully left = off.

| FX TYPE | What the FX knob does |
| --- | --- |
| **Tape** | Wow and flutter, saturation, treble loss: clean → worn cassette → seasick. |
| **Delay** | Tempo-synced delay (dotted 1/8 by default; 1/4, 1/8, 1/8 triplet or 1/16 via MIDI CC 77). Turning up raises mix and feedback together: slapback → long, darkening, singing tails. |
| **Repeat** | Tempo-synced beat repeat. The slice gets shorter as you turn: 1/4 → 1/8 → 1/16 → 1/32. On an expression pedal it makes a classic build-up roll. |

---

## 9. Expression pedal: morph between two sounds

Plug an expression pedal into the left side jack. You teach Ceres two "states", A and B, and the
pedal sweeps between them. It can move several things at once: both levels, both filters and the
FX amount (never the tempo).

**Learning a morph**

1. Set the knobs to the starting sound and put the pedal where that sound should be (e.g. heel).
2. **DIP 4 on** — state A is captured. Both LEDs flash, then breathe while you're learning.
3. Turn the knobs to the target sound and move the pedal to where it should be (e.g. toe).
4. **DIP 4 off** — state B is captured. Both LEDs flash.

Everything that differs between A and B now follows the pedal, between the two positions you
used (it doesn't have to be the full travel). Flip DIP 4 on and off without changing anything to
clear the morph.

**Ideas**

| Morph | Heel (A) | Toe (B) |
| --- | --- | --- |
| Track crossfader | T1 up, T2 down | T1 down, T2 up |
| Dual wah | both filters dark | both filters bright |
| Drop / build | levels as set | filters high-passed, levels down |
| Roll | FX off (Repeat) | FX full |

**Knobs and the pedal share** a parameter politely: whichever crosses the current value last owns
it. Nothing ever jumps.

---

## 10. Tempo, MIDI clock and time stretch

**Without MIDI**, the TEMPO knob sets the tempo. The LEDs flash on the beat (longer on bar 1).

**With MIDI clock** (from a DAW, drum machine, etc.), Ceres follows the clock's tempo and phase.
It smooths out jittery clocks (USB-MIDI from a computer is fine) and never jumps:

| MIDI | Ceres |
| --- | --- |
| Clock | Follows tempo and phase, locked within about a millisecond. |
| **Start** | Back to bar 1. Loops that were stopped by MIDI Stop restart from the top. |
| **Stop** | Playing loops fade out and remember where they were. |
| **Continue** | Loops resume where they were — or at the new song position if you moved it. |
| Clock disappears | Nothing audible: Ceres keeps going at the last tempo. **Both LEDs breathe slowly** until the clock returns (or you move the TEMPO knob). |

Tracks you stopped yourself stay stopped; only tracks stopped by MIDI Stop resume. A track armed
while the MIDI transport is stopped waits for it to start.

**Time stretch**: when the tempo changes (knob or MIDI), loops follow without changing pitch.
Close to the tempo a loop was recorded at, it plays back untouched.

**Overdubbing at a different tempo**: just tap. The overdub starts instantly; after a moment the
loop is re-rendered at the current tempo (a few bars take a fraction of a second, a full 75 s loop
about 3.5 s) and your overdub goes into it. **Undo brings back the original recording** at its
own tempo.

---

## 11. It remembers — and never jumps

- Levels, filters, FX amount, tempo, MIDI CC settings and the expression morph are **saved
  automatically** 10 seconds after your last change, and restored at power-up. (Loops themselves
  are not saved.)
- After power-up, **a knob does nothing until you turn it**, and it only takes over once it passes
  the saved value (the LED flashes when it does). So bumping a knob — or a curious kid — won't
  change your sound.
- The toggles and DIP switches always apply as they are set.

---

## 12. LEDs

| LED shows | Meaning |
| --- | --- |
| Off with a faint flash on the beat | Track empty |
| Blinking on the beat | Armed (waiting for the bar line) |
| Bright, dipping on the beat | Recording |
| Bright with dips at the bar line / on every beat | Recording, counting down to its end |
| Fast blinking | Overdubbing |
| Dim, dipping on the beat (longer on bar 1) | Playing |
| Dim blinking | Stop pending |
| Very faint glow | Stopped |
| Short flash | A knob or the pedal just took over a setting |
| Both flash, then breathe | Expression learn (DIP 4) |
| Both breathe slowly | MIDI clock lost, running on its own |

---

## 13. MIDI implementation

MIDI in only (1/8" TRS **Type A**). All channels (omni).

**Realtime**: Clock, Start, Stop, Continue, Song Position Pointer.

**Control change**

| CC | Function | CC | Function |
| --- | --- | --- | --- |
| 20 / 21 | Track 1 / 2 tap | 7 / 8 | Track 1 / 2 level |
| 22 / 23 | Track 1 / 2 stop | 74 / 75 | Track 1 / 2 filter |
| 24 / 25 | Track 1 / 2 undo / redo | 76 | FX amount |
| 26 / 27 | Track 1 / 2 clear | 77 | Delay time: 1/4, dotted 1/8, 1/8, 1/8 triplet, 1/16 |
| 28 | Stop all / start all | 29 | Preset loop length in bars (0 = off) |
| 30 | Grid 1/4, 1/8, 1/16 (until the GRID toggle moves) | | |

Buttons (CC 20–28) act when the value goes from below 64 to 64 or above. Value CCs jump straight to
their value (no pickup).

---

## 14. Latency calibration (once)

Ceres compensates for its own audio delay so overdubs land exactly where you played them. To
measure it:

1. Connect **Out L to In L** with an instrument cable.
2. **Hold footswitch 1 while plugging in the power**, release after a second.
3. Both LEDs light while it measures (about 4 seconds).
4. **Three slow blinks** = done and saved. **Fast blinking** = no signal (check the cable).
5. Unplug the power, remove the cable, power up again. Done for good.

(A typical result is about 2.4 ms.)

---

## 15. For tinkerers: USB, updates, session log

Connect the Daisy Seed's USB port to a computer.

- **Session log**: Ceres keeps a black-box log in its memory — every power-up, recordings,
  overdubs, undos, MIDI events, calibration results, and a summary every minute. Read it with
  `python3 test/log.py` from the Ceres folder.
- **Firmware updates**: `./flash.sh` — no buttons needed.
- A live status line is printed once per bar over USB serial.

---

## 16. Specifications

| | |
| --- | --- |
| Tracks | 2, mono each (dual mono) |
| Loop length | up to 75 s per track, plus a full undo level |
| Audio | 48 kHz, 32-bit float processing |
| Round-trip latency | about 2.4 ms, compensated |
| Tempo | 60–200 BPM (knob), any tempo via MIDI clock |
| Time stretch | pitch-neutral (WSOLA), follows tempo changes live |
| MIDI | In (TRS Type A): clock, transport, song position, CC |
| Expression | 1/8" TRS, A/B morph of up to 5 parameters |
| Hardware | FunBox v3 (Daisy Seed: ARM Cortex-M7 480 MHz, 64 MB SDRAM) |
| Power | 9 V DC, center negative |

---

## 17. Troubleshooting

| Problem | Try |
| --- | --- |
| A knob doesn't do anything | It's waiting to pick up its saved value: turn it past where the setting currently is (the LED flashes when it takes over). |
| MIDI clock isn't followed | Ceres needs a **Type A** TRS adapter. Check your clock source sends MIDI clock. |
| Loops don't start when I arm them | The MIDI transport is stopped — armed tracks wait for Start / Continue. |
| Overdubs sound slightly late or early | Run the latency calibration (section 14). |
| Tempo knob doesn't change the tempo | A MIDI clock is running, or a recording is in progress. |
