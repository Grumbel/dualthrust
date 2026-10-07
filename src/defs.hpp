// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Static game data: tuning constants, palette and ship presets.

#include "math.hpp"

inline constexpr const char* APP_VERSION = "0.1.0";

namespace tune {
inline constexpr float SIM_STEP = 1.f / 120.f;  // fixed real-time step
inline constexpr float TIME_SCALE = 0.62f;      // sim seconds per real second
inline constexpr int MAX_STEPS_PER_FRAME = 8;

inline constexpr float GRAVITY = 120.f;
inline constexpr float LINEAR_DRAG = 0.08f;
inline constexpr float ANGULAR_DRAG = 1.2f;
inline constexpr float MAX_ANGULAR_VEL = 8.f;

inline constexpr float LAND_MAX_VY = 55.f;
inline constexpr float LAND_MAX_VX = 40.f;
inline constexpr float LAND_MAX_ANGLE = 0.22f;
inline constexpr float LAND_MAX_ANGVEL = 1.2f;

// Impact speed along the surface normal; above → crash, below → bounce
inline constexpr float CRASH_IMPACT_SPEED = 200.f;
inline constexpr float BOUNCE_RESTITUTION = 0.42f;
inline constexpr float BOUNCE_FRICTION = 0.85f;
inline constexpr float BOUNCE_ANG_DAMP = 0.55f;

inline constexpr size_t MAX_PARTICLES = 1500;
inline constexpr float EXHAUST_RATE = 150.f;  // particles / sec / engine at full thrust
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
}  // namespace pal

struct ShipDef {
  const char* name;
  float half_w, half_h;
  float engine_offset_x, engine_offset_y;  // offset magnitudes
  float mass, inertia, max_thrust;
  bool engines_top;  // true: engines on nose/top (-y); false: aft/bottom (+y)

  // Signed local Y of the engine mounts (local +y = ground side when upright)
  float eng_y() const { return engines_top ? -engine_offset_y : engine_offset_y; }
  // Local Y of the nozzle rim, where exhaust leaves
  float nozzle_y() const { return eng_y() + (engines_top ? 16.f : 14.f); }
  float foot_y() const { return half_h * 0.95f; }
};

inline constexpr ShipDef SHIP_DEFS[] = {
    {"Narrow", 22.f, 30.f, 12.f, 26.f, 0.85f, 450.f, 380.f, false},
    {"Medium", 32.f, 32.f, 20.f, 28.f, 1.0f, 900.f, 400.f, false},
    {"Wide", 48.f, 28.f, 36.f, 26.f, 1.25f, 1600.f, 420.f, false},
    {"Barge", 64.f, 26.f, 52.f, 24.f, 1.6f, 2800.f, 440.f, false},
    {"Long", 26.f, 42.f, 14.f, 36.f, 1.1f, 1100.f, 390.f, false},
    {"Topdog", 28.f, 34.f, 16.f, 30.f, 1.05f, 950.f, 410.f, true},
    {"Canopy", 44.f, 30.f, 30.f, 28.f, 1.35f, 1700.f, 430.f, true},
};
inline constexpr int SHIP_DEF_COUNT = static_cast<int>(sizeof(SHIP_DEFS) / sizeof(SHIP_DEFS[0]));
inline constexpr int DEFAULT_SHIP = 1;
