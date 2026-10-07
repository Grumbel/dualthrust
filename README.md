<!--
SPDX-License-Identifier: GPL-3.0-or-later
-->
# dualthrust

CRT dual-engine **cave** lander. Left/right triggers = engines. Land on pads inside a wrapping 2D cave.

## Controls

| Input | Action |
|-------|--------|
| Triggers / L1 R1 / sticks up | Engines |
| **Start** | Menu |
| Menu Up/Down + A | Navigate / select |
| F / Alt+Enter | Fullscreen |
| A/B or R | Reset / relight |
| Y or G | New cave |
| M | Sound on/off |
| Escape | Menu / quit |

Menu items: Resume, Fullscreen, New Cave, Ship Preset, Swap Engines, Sound, Quit.

## World

- 24000×4800 cave, wraps in X
- Carved tunnels, chambers, stalactites, pillars, landing pads
- Vertical + horizontal camera scroll
- Background dots for motion in open space
- Minimap of the whole wrapped world; exhaust, landing and crash particles

## Build

```sh
nix develop
dualthrust-run
# or nix run
```

## R36S handheld (ArkOS / PortMaster)

`nix build .#dualthrust-r36s-portmaster-zip` — see AGENTS.md for the sysroot requirement.

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

Ship presets include **Topdog** and **Canopy** with engines mounted on top (still thrust ground-ward for lift).
