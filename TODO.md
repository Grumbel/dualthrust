# TODO

## Current tip

`add1212` — Add ship size presets (Select/Back) and slow the simulation

Bundle: `dualthrust-004.1-ship-presets-375fc76.bundle` (base 375fc76)

## Done recently

- Orientation fix (clockwise angle, exhaust, heading marker)
- Five ship presets: Narrow / Medium / Wide / Barge / Long
  (size, engine spacing, mass, inertia, max thrust)
- Select/Back (Tab / [ / ]) cycles presets; name shown on HUD
- TIME_SCALE 0.62 slows the whole sim

## Open work

- [ ] Polish physics / tuning per preset
- [ ] Landing pads / crash detection
- [ ] Better exhaust VFX / particles
- [ ] Sound (engine noise)
- [ ] Full official GPL-3.0 text in LICENSES/

## Handoff

- Bundle naming base: dualthrust
- Original base short: 375fc76
- Author: Ingo Ruhnke <grumbel@gmail.com>
- Co-authored-by: Grok <grok@x.ai>
- Apply: `git pull path/to/dualthrust-004.1-ship-presets-375fc76.bundle HEAD`
