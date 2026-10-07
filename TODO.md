# TODO

## Current tip

`375fc76` — Initial dualthrust: dual-engine spaceship with gamepad trigger controls

Bundle: `dualthrust-001.1-initial-375fc76.bundle` (base = this root commit)

## Open work

- [ ] Polish physics / tuning (thrust, gravity, drag, engine offset)
- [ ] Add simple landing pads / crash detection if desired
- [ ] Optional keyboard already present as fallback (A/D, arrows)
- [ ] Better ship sprite / exhaust VFX
- [ ] Sound (engine noise)
- [ ] Full official GPL-3.0 text in LICENSES/ (currently short placeholder)

## Handoff

- Project root: this repo
- Bundle naming base: dualthrust
- Next bundle number: 002 (or 001.2 if amending same tip work)
- Author: Ingo Ruhnke <grumbel@gmail.com>
- Co-authored-by: Grok <grok@x.ai>
- Apply: `git pull /path/to/dualthrust-001.1-initial-375fc76.bundle HEAD` into a fresh repo, or `git clone ...` from the bundle.

## Notes

- Keyboard fallback (A/D or arrows) is included for development without a pad; primary design is triggers only.
- No formal levels yet — free flight with gravity and soft screen bounds.
