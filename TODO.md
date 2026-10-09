# TODO

## Tip

- Bundle line: `dualthrust-018.1-scrub-rstick-respawn-9a093cd` → tip `062ca07`
- Base of this work line: `9a093cd`

## Roadmap

- **0.1.0** (released): flight, caves and pads, zoom, title screen and options, sound, GLES2 renderer, web and R36S builds.
- **0.2.0**: rigid-body rewrite with Box2D 3.1 (done: world, streamed terrain, sprung/retractable legs, slopes, statistics).
  - Done: landing anywhere; pads flat and grippy; legs; rope + cargo; fuel/score.
  - Rope ideas (open): wrap around rock corners, magnet/auto-grab, crate weights on HUD, cargo destinations.
  - Next: tune the feel on the handheld (leg Hz/damping, crash limits, `TIME_SCALE`).

## Open

- [ ] Real-hardware pass through the PortMaster launcher; controller mapping basically works but still needs
      tweaks and customization (R36S)
- [ ] Audio in the browser untested (gamepad in the browser works)
- [x] Fuel / score
- [x] Box2D on the R36S and in the browser
- [x] UI scale option
- [x] Debug menu (title → Debug, or F3 in play): live tune + Y resets one row; changed rows highlight
- [x] Data-driven sonar modes (REFLECT / PAINT / BOTH) + passive explore
- [x] Bidirectional stick thrust (channels 4/5 = stick-down) + Vernier / Bidraft / Seesaw ships
- [x] Zoom cycle action (Tab); zoom in/out unbound on pad by default
- [x] Menu nav stack (Options → Controls → Back → Options → Back works)
- [x] Right-stick click scrubbed from Respawn / NextShip on load (legacy configs)

## Notes

### Ships
- **Rocket**: tall 1950s sci-fi needle (`half_h` 96). Options → Ship or **S**.
- **Vernier**: mains on triggers; **LS up/down** opposing lateral thrusters.
- **Bidraft**: mains + LS lateral + **RS up/down** nose/belly.
- **Seesaw**: each stick is an opposing pair (no triggers required).

### Debug
- Title menu → **Debug** (not under Options). F3 opens it from play.
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
