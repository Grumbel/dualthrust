# TODO

## Tip

- Bundle line: `dualthrust-043.1-x11-exit-shutdown-9a093cd` → tip (HEAD)
- Base of this work line: `9a093cd`

## Roadmap

- **0.1.0** (released): flight, caves and pads, zoom, title screen and options, sound, GLES2 renderer, web and R36S builds.
- **0.2.0**: rigid-body rewrite with Box2D 3.1 (done: world, streamed terrain, sprung/retractable legs, slopes, statistics).
  - Done: landing anywhere; pads flat and grippy; legs; rope + cargo; fuel/score.
  - Rope ideas (open): wrap around rock corners.
  - Done: magnet/auto-grab; crate name+mass on HUD; dest pad highlighted in-world while hauling
    (ghost + DEST? label if the pad is not yet activated by sonar).
  - Done: per-engine damage (thrust loss, sputter, HUD); damage-aware exhaust particles and engine audio;
    always-on random thrust flutter (stronger when damaged).
  - Next: tune the feel on the handheld (leg Hz/damping, crash limits, `TIME_SCALE`).

## Open

- [x] Persist world state under XDG state dir (restore on next launch)

- [ ] Real-hardware pass through the PortMaster launcher; controller mapping basically works but still needs
      tweaks and customization (R36S)
- [ ] Audio in the browser untested (gamepad in the browser works)
- [x] Fuel / score
- [x] Box2D on the R36S and in the browser
- [x] UI scale option
- [x] Debug menu (pause → Debug, or F3 in play): live tune + Y resets one row; changed rows highlight
- [x] Data-driven sonar modes (REFLECT / PAINT / BOTH) + passive explore
- [x] Magnet auto-grab + HUD MAGNET cue + Debug AUTO GRAB
- [x] Bidirectional stick thrust (channels 4/5 = stick-down) + Vernier / Bidraft / Seesaw ships
- [x] Zoom cycle action (Tab); zoom in/out unbound on pad by default
- [x] Menu nav stack (Options → Controls → Back → Options → Back works)
- [x] Right-stick click scrubbed from Respawn / NextShip on load (legacy configs)
- [x] Per-engine damage → force, exhaust particles, flames, and engine audio
- [x] Random thrust fluctuations (always-on flutter + damage misfires)

## Notes

### Engine damage
- Each thruster tracks `damage` 0..1. Hull impacts wound the nearest nozzle(s); foot hits are gentler.
- `forces_system` writes `Thrusters::output[]` (channel level × power × fuel/hurt × health × flutter).
  Exhaust, flames and audio all read that so physics, particles and sound stay in lockstep.
- Above `ENGINE_SPUTTER` (~0.18): irregular cough + random hard misfires. Above `ENGINE_DEAD` (~0.92): no force.
- Always-on `ENGINE_FLUTTER` (~±4%) multi-sine + rng so healthy thrust is never perfectly flat.
- Damaged exhaust: soot/smoke palette, larger spread, occasional sparks. Damaged audio: brighter noise,
  amplitude flutter, crackle bursts. Pads repair engines over time (`ENGINE_REPAIR`).

### Ships
- **Rocket**: tall 1950s sci-fi needle (`half_h` 96). Options → Ship or **S**.
- **Vernier**: mains on triggers; **LS up/down** opposing lateral thrusters.
- **Bidraft**: mains + LS lateral + **RS up/down** nose/belly.
- **Seesaw**: each stick is an opposing pair (no triggers required).
- **Lurch**: deliberately unbalanced engines (fat left, weak canted right); equal primary input yaws hard.
  Trigger verniers for recovery. Built for differential-thrust practice.
- **Titan**: super-heavy needle rocket; twin primary mains (sticks) + small top L/R RCS (triggers).
- Controls: sticks = primary L/R, triggers = secondary
- Winch/hook only on haulers (`ShipDef.winch`): Medium, Frigate, Atlas, Dragonfly, Colossus, Vernier, Bidraft.
  Rockets / racers (Narrow, Rocket, Titan, Lurch, Seesaw) have no cable. (classic 2-engine ships fold triggers into primary at 85%).

### Hangar / pads
- Landed on a pad → **H** / gamepad **Y** opens Hangar: cycle ships, teleport to **visited** pads.
- Pad **visited** = ship has settled there (home pad starts visited). Sonar only marks **active**.
- World: filled diamond = visited, hollow ring = found-but-not-landed. Minimap/full map same colours
  (bright vs amber). Teleport (`[/]` / shoulders) only between visited pads.
- Holding **Map** (Z / Select) freezes the simulation until released.

### Debug
- Pause menu → **Debug** (not under Options). F3 opens it from play (via Pause).
- Rows that differ from stock defaults draw in HOT orange.
- **Y** resets the selected row; footer **RESET DEFAULTS** restores all.
- Mass mul rebuilds Box2D mass immediately; gravity syncs the world. Drag/friction on
  existing bodies need a respawn to fully re-apply.

### Respawn
- After a crash: Enter / B (and A on the pad) respawn on the home pad.
- While alive: pause-menu **Respawn** only. New Cave is pause-menu only.
- Right-stick click is not bound to Respawn or NextShip (stripped on load).

### Zoom
- **Tab** = cycle Near → Medium → Far. **=** / **-** and mouse wheel = in/out.
- No default gamepad zoom binds (rebind in Controls).

### Fog / sonar
- Passive circular uncover (`EXPLORE_RADIUS` ~900 ≈ FAR zoom size, LOS-limited).
- Sonar modes (`tune::SONAR_MODES`): **REFLECT** (default), **PAINT**, **BOTH**.
  Systems dispatch on flags; classic paint path is kept. Debug: SONAR MODE / PASSIVE MAP.
- Reflect hits draw as mirrored circular arc segments; ring expands while fading (never freezes).

### Controls
- Options → Controls: rebind every play action. Tab/Y toggles keyboard vs pad view.
- Channels: 0/1 triggers, 2/3 stick-up, 4/5 stick-down (LSd/RSd; keyboard F/G by default).
- Menu navigation is fixed (not rebindable).

### UI scale
- 1X–4X; first start picks by panel height (≤480 → 1X, else 2X, etc.).

### Platforms
- Box2D works on R36S and in browser. Gamepad works in browser.
