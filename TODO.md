# TODO

## Current tip

Orientation / heading clarity fix (pending commit on top of fc6bec5).

## Done in this tip

- Unified orientation convention: angle=0 nose-up, positive angle = clockwise (matches SDL + y-down).
- Fixed SDL_RenderCopyEx sign (was inverted vs physics).
- Fixed exhaust direction (was mirrored).
- Clearer procedural ship: sharp nose, gold tip, center keel, distinct rear engines.
- Yellow heading tick + crossbar drawn toward the nose.

## Open work

- [ ] Polish physics / tuning (thrust, gravity, drag, engine offset)
- [ ] Add simple landing pads / crash detection if desired
- [ ] Better exhaust VFX / particles
- [ ] Sound (engine noise)
- [ ] Full official GPL-3.0 text in LICENSES/ (currently short placeholder)

## Handoff

- Bundle naming base: dualthrust
- Original base short: 375fc76
- Author: Ingo Ruhnke <grumbel@gmail.com>
- Co-authored-by: Grok <grok@x.ai>
