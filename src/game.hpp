// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <vector>

#include "cave.hpp"
#include "defs.hpp"
#include "ecs.hpp"
#include "math.hpp"

// ---------------------------------------------------------------------------
// Components (plain data)
// ---------------------------------------------------------------------------
enum class FlightState { Flying, Landed, Crashed };

struct Transform { Vec2 pos; float angle = 0.f; };
struct Motion { Vec2 vel; float ang_vel = 0.f; };
struct Hull { const ShipDef* def = nullptr; };
struct Thrusters {
  float level[2] = {0.f, 0.f};     // 0 = left, 1 = right, 0..1
  float emit_acc[2] = {0.f, 0.f};  // fractional exhaust particles owed
};
struct Flight { FlightState state = FlightState::Flying; float timer = 0.f; };

// Short-lived visual: fades `from`→`to` over `ttl`, optionally affected by gravity and drag.
struct Particle {
  float life = 0.f, ttl = 1.f;
  float size = 2.f;
  float drag = 0.f;     // 1/s
  float gravity = 0.f;  // multiplier of world gravity
  Rgba from{}, to{};
  bool additive = true;
};

using World = Registry<Transform, Motion, Hull, Thrusters, Flight, Particle>;

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------
struct Camera {
  float x = 0.f, y = 0.f;
  float vw = 1280.f, vh = 720.f;  // viewport in world px: what the zoom level makes visible
  int zoom = DEFAULT_ZOOM;        // index into ZOOM_LEVELS
  float prev_ship_x = 0.f;
  bool have_prev = false;
  float shake = 0.f;  // 0..1, decays
  Vec2 shake_off;

  float center_x() const { return x + vw * 0.5f; }
  // X of a wrapped world coordinate, continuous around the camera centre
  float continuous_x(float wx) const {
    float c = center_x();
    return c + Cave::wrap_delta(c, Cave::wrap_x(wx));
  }
};

enum class SimEventKind { Landed, Crashed, Bounce };
struct SimEvent {
  SimEventKind kind;
  Vec2 pos, normal;
  float strength;
};

struct Game {
  Cave cave;
  World ecs;
  Entity ship = NULL_ENTITY;
  Camera cam;
  Rng rng{0x5eed};
  std::vector<SimEvent> events;
  std::vector<SimEvent> fired;  // copy of this tick's events for the audio layer; drained by main
  std::vector<Entity> dead;
  float time = 0.f;  // accumulated sim time
};
