# dualthrust

A small 2D dual-engine lander in a green CRT / oscilloscope look. Mildly inspired by Space Taxi and Lunar Lander. The player controls a ship with two engines using only the gamepad triggers; differential thrust provides translation and rotation.

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
```

Or: `nix run`

## Controls

- Left / right triggers: engine thrust
- Select / Back: cycle ship preset (Narrow / Medium / Wide / Barge / Long)
- Start: swap left/right engine mapping
- A / B (or keyboard R): reset after crash, or relight after landing
- Y (or keyboard G): regenerate fractal terrain
- Escape: quit

Keyboard fallback: A/D or arrows = engines; Tab / [ / ] = preset; X = swap.

## Gameplay

- World is **24000px** wide and **wraps in X** (fly off one side, appear on the other).


- Fractal (midpoint-displacement) ground with flat landing pads
- Camera scrolls with the ship across an 8000px-wide world
- Soft landing requires: on a pad, low horizontal/vertical speed, near-upright attitude
- Otherwise contact = crash; reset to spawn above a pad

## License

GPLv3-or-later. REUSE compliant.
