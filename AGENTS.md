# dualthrust

CRT dual-engine cave lander. Triggers control left/right engines; differential thrust rotates and translates.

## Stack

- C++17, SDL2, CMake + Ninja, Nix flake

## Build / run

```sh
nix develop
dualthrust-configure && dualthrust-build && dualthrust-run
# or: nix run
```

## Controls

- Triggers: engines
- Start: open/close menu
- Menu: D-pad/Up-Down, A/Enter select — Resume, Fullscreen, New Cave, Ship, Swap Engines, Quit
- A/B or R: reset after crash / relight after land
- Y or G: new cave
- F: fullscreen
- Escape: menu (or quit from menu)

## World

- Toroidal X (24000px), tall cave Y (4800px)
- 2D cellular cave: main tunnel, branches, stalactites/mites, pillars, pads
- Camera scrolls freely; background star dots for motion reference

## Packaging

CMake installs binary, `.desktop`, hicolor icons, AppStream metainfo.

## License

GPLv3-or-later.
