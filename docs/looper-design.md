# Ceres — MIDI-synced bar looper (design draft)

> Working name. Status: design / discussion, nothing implemented yet.
> Target hardware: FunBox v3.2 (MIDI in only).

Pluto is a creative looper (varispeed, effects, Frippertronics). Ceres is the opposite:
a **tight, RC-600-style performance looper** whose loops are always whole bars, locked to
an external MIDI clock or its own tempo knob.

## Goals

- **MIDI clock follower** (24 PPQN `0xF8`, `Start`, `Stop`, `Continue`, `Song Position Pointer`), with a **tempo knob** when there's no MIDI.
- **Grid-accurate loops on a fixed 4/4 grid**: recording always starts on a bar line; it ends on a bar
  line or, by footswitch gesture, on a beat/subdivision. Each track has its own length, so polymeters
  come from loop lengths (e.g. 16 beats against 7), not from a time-signature setting.
- **Never glitches because of the clock**: an internal clock always runs; MIDI only steers it.
- **Dual mono**: two independent channels (In L → Track 1 → Out L, In R → Track 2 → Out R),
  so two Y cables make it a two-instrument looper. Or one source feeding both tracks.
- **Pitch-neutral tempo changes** via time-stretch (later phase; tempo is locked until then).
- **Fully controllable over MIDI CC** for an external MIDI footswitch.

## Non-goals

- Varispeed / pitch-follow on tempo change.
- Stereo modes.
- Saving loops across power cycles (QSPI is only 8 MB ≈ 87 s of 16-bit mono).
- Clock master — v3.2 has no MIDI out.
- Auto-record on MIDI Start, click track output, dry level / kill-dry (serial use only).
- Knob automation (maybe some day).

## Hardware constraints

| Resource | Available | Implication |
| --- | --- | --- |
| SDRAM | 64 MB | ~290 s of mono float @ 48 kHz total |
| Footswitches | 2 | one per track; everything else via knobs/toggles/MIDI CC |
| LEDs | 2 (PWM-able) | track state + beat flash via brightness |
| Knobs | 6 pots (no detents) | continuous parameters only — no "pick from a list" on a pot |
| Audio in/out | 1 × TRS jack each (L/R) | dual mono via TRS → 2 × TS insert/Y cables; a TS plug grounds R |
| MIDI in | 1/8" TRS, **Type A** (tip = DIN pin 5, ring = DIN pin 4; checked on the v3 PCB, J6) | use a Type A TRS ↔ DIN adapter |
| Flash | 128 KB | fine; BOOT_SRAM if the stretcher grows it |

Memory budget (mono, float, 48 kHz): 2 tracks × (loop + undo buffer) → **~70 s per track**
(≈ 35 bars of 4/4 at 120 BPM). int16 storage doubles that if needed.

## Clock design: internal flywheel, MIDI or tempo knob steers it

The loops **never** follow MIDI directly. They follow an internal clock that always runs.
Incoming MIDI clock only *disciplines* it (a smoothed PLL). Without MIDI, the tempo knob sets it.

```
MIDI 0xF8 ticks ──► tempo/phase estimator ──► (bounded correction) ──┐
                                                                      ├──► internal clock ──► loops
Tempo knob (used only when no MIDI clock) ────────────────────────────┘    (always running)
```

### Why not "resync on error"
A threshold-based hard resync turns every clock hiccup (loose cable, USB-MIDI jitter burst,
clock box rebooting) into an audible jump — and it can't even resync *correctly*: MIDI
clock ticks carry no bar or beat information. Bar position only exists as "ticks counted
since Start". Once ticks are lost, the pedal can't know where the bar is until the next
`Start` / `Song Position Pointer`. So the only honest behaviour is to keep going.

### Behaviour
| Situation | What happens |
| --- | --- |
| Clock running | internal tempo tracks the smoothed tick period; phase is corrected by tiny playback-rate nudges, clamped to ±0.2 % (≈ 3.5 cents, inaudible) — later done through the stretcher, so pitch-neutral |
| Clock jitter | averaged out over two bars of ticks; never causes a jump |
| Master changes tempo | re-follows within ~half a second; while the tempo is moving the phase nudges may go to ±2 % (the loops can't follow a tempo change until the stretcher anyway) |
| Clock disappears | nothing audible — internal clock continues at the last tempo and bar position; LED shows "free-running" |
| Clock comes back | tempo re-tracks; only the sub-tick phase is gently corrected; **bar position is kept** |
| `Stop` | loops fade out (~5 ms) and remember their position |
| `Continue` | loops stopped by `Stop` resume where they were (or at the `SPP` position, if one was sent) |
| `Start` | bar position resets to bar 1; loops stopped by `Stop` restart from the top; playing loops re-align with a crossfade |
| No MIDI clock | tempo knob sets the tempo |
| Tempo changes while loops exist | loops follow, time-stretched, pitch unchanged (direct playback again within 0.6 % of the recorded tempo) |

MIDI has no Pause message: a DAW/clock "pause" is `Stop` → (optional `SPP`) → `Continue`.
Only loops stopped *by MIDI* auto-resume; a track you stopped with the footswitch stays stopped.
Many clock sources also stop sending `0xF8` while stopped — the flywheel doesn't care.

### Implementation (phase 1, `midi_sync.h`)
- Ticks are timestamped in the UART interrupt (which may cut into the audio callback), back-dated
  by the UART's idle-line delay, and parsed in the audio callback by our own parser (`midi_in.h`).
- A least-squares line through the last 192 tick arrival times: slope = tempo, end of the line =
  de-jittered phase. The clock gets that tempo; the phase error goes through the rate trim
  (corrected over ~1.5 s, clamped ±0.2 %).
- Tempo change detection: the newest 12 ticks are compared against the line through the older
  ones; drift beyond 4.5σ of what jitter can explain restarts the fit from the recent ticks.
- Ticks are counted, never inferred from timing, so bunched-up ticks can't shift the bar.
- Start / Continue locate the clock on the next tick, using the fitted grid when the clock was
  already running (a held-up first tick doesn't misplace bar 1).
- Simulated worst case (USB-MIDI from a DAW: 1 ms σ jitter, 1 ms USB frames, 2 % of ticks held up
  2–10 ms): ±0.6 ms around the average arrival delay, steady over an hour; no false tempo changes.

### Tempo knob
- Range ~60–200 BPM, quantized to whole BPM with hysteresis, so pot noise can't wobble the tempo.
- LED beat flash shows the tempo.
- Ignored while a MIDI clock is present. When the clock goes away, the last MIDI tempo stays until
  the knob is moved (pickup), so losing the clock stays inaudible.

### Tempo / bar tracking details
- Timestamp each `0xF8` (audio sample counter + offset within the block, or DWT cycle counter).
- The grid is fixed at 4/4: `ticks_per_beat = 24`, `ticks_per_bar = 96`. No time-signature state.

## Recording and playback

### Quantized record
1. Press → track **armed**; LED blinks on the beat.
2. Recording always starts at the next **bar line**, so every loop's start is anchored to the grid.
3. Ending the recording — chosen by foot, per loop, no mode switch:
   - **Tap** → recording runs to the next **bar line**, then playback starts.
     With **DIP 2 on**, it instead runs to the next power-of-two bar count: stop during
     bar 14 → loop is 16 bars, stop during bar 5 → 8 bars (LED counts down the extra bars).
   - **Double tap** → recording ends at the next **grid step** instead (step size = 3-way 1:
     1/4, 1/8 or 1/16 note). This is how odd lengths happen: 7 beats, 15 eighths, ...
     If the bar line arrives before the second tap, the loop simply ends there — a bar line is also a grid step.
4. Loop length is stored in **ticks**. Tracks have independent lengths, so a 16-beat guitar loop
   against a 7-beat bass loop drifts against itself and lines up again every 112 beats (polymeter).

A preset length (auto-stop after N bars) is available via MIDI CC, where the controller can show the value.

Latency compensation: shift the record window by a fixed, measured input latency so the downbeat lands on the downbeat.

### Seams
~5 ms crossfade at every loop seam, on stop/start and on `Start` re-alignment — no clicks.

### Time-stretch (phase 7, `stretch.h`)
Built as below. WSOLA grains of 1024 samples, hop 512, search +-8 ms in three stages (coarse /
medium / fine, ~10k multiply-adds per grain); the two tracks' grains are staggered by half a hop
so their searches never fall in the same audio block. Within +-0.6 % of the recorded tempo a loop
plays directly (bit-exact, PLL nudges as varispeed); beyond that it crossfades (5 ms) into the
stretcher. Overdubbing a stretched loop holds the overdub aside, re-renders the loop at the
current tempo into the undo buffer after the cancel window (main loop), crossfades over and writes
the held overdub in; undo returns the original at its own length.

### Time-stretch (design)
- Loops are stored at their recorded tempo; playback ratio = current tempo / recorded tempo.
- **WSOLA** (overlap-add of short grains, aligned by cross-correlation): pitch-neutral, cheap
  enough for 2 mono tracks on the Cortex-M7, sounds good for guitar at moderate ratios (±~15 %).
  Large ratios smear transients — a phase vocoder is better there but much heavier.
- The flywheel's phase nudges then also go through the stretcher → fully pitch-neutral sync.

## Track behaviour (per footswitch)

```
empty --tap--> armed --bar--> recording --tap--> (wait for bar / power of two) --> playing
                                         recording --double tap--> (wait for grid step) --> playing
playing --tap--> overdubbing --tap--> playing
playing/overdubbing --double tap--> stopped (per stop mode)
stopped --tap--> armed-play (restarts at next bar)
hold 1 s  --> undo / redo last overdub
hold 2 s  --> clear track
both footswitches tap --> stop all / start all
```

Overdub is classic: the existing loop is kept at full level, new input is added.

### Implementation notes (`track.h`, `footswitch.h`)
- A tap acts the moment the switch goes down (instant punch-in). If it turns out to be the first
  half of a double tap, a hold or a both-footswitch press, its action is taken back.
- Undo: two buffers per track (playing + undo/redo). An overdub writes in place; for its first
  1.2 s the samples it overwrites go to a side buffer, so a double tap or a hold can cancel it
  without touching the previous undo point (hold-to-undo always starts with a press, which starts
  an overdub). After that it's committed and the pre-overdub loop is copied to the undo buffer in
  the background, ahead of the playhead. Undo / redo = swap the buffers, with a crossfade.
- Overdubs are written 2 samples behind the playhead, outside the interpolator's read window.
- Playback follows the clock: position pulled toward where the clock says the loop is (±0.2 %),
  crossfaded jump if more than 10 ms off. Tempo more than 0.6 % off the recorded one: the loop
  plays at its own speed and restarts on every loop boundary (until the stretcher exists).
- Decisions made while building:
  - Tap while armed cancels; tap while a stop is pending keeps playing.
  - A tap up to 80 ms (and at most a quarter of the step) after a boundary ends the loop on it.
  - MIDI Stop pauses playing loops only; an armed track waits for the transport to play, a
    recording carries on on the free-running clock.
  - Hold 1 s while armed or recording discards the take.
  - Tempo knob is locked while any track has a loop (picks back up once moved after clearing).
- Latency compensation: a loopback calibration (hold footswitch 1 at power-up, cable Out L -> In L)
  measures the round trip; overdubs are written that far behind the playhead, first recordings
  skip half of it (the input side). Saved with the settings.
- LEDs: recording with its end set counts down (one dip per bar to go at each bar line, a dip on
  every beat in the last bar); MIDI clock lost = both LEDs breathe slowly.

## Signal flow (dual mono)

```
DIP 1 on (split, two instruments):          DIP 1 off (one source):
In L ─┬─► Track 1 ─► filter ─► level ─┐    In L ─┬─► Track 1 ─► filter ─► level ─┐
      └────────── dry ────────────────┴► Out L    ├─► Track 2 ─► filter ─► level ─┼─► Out L
In R ─┬─► Track 2 ─► filter ─► level ─┐          └────────── dry ─────────────────┴─► Out R (same mix)
      └────────── dry ────────────────┴► Out R
```

- Serial use: dry always passes at unity, like any in-line looper — no dry knob. The live/loop
  balance comes from the track level knobs.
- Recording takes the input at unity; a soft limiter on each output catches hot overdub stacks.
- Each track's filter is DJ-style and bipolar: low-pass to the left of noon, high-pass to the
  right, flat at noon. It's applied on playback only (non-destructive).

## Expression: morph between knob states

Builds on Keith's `include/expressionHandler.h` (heel/toe snapshots of the knobs), adapted for live looping.
Morphable parameters: Track 1/2 level (knobs 1, 3), Track 1/2 filter (knobs 4, 6) and FX amount (knob 5). Tempo (knob 2) is never morphed.
Because one pedal moves several parameters at once, it covers both tracks:

| Example | Heel | Toe |
| --- | --- | --- |
| Track crossfader | T1 level up, T2 level down | T1 down, T2 up |
| Dual wah | both filters dark (LP) | both filters bright (HP) |
| Drop / build | levels as set | both filters high-passed, levels down |

### Model: A/B states + pickup
- Two stored states, **A** and **B**, each a snapshot of the morphable knobs *and the expression pedal position*.
- Only parameters that differ between A and B are mapped; everything else stays knob-only.
- The pedal morphs mapped parameters from A to B between the two recorded pedal positions (clamped
  outside them), so any part of the pedal's travel can be used — no full-range requirement.
- **Pickup / takeover** (DeepMind-style): each parameter has one value and several sources (knob,
  pedal, MIDI CC). The last source to *cross* the current value owns it. Turn a mapped knob and it
  takes over once it catches the value; move the pedal and it takes back over once its morph value
  crosses. Never a jump.
- Cost: a few lerps and comparisons per audio block — negligible on the Seed.

### Learn procedure (DIP 4)
1. Set the knobs to the base sound (usually unity levels, flat filters) and put the pedal where it should start.
2. Flip **DIP 4 on** → state **A** captured. Knobs keep working normally, so you hear what you dial in.
3. Move knobs and pedal to the target sound.
4. Flip **DIP 4 off** → state **B** captured, mapping built and saved to QSPI (`PersistentStorage`, like Jupiter/Mars).
- Flip on and off without changing anything → nothing differs → mapping cleared.
- DIP 4 left on at power-up is ignored until it is toggled.

## State persistence ("it plays like I left it")

Power-on restores the last state; physical controls only take over once *moved*.

- **Snapshot** = every parameter value (track levels, filters, tempo, values set by MIDI CC) +
  expression A/B mapping. Loop audio is not saved.
- **Knobs and expression** restore from the snapshot. A control is ignored until it moves past a
  small threshold (pot noise or an unplugged expression jack doesn't count), then takes over via pickup
  when it crosses the stored value. A short LED flash signals the takeover.
- **Toggles and DIPs** are physical truth: you can see their position, so they always apply.
- **Saving**: there's no power-off detection, so save ahead of time. Debounced: any change (knob,
  pedal, MIDI CC, switch) restarts a ~10 s timer; the write happens when it expires. Sweeping a knob
  produces one write after you let go, never a flood. Two or more slots with a sequence number + CRC: boot
  picks the newest valid one (survives power loss mid-write) and rotating slots spreads flash wear.
- **Storage location**: the external 8 MB QSPI flash, not the internal flash the firmware lives in.
  Reflashing via DFU (`BOOT_NONE`) only rewrites internal flash, so the snapshot survives firmware updates.
  If we move to the Daisy bootloader (`BOOT_SRAM`), the app itself lives in QSPI from 0x90040000 —
  the snapshot then goes in the top sectors of QSPI, away from the app.
- **Firmware changes the snapshot layout**: libDaisy's `PersistentStorage` only checks a state byte,
  so a changed struct would load garbage. Our snapshot carries magic + version + CRC: mismatch →
  defaults (or a migration). Own small store on QSPI Erase/Write instead of `PersistentStorage`,
  which also gives us the slot rotation.
- **Audio during a save**: erase/write blocks the main loop (~tens of ms per 4 KB sector), not the
  audio interrupt — fine as long as the audio path never reads QSPI-mapped memory. MIDI bytes are
  buffered by UART DMA meanwhile, which is one more reason clock ticks are timestamped on arrival, not in the main loop.

### Implementation notes (`params.h`, `persist.h`)
- A source that loses a parameter has to move past the threshold again before it can take it
  back, so a knob resting where the pedal's morph passes by doesn't grab it.
- Knob readings ramp up from 0 at power-on (libDaisy's smoothing): they're ignored for 200 ms,
  otherwise the ramp would "cross" every saved value below the knob.
- MIDI CC jumps instead of picking up (deliberate values; DAW automation).
- Snapshot slots: top 16 KB of QSPI (0x907FC000), 4 x 4 KB, newest valid sequence wins.
- While DIP 4 is on, the morph is off; saving waits until the learn is finished.

## Loop FX (3-way 2 + knob 5)

Applied to the loops only (dry stays clean), one instance per output so split mode keeps guitar and bass apart.
Knob 5 is morphable, so the expression pedal can drive it.

| 3-way 2 | Knob 5 (0 = off) |
| --- | --- |
| Tape | wow/flutter + saturation + treble loss: clean → worn cassette → seasick |
| Delay | clock-synced (dotted 1/8 default, time via MIDI CC); mix + feedback together: slapback → long dark self-oscillating tails |
| Repeat | clock-synced beat repeat; slice shrinks as you turn: 1/4 → 1/8 → 1/16 → 1/32 (pedal heel→toe = build-up roll) |

## Control layout

Mirrored: left column = Track 1, right column = Track 2 (above their LED + footswitch), centre = global.

```
  K1 T1 level    K2 Tempo      K3 T2 level
  K4 T1 filter   K5 FX amount  K6 T2 filter
  Tog1 Grid      Tog2 FX type  Tog3 Stop mode
  LED1 / FS1 Track 1           LED2 / FS2 Track 2
  DIPs on the side
```

| Control | Function |
| --- | --- |
| Knob 1 | Track 1 level |
| Knob 2 | Tempo (only without MIDI clock) |
| Knob 3 | Track 2 level |
| Knob 4 | Track 1 filter (LP ← noon → HP) |
| Knob 5 | FX amount (0 = off) |
| Knob 6 | Track 2 filter (LP ← noon → HP) |
| 3-way 1 | Grid step for double-tap endings: 1/4 / 1/8 / 1/16 |
| 3-way 2 | FX type: Tape / Delay / Repeat |
| 3-way 3 | Stop mode: Immediate / End of bar / End of loop |
| DIP 1 | Split: In L/Out L = Track 1, In R/Out R = Track 2 (off = In L feeds both, both outs = same mix) |
| DIP 2 | Round loop length up to a power of two (1, 2, 4, 8, 16, 32 bars) |
| DIP 3 | Reserved |
| DIP 4 | Expression learn: on = capture state A, off = capture state B + save |
| FS 1 / FS 2 | Track 1 / Track 2 (see state machine) |
| LED 1 / LED 2 | Track state (record = solid bright, overdub = fast blink, play = solid dim, armed = beat blink); short brightness dip on every beat, longer on bar 1; slow pulse = free-running (clock lost) |
| Expression | Morph between knob states (see Expression) |

## MIDI CC map (draft)

| CC | Action |
| --- | --- |
| 20 / 21 | Track 1 / 2 record-play-dub (same as tap) |
| 22 / 23 | Track 1 / 2 stop |
| 24 / 25 | Track 1 / 2 undo |
| 26 / 27 | Track 1 / 2 clear |
| 28 | All start/stop |
| 29 | Preset loop length in bars (0 = off, play-defined) |
| 30 | Grid step (1/4, 1/8, 1/16) |
| 7 / 8 | Track 1 / 2 level |
| 74 / 75 | Track 1 / 2 filter |
| 76 | FX amount |
| 77 | Delay time (1/4, dotted 1/8, 1/8, 1/8T, 1/16) |

Channel: omni for v1, configurable later.

## Implementation phases

0. **MIDI bring-up** — new pedal from `software/Template`; blink an LED on every incoming beat
   and on bar 1. This also confirms MIDI in actually works on the board
   (Earth has a `// MIDI is not working for some reason` TODO).
1. **Internal clock + flywheel PLL + tempo knob** — tempo tracking, clock loss/return, `Start` / `Stop` / `Continue` / `SPP`.
2. **Track 1** — quantized record / play / overdub / stop, power-of-two rounding, seam crossfades, filter.
3. **Editing** — undo/redo, clear, stop modes.
4. **Track 2 + dual-mono routing** — both-footswitch gestures, MIDI CC map.
5. **Loop FX** — Tape, Delay, Repeat.
6. **Expression morph + state persistence** — A/B learn on DIP 4, pickup for knobs / pedal / MIDI CC, power-on restore, QSPI snapshots.
7. **Time-stretch (WSOLA)** — tempo changes with loops present, pitch-neutral phase correction.
8. **Maybe later** — loop saving to QSPI, knob automation.
