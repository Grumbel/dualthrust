<!--
SPDX-License-Identifier: GPL-3.0-or-later
-->
# dualthrust

A little 2D spaceship game mildly inspired by *Space Taxi*. You pilot a craft with **two engines** (left and right). The **only** controls are the gamepad **triggers**: left trigger → left engine, right trigger → right engine. Differential thrust gives you both translation and rotation.

## Requirements

- Gamepad with analog triggers (Xbox, DualShock/DualSense via SDL2 mappings, etc.)
- For testing without a pad: hold `A` / `←` for left engine, `D` / `→` for right engine
- **Select / Back** (or keyboard `Tab` / `[` / `]`) cycles ship presets (Narrow, Medium, Wide, Barge, Long)
- **Start** (or keyboard `X`) swaps left/right engine mapping

## Build with Nix

```sh
nix develop
dualthrust-configure
dualthrust-build
dualthrust-run
```

Or simply:

```sh
nix run
```

## Physics notes

- Gravity pulls downward.
- Each engine applies force toward the ship’s nose and a torque from its offset.
- Light linear and angular drag keep the ship manageable.
- Soft screen bounds bounce the ship gently.

## License

GPL-3.0-or-later. See `LICENSES/` and SPDX headers.
