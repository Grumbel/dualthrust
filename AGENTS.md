# dualthrust

A small 2D spaceship game inspired by Space Taxi. The player controls a ship with two engines (left and right) using only the gamepad triggers. Differential thrust provides both translation and rotation.

## Stack

- C++17
- SDL2 (video, events, gamecontroller, render)
- CMake + Ninja
- Nix flake for reproducible build and develop shell

## Build / run (Nix)

```sh
nix develop
dualthrust-configure
dualthrust-build
dualthrust-run
# or: dualthrust-run-gdb
```

Or:

```sh
nix run
```

## Controls

- Left trigger: left engine thrust (0–1)
- Right trigger: right engine thrust (0–1)
- Select / Back: cycle ship preset (Narrow / Medium / Wide / Barge / Long)
- Escape / window close: quit

Keyboard fallback: A/D or arrows = engines; Tab / [ / ] = cycle preset.
Simulation runs at TIME_SCALE 0.62 of wall-clock.

## License

GPLv3-or-later. REUSE compliant.
