# TODO

## Roadmap

- **0.1.0** (released): flight, caves and pads, zoom, title screen and options, sound, GLES2 renderer, web and R36S builds.
- **0.2.0**: rigid-body rewrite with Box2D 3.1 (done: world, streamed terrain, sprung/retractable legs, slopes, statistics).
  - Done: landing possible anywhere (contact-based: upright, slow, settled); pads are flat and grippy.
  - Done: landing legs on prismatic joints with a hand-applied spring + damper, retract on Space / X; permanent statistics.
  - Done: rope (distance-joint cable with a winch, hook) and cargo crates (cyan on the minimap, delivery to pads counted).
  - Rope ideas: wrap around rock corners (a chain of bodies would, a distance joint does not), a magnet/auto-grab, crate
    weights shown on the HUD, cargo with destinations.
  - Next: tune the feel on the handheld (leg Hz/damping, crash limits, `TIME_SCALE`), pads as score/refuel spots.

## Open

- [ ] Real-hardware pass through the PortMaster launcher and the controller mapping (R36S)
- [ ] Audio in the browser and gamepads in the browser are untested
- [ ] Fuel / score (no win conditions or money for now; the statistics are the only record)
- [ ] Box2D on the R36S and in the browser: builds, but not run on the device / in a browser yet
- [x] UI scale option (1X/2X/3X/4X; auto by display height; R36S → 1X, desktop → 2X)

## Notes

- UI scale: Options → UI SCALE cycles 1X (font 2, ~half of old desktop), 2X (font 3, previous default),
  3X (font 5), 4X (font 6). First start without a saved value picks by panel height (≤480 → 1X,
  ≥1440 → 3X, ≥2160 → 4X, else 2X). HUD, menus, minimap panel and toast all scale with the font.
