# TODO

## Roadmap

- **0.1.0** (released): flight, caves and pads, zoom, title screen and options, sound, GLES2 renderer, web and R36S builds.
- **0.2.0**: rigid-body rewrite with Box2D 3.1 (done: world, streamed terrain, sprung/retractable legs, slopes, statistics).
  - Done: landing possible anywhere (contact-based: upright, slow, settled); pads are flat and grippy.
  - Done: landing legs on prismatic joints with a hand-applied spring + damper, retract on Space / X; permanent statistics.
  - Done: rope (distance-joint cable with a winch, hook) and cargo crates (cyan on the minimap, delivery to pads counted).
  - Rope ideas: wrap around rock corners (a chain of bodies would, a distance joint does not), a magnet/auto-grab, crate
    weights shown on the HUD, cargo with destinations.
  - Done: pads as score/refuel spots (fuel gauge, session score).
  - Next: tune the feel on the handheld (leg Hz/damping, crash limits, `TIME_SCALE`).

## Open

- [ ] Real-hardware pass through the PortMaster launcher; controller mapping basically works but still needs
      tweaks and customization (R36S)
- [ ] Audio in the browser untested (gamepad in the browser works)
- [x] Fuel / score (tank burns with thrust, refills on pads; pad landings +100, cargo +250; HUD bar + score)
- [x] Box2D on the R36S and in the browser (builds and runs on device / in browser)
- [x] UI scale option (1X/2X/3X/4X; auto by display height; R36S → 1X, desktop → 2X)

## Notes

- Respawn: after a crash, Enter / B (and A on the gamepad) respawn immediately on the home pad.
  Intentional respawn while alive is pause-menu only (Respawn item). New Cave is pause-menu only.

- Exploration: sonar reveal strength fades past 75% range; deep-cave signals (+75);
  fuel limps instead of cutting out; map exploration awards small score.

- Map is 4:3 (7680×5760). Unexplored minimap cells are radio static, not flat black.
- Fog of war + sonar: minimap starts black; C / left-stick-click pings a ring that paints solid rock
  and cargo it sweeps. Select/Z holds the full revealed chart. Ship switch teleports to the nearest pad.

- Controls rebinding: Options → Controls lists every play action; Enter/A starts listening for a key or
  button/axis. Tab/Y toggles keyboard vs gamepad view. Binds save as `bind.K.*` / `bind.P.*` lines.
  Menu navigation stays fixed. Reset Defaults restores the historical layout. HUD hints follow the binds.
- UI scale: Options → UI SCALE cycles 1X (font 2, ~half of old desktop), 2X (font 3, previous default),
  3X (font 5), 4X (font 6). First start without a saved value picks by panel height (≤480 → 1X,
  ≥1440 → 3X, ≥2160 → 4X, else 2X). HUD, menus, minimap panel and toast all scale with the font.
