// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include "game.hpp"

// Entity setup / commands
void create_ship(Game& g, int def_index);
void set_ship_def(Game& g, int def_index);
int ship_def_index(const Game& g);
void respawn_ship(Game& g, float wx);  // onto the pad nearest wx, clears particles
void place_ship(Game& g, Vec2 pos, float angle);  // debugging: at rest anywhere (px, radians)
void snap_camera(Game& g);
// Zoom: set the level (clamped); update_view animates the camera's viewport toward it and keeps it at the
// screen's aspect ratio. snap = jump without animating.
void set_zoom(Game& g, int index);
void update_view(Game& g, float real_dt, float aspect, bool snap = false);
void set_thrust(Game& g, float left, float right);  // channels 0 and 1
void set_thrusts(Game& g, const float levels[4]);   // all four control channels
int ship_channels(const Game& g);                   // 2 for the classic ships, 4 for the big ones
void toggle_legs(Game& g);
void debug_rope(Game& g, float len);  // debugging: cable paid out to len px, hook hanging
void set_winch(Game& g, bool out);  // deploy the cable fully (true) or retract it fully (false)
void toggle_grip(Game& g);         // hook takes the crate in reach, or lets go of it  // retract / extend the landing legs
inline Transform& ship_transform(Game& g) { return g.ecs.get<Transform>(g.ship); }

// Fog of war / sonar: the minimap stays black until a ping paints solid cells and cargo.
void reset_fog(Game& g);                 // clear revealed, stop any ping (call after cave.generate)
void fire_sonar(Game& g);                // start a ring from the ship; ignored if one is already running
void update_sonar(Game& g, float dt);    // expand the ring and paint cells (sim seconds)

// Per-tick systems (dt = simulated seconds)
void step_sim(Game& g, float dt);
void update_camera(Game& g, float real_dt);
