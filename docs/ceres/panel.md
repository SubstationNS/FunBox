# Ceres — panel and artwork spec

For the enclosure face artwork and the infographic. Everything here matches the firmware.

## Hardware

- **FunBox v3** PCB in a **Hammond 125B** enclosure (about 66 × 121 mm face, 39 mm deep).
- Drill layout = the FunBox v3 (see `hardware/images/funbox_v3_front.jpg` and the FunBox
  infographic): 2 rows of 3 knobs, a row of 3 mini toggles, 2 LEDs above 2 footswitches.
- Jacks on top (In L, In R, Out L, Out R, 9 V). Expression jack on the left side (top), MIDI jack
  and USB on the right side, 4 DIP switches on the right side (lower).

## Face layout (top to bottom, left to right)

| Position | Control | Label | Notes |
| --- | --- | --- | --- |
| Row 1 left | Knob 1 | **T1 LEVEL** | unity ≈ 2 o'clock (optional tick mark) |
| Row 1 middle | Knob 2 | **TEMPO** | 60–200 BPM (optional scale: 60 / 120 / 200) |
| Row 1 right | Knob 3 | **T2 LEVEL** | unity ≈ 2 o'clock |
| Row 2 left | Knob 4 | **T1 FILTER** | scale: LP ← • → HP (noon = flat) |
| Row 2 middle | Knob 5 | **FX** | 0 = off |
| Row 2 right | Knob 6 | **T2 FILTER** | scale: LP ← • → HP |
| Toggle 1 (left) | Grid | **GRID** | positions left / center / right: **1/4 · 1/8 · 1/16** |
| Toggle 2 (middle) | FX type | **FX TYPE** | **TAPE · DELAY · REPEAT** |
| Toggle 3 (right) | Stop mode | **STOP** | **NOW · BAR · LOOP** |
| LED left | LED 1 | — | Track 1 status |
| LED right | LED 2 | — | Track 2 status |
| Footswitch left | FS 1 | **TRACK 1** | |
| Footswitch right | FS 2 | **TRACK 2** | |
| Name | | **CERES** | subtitle: *dual bar looper* |

Mirrored design idea: left column = Track 1, right column = Track 2, middle = shared. A subtle
visual grouping (e.g. a thin outline or color band per column) makes that obvious.

## Side labels

| Side | Label |
| --- | --- |
| Left, top | **EXP** |
| Right, top | **MIDI** (Type A) |
| Right, middle | **USB** |
| Right, lower | **DIP**: 1 SPLIT · 2 POW2 · 3 — · 4 LEARN |
| Top edge | **OUT R · OUT L · 9V · IN R · IN L** (check against your jack order) |

## Name

Ceres — the dwarf planet in the asteroid belt (the FunBox pedals are named after planets: Earth,
Mars, Jupiter, Venus...). Roman goddess of harvest and growth: layers growing on layers.
