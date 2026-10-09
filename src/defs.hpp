// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Static game data: tuning constants, palette and ship presets.

#include "math.hpp"

// Full version string from the top-level VERSION file (CMake: DUALTHRUST_VERSION)
#ifndef DUALTHRUST_VERSION
#define DUALTHRUST_VERSION "unknown"
#endif
inline constexpr const char* APP_VERSION = DUALTHRUST_VERSION;

namespace tune {
inline constexpr float SIM_STEP = 1.f / 120.f;  // fixed real-time step
inline float TIME_SCALE = 0.62f;      // sim seconds per real second
inline constexpr int MAX_STEPS_PER_FRAME = 8;

inline float GRAVITY = 120.f;
inline float LINEAR_DRAG = 0.08f;
inline float ANGULAR_DRAG = 1.2f;
inline float MAX_ANGULAR_VEL = 8.f;

// HUD guides for a gentle touchdown (the physics decides what actually happens)
inline float LAND_MAX_VY = 55.f;
inline float LAND_MAX_VX = 40.f;
inline float LAND_MAX_ANGLE = 0.22f;

// Impacts (normal speed, px/s): hull or engine bells above CRASH_HULL_SPEED, feet above CRASH_FOOT_SPEED destroy
// the ship; anything above HIT_MIN_SPEED makes a bounce effect.
inline float CRASH_HULL_SPEED = 120.f;
inline float CRASH_FOOT_SPEED = 260.f;
inline float HIT_MIN_SPEED = 30.f;

// Landed = touching rock, almost still and without thrust for SETTLE_TIME (sim seconds). Hysteresis: a ship
// that slides faster than UNSETTLE_SPEED (e.g. down a slope) counts as flying again.
inline float SETTLE_SPEED = 10.f;
inline float SETTLE_ANGVEL = 0.25f;
inline float SETTLE_MAX_ANGLE = 1.2f;  // rad: leaning more than this is toppled, not landed
inline float UNSETTLE_SPEED = 30.f;
inline float SETTLE_TIME = 0.5f;

// Fuel (0..1): light drain, generous refill. Engines limp below ~20% instead of cutting out.
inline float FUEL_BURN = 0.028f;      // fraction per second at summed channel level 1.0
inline float FUEL_REFUEL = 0.55f;     // fraction per second on a pad
inline float FUEL_SONAR = 0.012f;     // small ping cost (never blocks scanning)
inline float FUEL_LIMP = 0.22f;       // below this, thrust power floors here (sputter, not stop)
inline float SONAR_COOLDOWN = 0.9f;   // seconds between pings
inline float SONAR_MAX_RADIUS = 900.f; // world px — painting stops here
inline float SONAR_SPEED = 720.f;      // world px per sim second
inline float SONAR_FADE_TIME = 0.9f;   // seconds of fade-out after the wave reaches max range
inline float EXPLORE_RADIUS = 260.f;   // passive minimap uncover around the ship (world px)
inline float EXPLORE_FADE = 0.75f;     // full strength out to this fraction of EXPLORE_RADIUS
inline bool PASSIVE_EXPLORE = true;    // ship proximity paints the fog map each tick

// Sonar behaviour is data-driven: pick a row, systems dispatch on the flags.
// REFLECT = search pulse (reflections on pads/cargo/signals); PAINT = classic fog ping;
// BOTH keeps both paths for experiments.
struct SonarModeDef {
  const char* name;
  bool paint_fog;           // expanding annulus paints revealed[]
  bool reflect_targets;     // spawn reflections on pad / cargo / signal
  bool tag_blob_on_hit;     // small permanent chart blob on a reflect hit
  bool activate_pad_on_hit; // pad goes online when the wavefront hits it
};
inline constexpr SonarModeDef SONAR_MODES[] = {
    {"REFLECT", false, true, true, true},
    {"PAINT", true, false, false, false},
    {"BOTH", true, true, true, true},
};
inline constexpr int SONAR_MODE_COUNT = static_cast<int>(sizeof(SONAR_MODES) / sizeof(SONAR_MODES[0]));
inline int SONAR_MODE = 0;  // index into SONAR_MODES (mutable; Debug menu)
inline const SonarModeDef& sonar_mode() {
  return SONAR_MODES[SONAR_MODE < 0 || SONAR_MODE >= SONAR_MODE_COUNT ? 0 : SONAR_MODE];
}

// Score (session): exploration-friendly rewards, not win conditions
inline constexpr int SCORE_PAD_LANDING = 100;
inline constexpr int SCORE_CARGO = 250;
inline constexpr int SCORE_SIGNAL = 75;         // first sonar contact with a deep-cave signal
inline constexpr int SCORE_REVEAL_CELL = 1;     // per newly solid-revealed cell (capped per ping)
inline constexpr int SCORE_DEST_BONUS = 150;    // delivering cargo to its preferred pad
inline constexpr int SCORE_ECHO = 15;           // ambient life answering a ping
inline constexpr int SCORE_MILESTONE = 50;      // exploration tier / all-signals / all-pads
inline float SIGNAL_PROX = 220.f;     // world px: passive cue near an unfound signal
inline float HURT_FROM_HIT = 0.12f;   // soft damage added on a hard bounce
inline float HURT_REPAIR = 0.35f;     // repair rate per second while on an active pad
inline constexpr float RESIDUE_TTL = 1.4f;      // seconds a scan glow lingers on rock

// Landing legs: spring along the strut (Hz, damping ratio), leg body mass as a fraction of the ship's
inline float LEG_HERTZ = 1.0f;
inline float LEG_DAMPING = 0.3f;
inline float LEG_MASS_FRACTION = 0.06f;

// Friction coefficients (mixed as sqrt(a * b) by Box2D)
inline float ROCK_FRICTION = 0.6f;
inline float PAD_FRICTION = 1.0f;
inline float HULL_FRICTION = 0.4f;
inline float FOOT_FRICTION = 0.6f;

inline constexpr size_t MAX_PARTICLES = 1500;
inline float EXHAUST_RATE = 150.f;  // particles / sec / engine at full thrust
}  // namespace tune

namespace pal {
inline constexpr Rgba BG{0, 10, 4, 255};
inline constexpr Rgba DIM{16, 64, 32, 255};
inline constexpr Rgba MID{40, 180, 80, 255};
inline constexpr Rgba BRIGHT{140, 255, 160, 255};
inline constexpr Rgba WARN{220, 200, 60, 255};
inline constexpr Rgba HOT{255, 120, 40, 255};
inline constexpr Rgba PAD{80, 220, 140, 255};
inline constexpr Rgba STAR{120, 220, 140, 255};
inline constexpr Rgba MENU{20, 40, 24, 230};
inline constexpr Rgba HULL_FILL{4, 26, 12, 255};
inline constexpr Rgba FLAME_CORE{255, 245, 210, 255};
inline constexpr Rgba FLAME_EDGE{255, 90, 20, 255};
inline constexpr Rgba CARGO{90, 205, 255, 255};   // crates, their minimap dots (cyan stands out from the green and amber)
inline constexpr Rgba ROPE{150, 215, 170, 255};
}  // namespace pal

// Cosmetic / structural extras of a ship (drawn; tanks also collide)
enum ShipStyle : unsigned {
  STYLE_FINS = 1,     // tail fins at the belly
  STYLE_TANKS = 2,    // fuel tanks beside the cabin
  STYLE_DOME = 4,     // glass canopy
  STYLE_DISH = 8,     // radar dish on a stalk
  STYLE_STRIPES = 16, // hazard stripes along the belly
  STYLE_DECK = 32     // a wide flat deck across the belly (collides): big ships with outboard thrusters
};

// One thruster of a ship with its own control channel. Ships without a list have the classic pair (see
// thruster_pose): left and right engines on the channels 0 and 1.
//   channel 0 = left trigger / left shoulder, 1 = right trigger / right shoulder, 2 = left stick up, 3 = right stick up
//   angle: direction of the push in radians off the nose (0 = toward the nose, +90° = to the right, 180° = down)
struct ThrusterDef {
  float x, y;     // hull-local mount
  float angle;
  float power;    // fraction of the ship's max_thrust
  int channel;    // 0..3
};

struct ShipDef {
  const char* name;
  float half_w, half_h;
  float engine_offset_x, engine_offset_y;  // offset magnitudes
  float mass, inertia, max_thrust;
  bool engines_top;  // true: engines on nose/top (-y); false: aft/bottom (+y)
  unsigned style = 0;
  const ThrusterDef* thrusters = nullptr;  // null: the classic left/right pair
  int thruster_n = 0;

  // Signed local Y of the engine mounts (local +y = ground side when upright)
  float eng_y() const { return engines_top ? -engine_offset_y : engine_offset_y; }
  // Local Y of the nozzle rim, where exhaust leaves
  float nozzle_y() const { return eng_y() + (engines_top ? 16.f : 14.f); }
  float foot_y() const { return half_h * 0.95f; }
};

// A thruster as the simulation and the renderer need it, hull-local
struct ThrusterPose {
  Vec2 pos;         // mount
  Vec2 push;        // unit direction of the force on the ship
  Vec2 flame;       // unit direction the exhaust leaves in
  Vec2 nozzle;      // where the exhaust starts
  float power;
  int channel;
};
inline int thruster_count(const ShipDef& d) { return d.thruster_n ? d.thruster_n : 2; }
inline int channel_count(const ShipDef& d) { return d.thruster_n ? 4 : 2; }  // controls the ship uses
inline ThrusterPose thruster_pose(const ShipDef& d, int i) {
  ThrusterPose p;
  if (d.thruster_n) {
    const ThrusterDef& t = d.thrusters[i];
    p.pos = {t.x, t.y};
    p.push = {std::sin(t.angle), -std::cos(t.angle)};
    p.flame = {-p.push.x, -p.push.y};
    p.nozzle = p.pos + p.flame * 12.f;
    p.power = t.power;
    p.channel = t.channel;
  } else {  // classic pair; top-mounted engines still fire "down" visually
    p.pos = {i == 0 ? -d.engine_offset_x : d.engine_offset_x, d.eng_y()};
    p.push = {0.f, -1.f};
    p.flame = {0.f, 1.f};
    p.nozzle = {p.pos.x, d.nozzle_y()};
    p.power = 1.f;
    p.channel = i;
  }
  return p;
}

// Hull outline, in ship-local px (+y = ground side when upright). Shared by drawing and physics.
struct HullGeom {
  float hw, hh;                      // half extents of the drawn body
  float nose_y, cabin_y, belly_y;
  float cabin_hw() const { return hw * 0.45f; }
  float belly_hw() const { return hw * 0.55f; }
};
inline HullGeom hull_geom(const ShipDef& d) {
  HullGeom g;
  g.hw = d.half_w * 0.85f;
  g.hh = d.half_h * 0.9f;
  g.nose_y = -g.hh;
  g.cabin_y = -g.hh * 0.35f;
  g.belly_y = d.engines_top ? g.hh * 0.35f : std::min(std::abs(d.eng_y()) - 6.f, g.hh * 0.45f);
  if (g.belly_y < g.cabin_y + 8.f) g.belly_y = g.cabin_y + 12.f;
  return g;
}

// Where the winch cable leaves the hull (hull-local y; x = 0)
inline float winch_y(const ShipDef& d) { return hull_geom(d).belly_y + 6.f; }

// Landing legs: a strut from the hull (attach) out to the foot, which slides along `axis` (unit, out of the hull).
// Legs stand outside the engines and reach past the nozzle rims so the bells never touch the ground first.
struct LegGeom {
  float attach_x, attach_y;  // hull-local, for the right leg (mirror x for the left)
  float foot_x, foot_y;      // foot centre when fully extended
  float length;              // attach -> foot
  float axis_x, axis_y;      // unit vector attach -> foot
  float travel;              // how far the foot can be pushed in (retracted foot sits at the attach point)
  float foot_half_w;         // landing plate
};
inline LegGeom leg_geom(const ShipDef& d) {
  const HullGeom h = hull_geom(d);
  LegGeom g;
  g.attach_x = h.belly_hw();
  g.attach_y = h.belly_y;
  g.foot_x = d.engines_top ? h.hw * 0.9f : std::max(h.hw * 0.9f, d.engine_offset_x + 12.f);
  g.foot_y = d.engines_top ? h.hh * 0.85f + 6.f : std::max(h.hh * 0.85f, d.nozzle_y() + 8.f);
  const float dx = g.foot_x - g.attach_x, dy = g.foot_y - g.attach_y;
  g.length = std::sqrt(dx * dx + dy * dy);
  g.axis_x = dx / g.length;
  g.axis_y = dy / g.length;
  g.travel = g.length - 4.f;
  g.foot_half_w = 7.f;
  return g;
}

// Centre of mass, hull-local y: well below the hull centre (engines and gear are heavy), so the ship stands
// stably on its legs, even on a slope. Thrust acts along the ship's axis, so this does not change how it flies.
inline float com_y(const ShipDef& d) { return 0.45f * leg_geom(d).foot_y; }

// Big ships with four thrusters, two on the triggers and two on the sticks
inline constexpr ThrusterDef FRIGATE_T[] = {
    {-62.f, 34.f, 0.f, 1.f, 0},   {62.f, 34.f, 0.f, 1.f, 1},                    // main engines: triggers
    {-70.f, 26.f, 1.5708f, 0.5f, 2}, {70.f, 26.f, -1.5708f, 0.5f, 3},          // side thrusters: sticks strafe
};
inline constexpr ThrusterDef ATLAS_T[] = {
    {-56.f, 40.f, 0.f, 1.f, 0},   {56.f, 40.f, 0.f, 1.f, 1},                    // main engines: triggers
    {-44.f, -2.f, 3.1416f, 0.6f, 2}, {44.f, -2.f, 3.1416f, 0.6f, 3},         // top boosters push down: sticks brake / steer
};
inline constexpr ThrusterDef DRAGONFLY_T[] = {
    {-50.f, 24.f, 0.f, 1.f, 0},   {50.f, 24.f, 0.f, 1.f, 1},
    {-22.f, 26.f, 0.61f, 0.6f, 2}, {22.f, 26.f, -0.61f, 0.6f, 3},              // crossed at 35°: together lift, alone strafe
};
inline constexpr ThrusterDef COLOSSUS_T[] = {
    {-90.f, 44.f, 0.f, 1.f, 0},   {90.f, 44.f, 0.f, 1.f, 1},
    {-44.f, 24.f, 0.f, 0.8f, 2},  {44.f, 24.f, 0.f, 0.8f, 3},                  // inboard engines: fine attitude control
};

inline constexpr ShipDef SHIP_DEFS[] = {
    {"Narrow", 22.f, 30.f, 12.f, 26.f, 0.85f, 450.f, 380.f, false, STYLE_FINS},
    {"Medium", 32.f, 32.f, 20.f, 28.f, 1.0f, 900.f, 400.f, false, STYLE_DOME | STYLE_STRIPES},
    {"Wide", 48.f, 28.f, 36.f, 26.f, 1.25f, 1600.f, 420.f, false, STYLE_TANKS},
    {"Barge", 64.f, 26.f, 52.f, 24.f, 1.6f, 2800.f, 440.f, false, STYLE_TANKS | STYLE_STRIPES | STYLE_DISH},
    {"Long", 26.f, 42.f, 14.f, 36.f, 1.1f, 1100.f, 390.f, false, STYLE_FINS | STYLE_DISH},
    {"Topdog", 28.f, 34.f, 16.f, 30.f, 1.05f, 950.f, 410.f, true, STYLE_DOME | STYLE_FINS},
    {"Canopy", 44.f, 30.f, 30.f, 28.f, 1.35f, 1700.f, 430.f, true, STYLE_DOME | STYLE_TANKS},
    {"Dart", 18.f, 34.f, 10.f, 30.f, 0.7f, 330.f, 470.f, false, STYLE_FINS | STYLE_DOME},
    {"Hauler", 56.f, 34.f, 44.f, 28.f, 1.9f, 3600.f, 450.f, false, STYLE_TANKS | STYLE_STRIPES},
    {"Spire", 20.f, 50.f, 12.f, 44.f, 1.0f, 1300.f, 400.f, false, STYLE_FINS | STYLE_DISH},
    {"Crab", 52.f, 24.f, 40.f, 22.f, 1.4f, 2000.f, 430.f, true, STYLE_TANKS | STYLE_DOME},
    {"Gnat", 14.f, 22.f, 8.f, 20.f, 0.5f, 170.f, 400.f, false, STYLE_DOME},
    {"Orca", 40.f, 44.f, 26.f, 38.f, 2.0f, 3300.f, 460.f, false, STYLE_TANKS | STYLE_DISH | STYLE_STRIPES},
    {"Moth", 60.f, 22.f, 48.f, 20.f, 0.9f, 1500.f, 380.f, false, STYLE_FINS | STYLE_DOME | STYLE_STRIPES},
    {"Frigate", 80.f, 38.f, 62.f, 34.f, 3.0f, 9500.f, 400.f, false, STYLE_DECK | STYLE_TANKS | STYLE_STRIPES | STYLE_DISH, FRIGATE_T, 4},
    {"Atlas", 70.f, 46.f, 56.f, 40.f, 3.6f, 12500.f, 410.f, false, STYLE_DECK | STYLE_TANKS | STYLE_DOME, ATLAS_T, 4},
    {"Dragonfly", 66.f, 30.f, 50.f, 24.f, 2.4f, 6000.f, 430.f, false, STYLE_DECK | STYLE_FINS | STYLE_DOME, DRAGONFLY_T, 4},
    {"Colossus", 100.f, 52.f, 90.f, 44.f, 5.0f, 22000.f, 400.f, false, STYLE_DECK | STYLE_TANKS | STYLE_STRIPES | STYLE_DISH, COLOSSUS_T, 4},
    // 1950s sci-fi needle rocket: very tall, narrow, classic tail fins + nose dish
    {"Rocket", 14.f, 96.f, 9.f, 88.f, 1.55f, 4200.f, 400.f, false, STYLE_FINS | STYLE_STRIPES | STYLE_DISH},
};
inline constexpr int SHIP_DEF_COUNT = static_cast<int>(sizeof(SHIP_DEFS) / sizeof(SHIP_DEFS[0]));
inline constexpr int DEFAULT_SHIP = 1;

// Zoom levels. Resolution independent: a level fixes how many world pixels are visible vertically, so
// the scale on screen is screen_height / visible_h (1:1 at 480 px for Near, 720 px for Medium).
struct ZoomLevel {
  const char* name;
  float visible_h;
};
inline constexpr ZoomLevel ZOOM_LEVELS[] = {{"NEAR", 480.f}, {"MEDIUM", 720.f}, {"FAR", 1080.f}};
inline constexpr int ZOOM_COUNT = static_cast<int>(sizeof(ZOOM_LEVELS) / sizeof(ZOOM_LEVELS[0]));
inline constexpr int DEFAULT_ZOOM = 1;
// First start without a saved zoom: small displays (the R36S panel) start Near, everything else Medium.
// Near is 1:1 on a 480 px high screen, which is also the cheapest path for weak GPUs.
inline int auto_zoom_for_height(int display_h) { return display_h <= 480 ? 0 : DEFAULT_ZOOM; }

// UI scale: multiplies the pixel font and all HUD/menu/minimap layout. 2X matches the pre-scale
// desktop look (font multiplier 3); 1X is roughly half that size for the R36S 480p panel; 3X/4X
// for larger / 4K displays. `font` is the 3x5 glyph pixel multiplier used by the renderer.
struct UiScaleLevel {
  const char* name;
  int font;
};
inline constexpr UiScaleLevel UI_SCALE_LEVELS[] = {{"1X", 2}, {"2X", 3}, {"3X", 5}, {"4X", 6}};
inline constexpr int UI_SCALE_COUNT = static_cast<int>(sizeof(UI_SCALE_LEVELS) / sizeof(UI_SCALE_LEVELS[0]));
inline constexpr int DEFAULT_UI_SCALE = 1;  // 2X
// First start without a saved scale: R36S-class panels → 1X, 1440p+ → 3X, 4K → 4X, else 2X.
inline int auto_ui_scale_for_height(int display_h) {
  if (display_h <= 480) return 0;
  if (display_h >= 2160) return 3;
  if (display_h >= 1440) return 2;
  return DEFAULT_UI_SCALE;
}

// Cargo crates: half extents (px) and mass (the same units as the ships')
struct CargoDef {
  const char* name;
  float half_w, half_h, mass;
};
inline constexpr CargoDef CARGO_DEFS[] = {
    {"Parcel", 12.f, 12.f, 0.15f},
    {"Crate", 18.f, 14.f, 0.35f},
    {"Barrel", 11.f, 18.f, 0.30f},
    {"Container", 26.f, 18.f, 0.70f},
    {"Heavy", 30.f, 22.f, 1.20f},
};
inline constexpr int CARGO_DEF_COUNT = static_cast<int>(sizeof(CARGO_DEFS) / sizeof(CARGO_DEFS[0]));

// Rope and hook (px, px/s)
namespace rope {
inline float MIN_LEN = 16.f;   // hook tucked under the belly
inline float OUT_LEN = 300.f;  // cable fully deployed
inline float REEL_SPEED = 130.f;
inline float REEL_SPEED_LOADED = 70.f;
inline float HOOK_MASS_FRACTION = 0.12f;  // of the ship's mass
inline float GRAB_REACH = 22.f;           // hook centre to the crate's edge
inline constexpr int CARGO_COUNT = 12;
// A crate on the hook is calmed so it hangs instead of flailing: air drag, friction in the pivot, and it no longer
// collides with the ship (a swinging crate would otherwise snag legs and engines and yank the ship around)
inline constexpr float HELD_LINEAR_DAMPING = 0.3f;
inline constexpr float HELD_ANGULAR_DAMPING = 2.5f;
inline constexpr float GRIP_FRICTION = 0.8f;  // pivot friction torque per unit of crate mass
}  // namespace rope
