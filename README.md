<!--
SPDX-License-Identifier: GPL-3.0-or-later
-->
# dualthrust

Dual-engine lander with a green CRT / oscilloscope look. Triggers only for thrust; differential thrust rotates and translates. Lunar Lander–style fractal terrain, scrolling camera, and pad landings.

## Controls

| Input | Action |
|-------|--------|
| Left / right triggers | Left / right engine |
| Select / Back | Cycle ship preset |
| Start | Swap L/R engine mapping |
| A / B (pad) or `R` | Reset after crash / relight after land |
| Y (pad) or `G` | New fractal terrain |
| `Tab` / `[` / `]` | Preset (keyboard) |
| `X` | Swap engines (keyboard) |

Landing lights (HUD top-right): **PAD OK**, **SPEED OK**, **ATT OK** — all three green for a safe touchdown.

## Build

```sh
nix develop
dualthrust-configure && dualthrust-build && dualthrust-run
# or
nix run
```

## License

GPL-3.0-or-later.
