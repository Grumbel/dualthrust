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
- Start/Escape: pause menu (Escape also = back; quits from the title screen on desktop)
- Screens: title (Start/Options/Statistics/Quit) → play ⇄ pause (Resume/Respawn/New Cave/Options/Statistics/Main Menu/Quit); Options: Ship, Zoom, UI Scale,
  Swap Engines, Music + Effects sliders, CRT Effect, Fullscreen, Controls, Back. Pages are data in `ui.hpp`; left/right change values
- On-screen hints follow the last-used device (`UiState::device`: keyboard events/wheel vs gamepad buttons/axes)
- Zoom: Tab (cycle), mouse wheel, D-pad up/down in play; `-z near|medium|far`. Ship: S or Select.
- M: sound on/off (saved in config; `-m/--mute` for one run)
- Space (keyboard) or X (gamepad): retract / extend the landing legs
- Q/E or D-pad left/right (one press): hook cable fully out / fully in (no in-betweens); R or A (gamepad): hook grabs / releases a crate (A respawns when crashed)
- Enter (keyboard) or A/B (gamepad): respawn after crash (a landed ship lifts off when thrust is applied)
- Y or G: new cave
- F: fullscreen

## World

- Bounded 4:3 map, 7680×5760 px, ringed by solid rock (4 cells at the sides, 4 at roof and floor); nothing wraps
- 2D cellular cave: main tunnel, branches, stalactites/mites, pillars, pads
- Camera scrolls freely with velocity look-ahead; background star dots for motion reference
- Main tunnel runs side to side; pads are only placed in the main connected cave
- Minimap (bottom centre, 400×100 px) is a window of one texel per cave cell that scrolls with the ship (`Backend::copy_part`); pads, crates, ship and view box

## Source layout (`src/`)

Small sparse-set ECS plus data tables; all tuning/art data lives in `defs.hpp`.

- `ecs.hpp` — `Pool<T>` / `Registry<Cs...>` with `view<Driver, Others...>(f)`; queue destroys, never destroy inside `view`
- `defs.hpp` — tuning constants, palette, `SHIP_DEFS` table (add a ship = add a row)
- `game.hpp` — components (`Transform`, `Motion`, `Hull`, `Thrusters`, `Flight`, `Body`, `Legs`, `Particle`), `Camera`, `Game`.
  `Transform`/`Motion` are mirrors of the Box2D bodies (px, px/s) written by `sync_system`; particles are plain ECS entities, not bodies.
- `physics.hpp/.cpp` — Box2D 3.1 (C API) wrapper: world, terrain streaming, ship bodies. Game code works in px (+y down),
  Box2D in metres (`PPM` = 32 px/m; `to_b2`/`from_b2`). Collision categories `CAT_*`, shape user data `Part` (hull/foot/strut/terrain).
  - Terrain = the cave's marching-squares contour (the line the renderer draws), traced into polylines and made one-sided
    chain shapes (right-hand side = open air), per-segment friction (pads grippier). 30×30-cell chunks (480 px; 50×10 tile
    the world exactly) are built on demand around the ship, one-cell overlap, at most 2 per tick, dropped beyond 2 chunks.
  - Ship = hull (cabin polygon + two engine bells), COM well below the hull centre (stands stable on slopes; thrust acts along
    the axis so flight is unchanged), and two light leg bodies on prismatic joints (`leg_geom()` in `defs.hpp`). The leg spring
    is applied by hand in `forces_system` (joint springs scale with the reduced mass, which is the light leg's), target 0 =
    extended, `-travel` = retracted. Sleeping bodies are left alone (`b2Body_IsAwake`); moving a sleeping body does not wake it.
  - Chain gotchas: open chains need a ghost point at each end (first and last point create no segment); chains are one-sided.
- Rope and cargo: the cable is a b2 distance joint hull→hook with `enableSpring` at 0 Hz and a min/max length (that is how Box2D
  makes a rope); `rope_system` reels `Rope::length` toward `rope::OUT_LEN` or `MIN_LEN` depending on `Rope::out` (`rope::*` in `defs.hpp`). Grab = revolute joint hook→crate at
  the hook's position (a pendulum) with a friction motor, crate damping raised and crate-vs-ship collision off while held
  (`release_crate` undoes it). The cable anchors at the COM (`com_y`), which keeps the load from twisting the ship. Crates (`Cargo` + `Transform` entities, `CARGO_DEFS`, spots from `Cave::cargo`) are created
  *disabled* and enabled only while the ground around them is built (`Physics::terrain_at`, `cargo_activation`); moving crates
  are extra stream anchors. Delivery = lifted crate resting 1 s on a pad (`cargo_system`). Rope visual: sagging chain in `draw_rope`.
  Crates collide with terrain, ship, hook and each other; the hook only with terrain and crates; the cable itself never collides.
- Thrusters: `ThrusterDef` lists (`FRIGATE_T` …) give position, push angle (0 = toward the nose, +90° = right, 180° = down), power and
  control channel 0..3 (left trigger, right trigger, left stick up, right stick up); ships without a list have the classic pair.
  `thruster_pose()` is what forces, exhaust particles, flames and bells use; `channel_count()` is 2 or 4 and `read_thrust()` folds the
  sticks into channels 0/1 for 2-channel ships. `Thrusters::level[4]` is per channel. Big ships set `engine_offset_x/y` to the outboard
  main engines so legs and belly geometry keep working, and `STYLE_DECK` (a collidable belly deck).
- Ships: `SHIP_DEFS` rows + `ShipStyle` flags (fins/tanks/dome/dish/stripes drawn in `draw_ship`; tanks also add collision shapes in
  `physics.cpp`). `hull_geom()`/`leg_geom()`/`winch_y()` in `defs.hpp` are shared by drawing and physics.
- Cave generation (`cave.cpp`): noise-warped rock density around a wandering spine that thins toward both map edges, majority-CA
  smoothing, side chambers, meandering branches, then (after smoothing, whose erosion would eat them) rock islands, cone-shaped
  stalactites/stalagmites, pads, crate spots.
- `systems.cpp` — `forces_system` (thrust, leg springs) → `Physics::step` → `sync_system` → `impact_system` (hit events: crash
  or bounce) → `ground_system` (touching/resting; `Flight` Flying/Landed/Crashed is a label, the body always simulates;
  Landed = touching, upright, still and thrust-free for `SETTLE_TIME`) → events, exhaust, particles; fixed 120 Hz step from `main.cpp`
- `stats.hpp/.cpp` — permanent statistics: `STAT_FIELDS` table (key, menu label, format); file `$XDG_STATE_HOME/dualthrust/stats`,
  saved on landing/crash, every 30 s, on pause and at exit (not in `--frames`/`--screenshot` runs). Web build mounts the state dir as IDBFS.
- `cave.cpp` — generation stages + baked per-cell `depth` / `contour` (rendering reads these, never recomputes)
- `backend.hpp` + `backend_gles2.cpp` / `backend_sdl.cpp` — the 2D primitive layer under `Gfx`. GLES2: own batcher (one VBO, one
  shader, flush only on texture/blend change; ~7-18 GL draws per frame; entry points via `SDL_GL_GetProcAddress`, minimal
  GL declarations in the file so no GLES headers are needed). SDL: fallback for software/headless (`--renderer sdl`).
  `main.cpp` tries GLES2 first (window needs `SDL_WINDOW_OPENGL` + ES attributes), else falls back.
- `render.cpp` — zoom: `ZOOM_LEVELS` (defs.hpp) fix the world px visible vertically (480/720/1080), so scale = screen_h / visible_h,
  resolution independent. The world layer is *vector*-scaled, not pixel-scaled: chunks are rasterised at the zoom level's own
  scale (outline, hatch pitch, stars in global pixel coordinates so chunk seams are invisible), the ship/pads/particles are
  placed and sized in screen px via `sx()/sy()/Z()`, and 1 px line width never scales. The chunk cache is rebuilt when the
  level changes (stretched, filtered, while the zoom glides); camera `vw/vh` are the visible world px. `Gfx`: cached world chunk textures, CRT overlay, minimap, flames, batched text/polygons, HUD, menu
- `audio.cpp` — synthesised sound (no assets): engine rumble, landing/bounce/crash effects, generative Am-F-C-G music with echo; mixed in the SDL callback, fed from `Game::fired`
- `ui.hpp` — `MENU_ITEMS` table; `config.cpp` — XDG config; `main.cpp` — args, input, loop

Debug flags: `--play`, `--no-gamepad` (ignore a connected pad; a pad with stuck triggers fires the engines in test runs), `--rope LEN`, `--screen title|pause|options`, `--renderer auto|gles2|sdl`, `--thrust L,R` (channels 0 and 1), `--at X,Y[,DEG]` (ship at rest anywhere),
`--frames N`, `--screenshot FILE.bmp` (works headless with `SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software`).

Tests: `cmake -DDUALTHRUST_BUILD_TESTS=ON`, then `ctest`. `physics_test` is headless (settling, lift-off, crash vs soft drop, leg retract,
map edges, 120 random drops must never end inside rock); the GL backend test needs `xvfb-run -a env SDL_VIDEODRIVER=x11 ctest` (skips without GLES2).

Box2D: native builds `find_package(box2d)` (nixpkgs `box2d` 3.1.1); R36S and wasm compile the sources of `pkgs.box2d.src` via
`-DDUALTHRUST_BOX2D_SRC=` (subproject, so the cross toolchains compile it as C17). No SIMD flags needed (NEON on aarch64, scalar on wasm).

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
- Draw calls were the bottleneck on the Mali GPU: SDL 2.0.10's GLES2 renderer issues one `glDrawArrays` per
  `SDL_RenderCopy` and per rect of `SDL_RenderFillRects`. Our own GLES2 backend batches everything (see `backend_gles2.cpp`):
  on the device 7-18 GL draws/frame, 14.6 ms (60 fps) vs 27.7 ms with SDL's renderer. The static world is additionally baked into
  cached CPU-rasterised chunk textures. Check with `./dualthrust --play --frames 300` (prints per-frame sim/draw/present ms and
  the GL draw count; `--renderer sdl` compares).
- Keep to SDL ≤ 2.0.10 API (headers in the sysroot; no `SDL_RenderGeometry` — see `Gfx::gradient_triangle`).
- `mk/r36s/cxxabi_shim.cpp` shims GCC 15 → old libstdc++/glibc symbols (`-DDUALTHRUST_CXXABI_SHIM`).
- Launcher exports `XDG_CONFIG_HOME` and `XDG_STATE_HOME` = `<port>/conf`, so settings persist in `conf/dualthrust/`, not `$HOME`.
  The game never guesses: settings = `$XDG_CONFIG_HOME/dualthrust`, saves/stats = `$XDG_STATE_HOME/dualthrust`, each falling back to
  `$HOME/.config` / `$HOME/.local/state` only when the variable is unset; a relative path or no usable base → error, exit 1.
- Verified: aarch64 ELF, needs only GLIBC ≤ 2.17 / GLIBCXX ≤ 3.4.18 / CXXABI ≤ 1.3.9. Runs on a real R36S (ArkOS,
  SDL 2.0.10, KMSDRM + opengles2) at 60 fps in play with Box2D. Not yet tested through the PortMaster launcher;
  controller mapping basically works but still needs tweaks and customization.

## Versioning

- `VERSION` (plain text, top level) is the only source of truth; on the main branch it always ends in `-dev`
  (currently `0.2.0-dev`). Never hardcode the version elsewhere.
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
  WebGL renderer, keyboard (menu, Ctrl/D thrust), gamepad, Box2D, settings survive a reload. Untested: audio output.

## Packaging

CMake installs binary, `.desktop`, hicolor icons, AppStream metainfo.

## License

GPLv3-or-later.
