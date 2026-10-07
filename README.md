<!--
SPDX-License-Identifier: GPL-3.0-or-later
-->
# dualthrust

CRT dual-engine **cave** lander. Left/right triggers = engines. Land on pads inside a wrapping 2D cave.

## Controls

| Input | Action |
|-------|--------|
| Triggers | Engines |
| **Start** | Menu |
| Menu Up/Down + A | Navigate / select |
| F | Fullscreen |
| A/B or R | Reset / relight |
| Y or G | New cave |
| Escape | Menu / quit |

Menu items: Resume, Fullscreen, New Cave, Ship Preset, Swap Engines, Quit.

## World

- 24000×4800 cave, wraps in X
- Carved tunnels, chambers, stalactites, pillars, landing pads
- Vertical + horizontal camera scroll
- Background dots for motion in open space

## Build

```sh
nix develop
dualthrust-run
# or nix run
```

## Desktop install (Linux)

`bin/dualthrust`, `share/applications/dualthrust.desktop`, icons, AppStream metainfo.

## License

GPL-3.0-or-later.

## Config

Settings (fullscreen, ship preset, engine swap) are stored under `$XDG_CONFIG_HOME/dualthrust/config` (default `~/.config/dualthrust/config`).

## CLI

```
dualthrust --help
dualthrust --fullscreen --ship 2 --seed 42
```

Man page: `man dualthrust` after install.

Controls: triggers **or** analog stick up (left stick → left engine, right stick → right engine).
