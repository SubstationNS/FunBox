# Prompt for the Ceres manual and artwork

Copy everything between the lines into Claude (claude.ai). If the image links don't load there,
download the images and attach them to the message instead.

---

I'm building **Ceres**, a two-track MIDI-synced looper pedal on the FunBox v3 platform (Daisy Seed,
Hammond 125B enclosure). The firmware is finished; I need the documentation and artwork designed.

**Source material (read all of it first):**
- Full manual content, accurate to the firmware — use this as the text, don't invent features:
  https://raw.githubusercontent.com/SubstationNS/FunBox/main/docs/ceres/manual.md
- Panel / artwork spec (every label, position, switch order, side jacks):
  https://raw.githubusercontent.com/SubstationNS/FunBox/main/docs/ceres/panel.md
- The FunBox v3 hardware (photo and render — this is the physical layout to draw):
  https://raw.githubusercontent.com/SubstationNS/FunBox/main/hardware/images/funbox_v3_front.jpg
  https://raw.githubusercontent.com/SubstationNS/FunBox/main/hardware/images/funbox_v3_render.jpg
- House style of the other FunBox pedal infographics (match this for #2):
  https://raw.githubusercontent.com/SubstationNS/FunBox/main/software/images/funbox_infographic.jpg
  https://raw.githubusercontent.com/SubstationNS/FunBox/main/software/images/earth_infographic.jpg
  https://raw.githubusercontent.com/SubstationNS/FunBox/main/software/images/jupiter_infographic.jpg

**Please make three things:**

1. **A printable PDF user manual** (A5 portrait, booklet-friendly, ~12–16 pages):
   - A cover with the pedal illustration and the name **CERES** (subtitle *dual bar looper*).
   - A one-page **quick start** (power, plug in, tap to arm, tap to close the loop, tap to overdub,
     hold to undo, double tap to stop) right after the cover.
   - A **panel overview page**: a clean drawing of the pedal face with every control called out
     (from panel.md), plus the side jacks and DIP switches.
   - Then the manual sections from manual.md, with the tables kept as real tables. Add small
     diagrams where they help: the footswitch gesture table, mono vs. split routing (DIP 1), the
     expression A/B morph (heel → toe), the recording countdown, and an LED legend.
   - Style: clean and technical, generous whitespace, one accent color, readable at A5 print size.
     A subtle planet / orbit motif for Ceres (the dwarf planet in the asteroid belt) is welcome but
     shouldn't get in the way of the content.
   - Deliver it as a single HTML document laid out for A5 print (CSS @page, page breaks) that I can
     print to PDF, plus the PDF if you can produce one.

2. **A Ceres infographic** in the same style and size as the FunBox pedal infographics above
   (about 1100 × 650 px): the pedal drawn in the middle, call-out boxes around it for the knobs,
   toggles, DIP switches and footswitches, a title box ("**Ceres** is a two-track MIDI-synced bar
   looper") and a small feature box (e.g. "Mono / Split · Expression: YES · MIDI: YES"). SVG or PNG.

3. **Enclosure face artwork** for the 125B (face about 66 × 121 mm): all labels from panel.md at
   the FunBox v3 control positions, the CERES name, scale marks for the filter knobs (LP · HP) and
   the level knobs (a unity tick at about 2 o'clock), and toggle position labels. Vector (SVG),
   print-ready, with the knob / toggle / LED / footswitch holes marked as guides on a separate
   layer. I'll check it against my drill template before printing, so mark it "check against drill
   template".

Keep the wording of controls and behavior exactly as in manual.md and panel.md.

---
