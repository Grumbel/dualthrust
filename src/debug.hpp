// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Live-tweakable gameplay parameters for the Debug menu.
// Values point into the mutable tune:: / rope:: / Game fields.

#include <cmath>

#include "defs.hpp"
#include "game.hpp"
#include "ui.hpp"

struct DebugParam {
  const char* name;
  float* value;       // nullptr → use Game float via offset path; see kind
  float min_v, max_v, step;
  enum class Kind { Tune, MassMul, ThrustMul, SonarMode, PassiveExplore } kind = Kind::Tune;
};

// Stock defaults captured once so Reset can restore them.
struct DebugDefaults {
  float time_scale = 0.62f;
  float gravity = 120.f;
  float linear_drag = 0.08f;
  float angular_drag = 1.2f;
  float max_angular_vel = 8.f;
  float land_max_vy = 55.f;
  float land_max_vx = 40.f;
  float land_max_angle = 0.22f;
  float crash_hull = 120.f;
  float crash_foot = 260.f;
  float hit_min = 30.f;
  float settle_speed = 10.f;
  float settle_angvel = 0.25f;
  float settle_max_angle = 1.2f;
  float unsettle_speed = 30.f;
  float settle_time = 0.5f;
  float fuel_burn = 0.028f;
  float fuel_refuel = 0.55f;
  float fuel_sonar = 0.012f;
  float fuel_limp = 0.22f;
  float sonar_cooldown = 0.9f;
  float sonar_max_radius = 900.f;
  float sonar_speed = 720.f;
  float sonar_fade_time = 0.9f;
  float explore_radius = 900.f;
  float explore_fade = 0.75f;
  int sonar_mode = 0;
  bool passive_explore = true;
  float leg_hertz = 1.0f;
  float leg_damping = 0.3f;
  float leg_mass_frac = 0.06f;
  float rock_friction = 0.6f;
  float pad_friction = 1.0f;
  float hull_friction = 0.4f;
  float foot_friction = 0.6f;
  float rope_out_len = 300.f;
  float rope_reel = 130.f;
  float rope_reel_loaded = 70.f;
  float grab_reach = 22.f;
  float signal_prox = 220.f;
};

inline const DebugDefaults& debug_defaults() {
  static const DebugDefaults d;
  return d;
}

// Param table. Mass/Thrust mul rows use Kind so the menu can write Game fields.
// Order is the on-screen list order.
inline constexpr int DEBUG_PARAM_COUNT = 39;

inline DebugParam* debug_params(Game& g) {
  // Static table; mass/thrust point at g fields each call via rebinding below.
  static DebugParam params[] = {
      {"TIME SCALE", &tune::TIME_SCALE, 0.2f, 1.5f, 0.02f},
      {"GRAVITY", &tune::GRAVITY, 20.f, 300.f, 5.f},
      {"LINEAR DRAG", &tune::LINEAR_DRAG, 0.f, 0.5f, 0.01f},
      {"ANGULAR DRAG", &tune::ANGULAR_DRAG, 0.f, 5.f, 0.1f},
      {"MAX SPIN", &tune::MAX_ANGULAR_VEL, 1.f, 20.f, 0.5f},
      {"MASS MUL", nullptr, 0.25f, 4.f, 0.05f, DebugParam::Kind::MassMul},
      {"THRUST MUL", nullptr, 0.25f, 4.f, 0.05f, DebugParam::Kind::ThrustMul},
      {"FUEL BURN", &tune::FUEL_BURN, 0.f, 0.15f, 0.002f},
      {"FUEL REFUEL", &tune::FUEL_REFUEL, 0.f, 2.f, 0.05f},
      {"FUEL LIMP", &tune::FUEL_LIMP, 0.f, 0.8f, 0.02f},
      {"FUEL SONAR", &tune::FUEL_SONAR, 0.f, 0.1f, 0.002f},
      {"SONAR RANGE", &tune::SONAR_MAX_RADIUS, 100.f, 2500.f, 50.f},
      {"SONAR SPEED", &tune::SONAR_SPEED, 100.f, 2000.f, 40.f},
      {"SONAR COOL", &tune::SONAR_COOLDOWN, 0.f, 5.f, 0.1f},
      {"SONAR FADE", &tune::SONAR_FADE_TIME, 0.15f, 3.f, 0.05f},
      {"EXPLORE R", &tune::EXPLORE_RADIUS, 80.f, 2000.f, 20.f},
      {"EXPLORE FADE", &tune::EXPLORE_FADE, 0.3f, 0.95f, 0.05f},
      {"PASSIVE MAP", nullptr, 0.f, 1.f, 1.f, DebugParam::Kind::PassiveExplore},
      {"SONAR MODE", nullptr, 0.f, 2.f, 1.f, DebugParam::Kind::SonarMode},
      {"LEG HERTZ", &tune::LEG_HERTZ, 0.2f, 4.f, 0.1f},
      {"LEG DAMP", &tune::LEG_DAMPING, 0.05f, 2.f, 0.05f},
      {"LEG MASS %", &tune::LEG_MASS_FRACTION, 0.01f, 0.2f, 0.01f},
      {"CRASH HULL", &tune::CRASH_HULL_SPEED, 40.f, 400.f, 10.f},
      {"CRASH FOOT", &tune::CRASH_FOOT_SPEED, 80.f, 600.f, 10.f},
      {"HIT MIN", &tune::HIT_MIN_SPEED, 5.f, 100.f, 5.f},
      {"LAND MAX VY", &tune::LAND_MAX_VY, 10.f, 150.f, 5.f},
      {"LAND MAX VX", &tune::LAND_MAX_VX, 10.f, 120.f, 5.f},
      {"LAND MAX ANG", &tune::LAND_MAX_ANGLE, 0.05f, 1.f, 0.02f},
      {"SETTLE SPD", &tune::SETTLE_SPEED, 1.f, 40.f, 1.f},
      {"SETTLE TIME", &tune::SETTLE_TIME, 0.1f, 2.f, 0.05f},
      {"ROCK FRIC", &tune::ROCK_FRICTION, 0.f, 2.f, 0.05f},
      {"PAD FRIC", &tune::PAD_FRICTION, 0.f, 2.f, 0.05f},
      {"HULL FRIC", &tune::HULL_FRICTION, 0.f, 2.f, 0.05f},
      {"FOOT FRIC", &tune::FOOT_FRICTION, 0.f, 2.f, 0.05f},
      {"ROPE LEN", &rope::OUT_LEN, 40.f, 800.f, 20.f},
      {"REEL SPEED", &rope::REEL_SPEED, 20.f, 400.f, 10.f},
      {"REEL LOADED", &rope::REEL_SPEED_LOADED, 10.f, 300.f, 10.f},
      {"GRAB REACH", &rope::GRAB_REACH, 5.f, 80.f, 2.f},
      {"SIGNAL PROX", &tune::SIGNAL_PROX, 40.f, 600.f, 20.f},
  };
  static_assert(sizeof(params) / sizeof(params[0]) == DEBUG_PARAM_COUNT, "DEBUG_PARAM_COUNT");
  (void)g;
  return params;
}

inline float debug_param_value(Game& g, const DebugParam& p) {
  switch (p.kind) {
    case DebugParam::Kind::MassMul: return g.dbg_mass_mul;
    case DebugParam::Kind::ThrustMul: return g.dbg_thrust_mul;
    case DebugParam::Kind::SonarMode: return static_cast<float>(tune::SONAR_MODE);
    case DebugParam::Kind::PassiveExplore: return tune::PASSIVE_EXPLORE ? 1.f : 0.f;
    default: return p.value ? *p.value : 0.f;
  }
}

inline void debug_param_set(Game& g, const DebugParam& p, float v) {
  v = clampf(v, p.min_v, p.max_v);
  switch (p.kind) {
    case DebugParam::Kind::MassMul:
      g.dbg_mass_mul = v;
      if (g.ship != NULL_ENTITY && g.ecs.has<Body>(g.ship) && g.ecs.has<Hull>(g.ship))
        g.phys.apply_ship_mass(g.ecs.get<Body>(g.ship).b, *g.ecs.get<Hull>(g.ship).def, g.dbg_mass_mul);
      break;
    case DebugParam::Kind::ThrustMul:
      g.dbg_thrust_mul = v;
      break;
    case DebugParam::Kind::SonarMode:
      tune::SONAR_MODE = static_cast<int>(std::lround(v)) % tune::SONAR_MODE_COUNT;
      if (tune::SONAR_MODE < 0) tune::SONAR_MODE += tune::SONAR_MODE_COUNT;
      break;
    case DebugParam::Kind::PassiveExplore:
      tune::PASSIVE_EXPLORE = v >= 0.5f;
      break;
    default:
      if (p.value) *p.value = v;
      if (p.value == &tune::GRAVITY) g.phys.sync_gravity();
      break;
  }
}

inline void debug_param_nudge(Game& g, int index, int dir) {
  if (index < 0 || index >= DEBUG_PARAM_COUNT) return;
  DebugParam* params = debug_params(g);
  const DebugParam& p = params[index];
  float v = debug_param_value(g, p) + dir * p.step;
  // quantize to step
  if (p.step > 0.f) v = std::round(v / p.step) * p.step;
  debug_param_set(g, p, v);
}

// Defaults in the same order as debug_params() rows (must stay in sync).
inline float debug_param_default(int index) {
  static const float defs[DEBUG_PARAM_COUNT] = {
      0.62f, 120.f, 0.08f, 1.2f, 8.f,           // time, gravity, drags, spin
      1.f, 1.f,                                   // mass/thrust mul
      0.028f, 0.55f, 0.22f, 0.012f,              // fuel
      900.f, 720.f, 0.9f, 0.9f,                  // sonar
      900.f, 0.75f, 1.f, 0.f,                    // explore + passive + mode
      1.0f, 0.3f, 0.06f,                         // legs
      120.f, 260.f, 30.f,                        // crash / hit
      55.f, 40.f, 0.22f,                         // land
      10.f, 0.5f,                                // settle
      0.6f, 1.0f, 0.4f, 0.6f,                    // friction
      300.f, 130.f, 70.f, 22.f,                  // rope
      220.f,                                     // signal prox
  };
  if (index < 0 || index >= DEBUG_PARAM_COUNT) return 0.f;
  return defs[index];
}

inline bool debug_param_changed(Game& g, int index) {
  if (index < 0 || index >= DEBUG_PARAM_COUNT) return false;
  const DebugParam& p = debug_params(g)[index];
  const float cur = debug_param_value(g, p);
  const float def = debug_param_default(index);
  const float eps = std::max(1e-4f, p.step * 0.01f);
  return std::fabs(cur - def) > eps;
}

inline void debug_param_reset_one(Game& g, int index) {
  if (index < 0 || index >= DEBUG_PARAM_COUNT) return;
  debug_param_set(g, debug_params(g)[index], debug_param_default(index));
}

inline void debug_reset_all(Game& g) {
  const DebugDefaults& d = debug_defaults();
  tune::TIME_SCALE = d.time_scale;
  tune::GRAVITY = d.gravity;
  tune::LINEAR_DRAG = d.linear_drag;
  tune::ANGULAR_DRAG = d.angular_drag;
  tune::MAX_ANGULAR_VEL = d.max_angular_vel;
  tune::LAND_MAX_VY = d.land_max_vy;
  tune::LAND_MAX_VX = d.land_max_vx;
  tune::LAND_MAX_ANGLE = d.land_max_angle;
  tune::CRASH_HULL_SPEED = d.crash_hull;
  tune::CRASH_FOOT_SPEED = d.crash_foot;
  tune::HIT_MIN_SPEED = d.hit_min;
  tune::SETTLE_SPEED = d.settle_speed;
  tune::SETTLE_ANGVEL = d.settle_angvel;
  tune::SETTLE_MAX_ANGLE = d.settle_max_angle;
  tune::UNSETTLE_SPEED = d.unsettle_speed;
  tune::SETTLE_TIME = d.settle_time;
  tune::FUEL_BURN = d.fuel_burn;
  tune::FUEL_REFUEL = d.fuel_refuel;
  tune::FUEL_SONAR = d.fuel_sonar;
  tune::FUEL_LIMP = d.fuel_limp;
  tune::SONAR_COOLDOWN = d.sonar_cooldown;
  tune::SONAR_MAX_RADIUS = d.sonar_max_radius;
  tune::SONAR_SPEED = d.sonar_speed;
  tune::SONAR_FADE_TIME = d.sonar_fade_time;
  tune::EXPLORE_RADIUS = d.explore_radius;
  tune::EXPLORE_FADE = d.explore_fade;
  tune::SONAR_MODE = d.sonar_mode;
  tune::PASSIVE_EXPLORE = d.passive_explore;
  tune::LEG_HERTZ = d.leg_hertz;
  tune::LEG_DAMPING = d.leg_damping;
  tune::LEG_MASS_FRACTION = d.leg_mass_frac;
  tune::ROCK_FRICTION = d.rock_friction;
  tune::PAD_FRICTION = d.pad_friction;
  tune::HULL_FRICTION = d.hull_friction;
  tune::FOOT_FRICTION = d.foot_friction;
  rope::OUT_LEN = d.rope_out_len;
  rope::REEL_SPEED = d.rope_reel;
  rope::REEL_SPEED_LOADED = d.rope_reel_loaded;
  rope::GRAB_REACH = d.grab_reach;
  tune::SIGNAL_PROX = d.signal_prox;
  g.dbg_mass_mul = 1.f;
  g.dbg_thrust_mul = 1.f;
  g.phys.sync_gravity();
  if (g.ship != NULL_ENTITY && g.ecs.has<Body>(g.ship) && g.ecs.has<Hull>(g.ship))
    g.phys.apply_ship_mass(g.ecs.get<Body>(g.ship).b, *g.ecs.get<Hull>(g.ship).def, 1.f);
}
