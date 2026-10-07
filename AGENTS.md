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

- Triggers (or L1/R1, or sticks up): engines
- Start: open/close menu
- Menu: D-pad/Up-Down, A/Enter select — Resume, Fullscreen, New Cave, Ship, Swap Engines, Sound, Quit
- M: sound on/off (saved in config; `-m/--mute` for one run)
- A/B or R: reset after crash / relight after land
- Y or G: new cave
- F: fullscreen
- Escape: menu (or quit from menu)

## World

- Toroidal X (24000px), tall cave Y (4800px)
- 2D cellular cave: main tunnel, branches, stalactites/mites, pillars, pads
- Camera scrolls freely with velocity look-ahead; background star dots for motion reference
- Tunnel is blended across the X seam; pads are only placed in the main connected cave
- Minimap (bottom centre) shows the whole wrapped world, pads and ship

## Source layout (`src/`)

Small sparse-set ECS plus data tables; all tuning/art data lives in `defs.hpp`.

- `ecs.hpp` — `Pool<T>` / `Registry<Cs...>` with `view<Driver, Others...>(f)`; queue destroys, never destroy inside `view`
- `defs.hpp` — tuning constants, palette, `SHIP_DEFS` table (add a ship = add a row)
- `game.hpp` — components (`Transform`, `Motion`, `Hull`, `Thrusters`, `Flight`, `Particle`), `Camera`, `Game`
- `systems.cpp` — flight, collision, exhaust, particles, events, camera; fixed 120 Hz step from `main.cpp`
- `cave.cpp` — generation stages + baked per-cell `depth` / `contour` (rendering reads these, never recomputes)
- `render.cpp` — `Gfx`: glyph atlas, rock-tile atlas, CRT overlay, minimap, flames, HUD, menu
- `audio.cpp` — synthesised sound (no assets): engine rumble, landing/bounce/crash effects, generative Am-F-C-G music with echo; mixed in the SDL callback, fed from `Game::fired`
- `ui.hpp` — `MENU_ITEMS` table; `config.cpp` — XDG config; `main.cpp` — args, input, loop

Debug flags: `--play`, `--thrust L,R`, `--frames N`, `--screenshot FILE.bmp` (works headless with
`SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software`).

New `.cpp` files must be listed in `CMakeLists.txt` and `git add`ed (flake builds only see tracked files).

## R36S / ArkOS handheld port

Native aarch64 build linked against an ArkOS sysroot (glibc 2.30, SDL2 ≥ 2.0.10, 640×480 panel); same
approach as pingus (`nix/r36s.nix` mirrors `pingus/nix/r36s.nix`, trimmed to SDL2 only).

```sh
nix build .#dualthrust-r36s                 # binary
nix build .#dualthrust-r36s-portmaster      # Dualthrust.sh + dualthrust/ for /roms/ports
nix build .#dualthrust-r36s-portmaster-zip  # PortMaster autoinstall zip
```

- The sysroot is the flake input `github:grumnix/arkos-sysroot` (pinned in `flake.lock`; `nix flake update arkos-sysroot`
  to refresh). Dev shortcut: `DUALTHRUST_ARKOS_SYSROOT=/nix/store/…-arkos-sysroot-… nix build --impure .#dualthrust-r36s`
- Draw calls are the bottleneck on the Mali GPU (SDL 2.0.10's GLES2 renderer issues one GL draw per render
  call): the static world is baked into cached CPU-rasterised chunk textures, polygons/text/particles go
  through `SDL_RenderFillRects` batches. Measured on device: 16 ms/frame in play (was 33 ms). Check with
  `./dualthrust --play --frames 300` (prints per-frame sim/draw/present ms). Avoid per-glyph/per-line calls.
- Keep to SDL ≤ 2.0.10 API (headers in the sysroot; no `SDL_RenderGeometry` — see `Gfx::triangle`).
- `mk/r36s/cxxabi_shim.cpp` shims GCC 15 → old libstdc++/glibc symbols (`-DDUALTHRUST_CXXABI_SHIM`).
- Launcher exports `XDG_CONFIG_HOME` into the port dir, so ship/sound settings persist there.
- Verified: aarch64 ELF, needs only GLIBC ≤ 2.17 / GLIBCXX 3.4 / CXXABI 1.3.9, loads and reaches `SDL_Init`
  under qemu. Not yet run on real hardware.

## Packaging

CMake installs binary, `.desktop`, hicolor icons, AppStream metainfo.

## License

GPLv3-or-later.
