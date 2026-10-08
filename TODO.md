# TODO

## Roadmap

- **0.1.0** (released): flight, caves and pads, zoom, title screen and options, sound, GLES2 renderer, web and R36S builds.
- **0.2.0**: rigid-body rewrite with Box2D (v3, C, MIT; nixpkgs has 3.1.x).
  - Landing possible anywhere (contact-based: feet down, slow, settled), pads become scoring/refuel spots.
  - Springy landing legs: prismatic joints with spring + damper, drawn as hydraulic cylinders.
  - Rope and cargo: chain of small bodies on revolute joints (or a distance joint), attach/release key.
  - Terrain as static chain shapes streamed around the ship, shifted across the X wrap.
  - First step: Box2D in the Nix, Emscripten and R36S (ArkOS sysroot, old glibc) builds; retune the feel afterwards.

## Open

- [ ] Real-hardware pass through the PortMaster launcher and the controller mapping (R36S)
- [ ] Audio in the browser and gamepads in the browser are untested
- [ ] Fuel / score
- [ ] UI scale for large displays (HUD and menus use fixed pixel sizes)
