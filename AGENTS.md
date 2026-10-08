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

- Triggers (or L1/R1, or sticks up): engines. Keyboard: L/R Ctrl (or A/D, arrows) full, L/R Shift half thrust
- Start: open/close menu
- Menu: D-pad/Up-Down, A/Enter select — Resume, Fullscreen, New Cave, Ship, Zoom, Swap Engines, Sound, Quit
- On-screen hints follow the last-used device (`UiState::device`: keyboard events/wheel vs gamepad buttons/axes)
- Zoom: Tab (cycle), mouse wheel, D-pad up/down in play; `-z near|medium|far`. Ship: S or Select.
- M: sound on/off (saved in config; `-m/--mute` for one run)
- Enter (keyboard) or A/B (gamepad): respawn after crash (landed ships lift off automatically when thrust is applied)
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
- `render.cpp` — zoom: `ZOOM_LEVELS` (defs.hpp) fix the world px visible vertically (480/720/1080), so scale = screen_h / visible_h,
  resolution independent. The world layer is *vector*-scaled, not pixel-scaled: chunks are rasterised at the zoom level's own
  scale (outline, hatch pitch, stars in global pixel coordinates so chunk seams are invisible), the ship/pads/particles are
  placed and sized in screen px via `sx()/sy()/Z()`, and 1 px line width never scales. The chunk cache is rebuilt when the
  level changes (stretched, filtered, while the zoom glides); camera `vw/vh` are the visible world px. `Gfx`: cached world chunk textures, CRT overlay, minimap, flames, batched text/polygons, HUD, menu
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
- Draw calls are the bottleneck on the Mali GPU. SDL 2.0.10's GLES2 backend issues one `glDrawArrays` per
  `SDL_RenderCopy` and per *rect* of `SDL_RenderFillRects` (only consecutive same-blend two-point lines are grouped, and a
  colour change breaks the group); SDL >= 2.0.18 merges same-texture/blend commands and has `SDL_RenderGeometry`. So
  `FillRects` arrays save API overhead on the device, not GL draws. What really helped: the static world is baked into
  cached CPU-rasterised chunk textures (about 3000 draws -> about 12; 33 ms -> 16 ms/frame). Check with
  `./dualthrust --play --frames 300` (prints per-frame sim/draw/present ms). Avoid per-glyph/per-line/per-particle calls;
  the next step would be a small GLES2 renderer with a real vertex batcher.
- Keep to SDL ≤ 2.0.10 API (headers in the sysroot; no `SDL_RenderGeometry` — see `Gfx::gradient_triangle`).
- `mk/r36s/cxxabi_shim.cpp` shims GCC 15 → old libstdc++/glibc symbols (`-DDUALTHRUST_CXXABI_SHIM`).
- Launcher exports `XDG_CONFIG_HOME` into the port dir, so ship/sound settings persist there.
- Verified: aarch64 ELF, needs only GLIBC ≤ 2.17 / GLIBCXX 3.4 / CXXABI 1.3.9. Runs on a real R36S (ArkOS,
  SDL 2.0.10, KMSDRM + opengles2) at 60 fps in play. Not yet tested through the PortMaster launcher or with
  the real controller mapping.

## Versioning

- `VERSION` (plain text, top level) is the only source of truth; on the main branch it always ends in `-dev`
  (currently `0.1.0-dev`). Never hardcode the version elsewhere.
- Dev builds are `0.1.0-dev.<revCount>+g<shortRev>[-dirty]` (Nix: `self.revCount or 0`); release builds
  (`VERSION` without `-dev`) use the file as-is.
- CMake reads `VERSION` into `PROJECT_VERSION_FULL` (packaging overrides it with `-DPROJECT_VERSION_FULL=…`),
  strips the suffix for `project(VERSION)`, defines `DUALTHRUST_VERSION` (→ `--version`) and fills the man page
  (`data/man/dualthrust.1.in`).
- Release: set `VERSION` to `x.y.z`, commit, tag `vx.y.z`, then bump to the next `-dev`.

## WebAssembly build

Emscripten build after Kurvenrausch (`nix/wasm.nix`, `mk/wasm/`), fully offline: SDL2 2.30.3 comes from a flake
input and is built static (`sdl2-wasm`); the game itself needs nothing else and has no data files.

```sh
nix build .#dualthrust-wasm   # site in result/: dualthrust.html (= index.html), .js, .wasm
nix run .#dualthrust-wasm     # serve it on 127.0.0.1:8765 and open a browser (DUALTHRUST_WASM_PORT, BROWSER)
```

- `main()` builds one `frame_fn` lambda: desktop loops over it, the browser runs it from
  `emscripten_set_main_loop` (simulate-infinite-loop keeps `main`'s locals alive). Escape toggles the menu; there
  is no Quit item, and fullscreen is never restored at startup (browsers need a user gesture).
- `~/.config/dualthrust` is an IndexedDB (IDBFS) mount set up by `mk/wasm/shell.html`; `save_config()` calls
  `FS.syncfs` after each write, so ship/sound/swap persist across reloads. The page also calls `dualthrust_pause`
  (opens the menu) when the tab is hidden or its Menu button is pressed.
- The web build uses SDL 2.30, but keep avoiding newer-SDL-only calls (the R36S shares the code); the draw-call
  batching helps WebGL too.
- Testing: serve `result/` and drive it in real time over the Chrome DevTools protocol (headless Chromium;
  virtual-time mode never completes the IndexedDB sync, so the page stays at "Preparing…"). Verified: boots,
  WebGL renderer, keyboard (menu, Ctrl/D thrust), settings survive a reload. Untested: gamepad, audio output.

## Packaging

CMake installs binary, `.desktop`, hicolor icons, AppStream metainfo.

## License

GPLv3-or-later.
