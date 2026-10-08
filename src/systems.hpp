// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include "game.hpp"

// Entity setup / commands
void create_ship(Game& g, int def_index);
void set_ship_def(Game& g, int def_index);
int ship_def_index(const Game& g);
void respawn_ship(Game& g, float wx);  // onto the pad nearest wx, clears particles
void snap_camera(Game& g);
// Zoom: set the level (clamped); update_view animates the camera's viewport toward it and keeps it at the
// screen's aspect ratio. snap = jump without animating.
void set_zoom(Game& g, int index);
void update_view(Game& g, float real_dt, float aspect, bool snap = false);
void set_thrust(Game& g, float left, float right);
inline Transform& ship_transform(Game& g) { return g.ecs.get<Transform>(g.ship); }

// Per-tick systems (dt = simulated seconds)
void step_sim(Game& g, float dt);
void update_camera(Game& g, float real_dt);
