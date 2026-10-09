// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <vector>

#include "cave.hpp"
#include "defs.hpp"
#include "ecs.hpp"
#include "math.hpp"
#include "physics.hpp"
#include "stats.hpp"

// ---------------------------------------------------------------------------
// Components (plain data)
// ---------------------------------------------------------------------------
enum class FlightState { Flying, Landed, Crashed };

// Mirrors of the rigid body for drawing, camera and effects (px, px/s); particles are not simulated by Box2D
struct Transform { Vec2 pos; float angle = 0.f; };
struct Motion { Vec2 vel; float ang_vel = 0.f; };
struct Hull { const ShipDef* def = nullptr; };
struct Thrusters {
  static constexpr int MAX = 8;  // hard cap on thrusters per ship (SHIP_DEFS rows stay ≤ this)
  float level[6] = {};           // control channels 0..5
  float emit_acc[MAX] = {};      // fractional exhaust particles owed, per thruster index
  float damage[MAX] = {};        // 0 = healthy, 1 = dead; per thruster, not per channel
  // Effective nozzle output after damage, fuel/hurt power and random flutter (written by forces_system;
  // exhaust, flames and audio read it so visuals, physics and sound stay in lockstep).
  float output[MAX] = {};
};
// Flying / Landed / Crashed is a label on top of the rigid-body simulation: the body never stops simulating.
struct Flight {
  FlightState state = FlightState::Flying;
  float timer = 0.f;     // time in the current state
  float settle = 0.f;    // how long the ship has been resting (Flying -> Landed at SETTLE_TIME)
  int contacts = 0;      // touching contact manifolds on hull and legs this tick
  Vec2 contact_pt;       // lowest touching point (px)
  float fuel = 1.f;      // 0..1 tank; low fuel limps engines, does not hard-stop
  float hurt = 0.f;      // 0..1 soft damage from hard bumps; repairs on a pad
};
// Box2D bodies of the ship, and the landing gear on top of them
struct Body { ShipBodies b; };
struct Legs {
  bool deployed = true;
  float trans[2] = {0.f, 0.f};  // foot travel along its strut in px: 0 = fully out, negative = pushed in
};

// The winch: the cable's reach and what the hook holds. The hook's own pose is mirrored for drawing.
struct Rope {
  float length = rope::MIN_LEN;  // how far the cable lets the hook go (px)
  bool out = false;              // winch wants the cable fully out (true) or fully in (false); no stops between
  Entity held = NULL_ENTITY;     // crate on the hook
  Vec2 hook_pos;
  float hook_angle = 0.f;
  Vec2 anchor;                   // where the cable leaves the hull (px)
  float slack = 0.f;             // px of cable not taut, for drawing the sag
};
// A crate. `body` stays valid for the life of the cave; it is disabled while the ground around it is not built.
struct Cargo {
  b2BodyId body = b2_nullBodyId;
  const CargoDef* def = nullptr;
  bool picked = false;      // lifted since it last stood on a pad: counts as delivered when it rests on one
  float rest_time = 0.f;    // how long it has been still
  int dest_pad = -1;        // home pad index for return-to-base
  Vec2 haul_origin{};       // where it was first lifted (distance bonus)
  bool has_origin = false;
};

// Short-lived visual: fades `from`→`to` over `ttl`, optionally affected by gravity and drag.
struct Particle {
  float life = 0.f, ttl = 1.f;
  float size = 2.f;
  float drag = 0.f;     // 1/s
  float gravity = 0.f;  // multiplier of world gravity
  Rgba from{}, to{};
  bool additive = true;
};

using World = Registry<Transform, Motion, Hull, Thrusters, Flight, Particle, Body, Legs, Rope, Cargo>;

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------
struct Camera {
  float x = 0.f, y = 0.f;
  float vw = 1280.f, vh = 720.f;  // viewport in world px: what the zoom level makes visible
  int zoom = DEFAULT_ZOOM;        // index into ZOOM_LEVELS
  float shake = 0.f;  // 0..1, decays
  Vec2 shake_off;

  float center_x() const { return x + vw * 0.5f; }
};

// Landed / Crashed / Bounce come out of the simulation; the rest are feedback for the pilot's own actions
// (beeps), pushed straight to `fired`
enum class SimEventKind { Landed, Crashed, Bounce, HookOut, HookIn, Grab, Release, NoTarget, Delivered, LegsOut, LegsIn, SonarPing };
struct SimEvent {
  SimEventKind kind;
  Vec2 pos, normal;
  float strength;
};

// Sonar ping: a search pulse for pads, cargo and deep-cave signals — not map uncover.
// The wavefront expands; when it sweeps a target, a reflection segment is spawned (bright
// arc on the circle, then a pulse returning toward the origin). Fog of war is filled by
// passive proximity around the ship instead.
struct SonarReflection {
  float angle = 0.f;   // direction from origin to the hit (radians)
  float hit_r = 0.f;   // distance of the target
  float age = 0.f;     // seconds since registration
  float life = 1.4f;   // total visual lifetime
  enum class Kind : uint8_t { Pad = 0, Cargo = 1, Signal = 2 } kind = Kind::Pad;
};

struct SonarPing {
  bool active = false;
  bool fading = false;       // true once past max_radius; ring still expands while fade drops
  Vec2 origin;
  float radius = 0.f;
  float prev_radius = 0.f;
  float max_radius = 900.f;
  float speed = 720.f;
  float fade = 1.f;          // 1 until max_radius; then 1→0 while radius keeps growing
  std::vector<SonarReflection> echoes;
  // Avoid double-echoing the same target in one ping
  std::vector<uint8_t> pad_hit;     // size = pads, 1 if already reflected
  std::vector<Entity> cargo_hit;
  std::vector<uint8_t> signal_hit;
};

struct Game {
  Cave cave;
  Physics phys;
  World ecs;
  Entity ship = NULL_ENTITY;
  unsigned cargo_generation = ~0u;  // cave the crates belong to
  char notice[40] = "";             // short message in the middle of the HUD ("CARGO PICKED UP")
  float notice_timer = 0.f;
  Stats stats;
  bool stats_dirty = false;
  bool stats_enabled = true;  // off for screenshot/debug runs so they leave the saved statistics alone
  Camera cam;
  Rng rng{0x5eed};
  std::vector<SimEvent> events;
  std::vector<SimEvent> fired;  // copy of this tick's events for the audio layer; drained by main
  std::vector<Entity> dead;
  float time = 0.f;  // accumulated sim time
  int score = 0;             // this session (resets on quit; not permanent stats)
  float sonar_cool = 0.f;    // seconds until the next ping is allowed
  int last_pad = -1;
  int home_pad = 0;  // cargo return-to-base pad
  int hauls_run = 0; // deliveries completed this cave

  // Debug menu multipliers (1 = stock). Applied live; mass rebuild happens when mass_mul changes.
  float dbg_mass_mul = 1.f;
  float dbg_thrust_mul = 1.f;

  // Fog of war: one byte per cave cell, 0 = unknown, 1..255 = reveal strength.
  // Filled by passive proximity around the ship (and a small blob when sonar tags a target).
  // Size GW*GH after the first cave generate; reset when the cave regenerates.
  std::vector<uint8_t> revealed;
  bool reveal_dirty = true;  // minimap texture needs a rebuild
  SonarPing sonar;

  // Deep-cave signals: faint beacons that light up when a ping brushes them (exploration goals).
  struct Signal {
    Vec2 pos;
    bool found = false;
  };
  std::vector<Signal> signals;
  int cells_explored = 0;  // cells painted at least once this cave
  int explore_tier = 0;    // 0..4 milestones for map coverage (25/50/75/100)
  bool signals_cleared = false;
  bool pads_cleared = false;

  // Brief phosphor ghosts left on rock faces the last ping painted (world-space residues)
  struct Residue {
    Vec2 pos;
    float life = 0.f;
  };
  std::vector<Residue> residues;

  // Permanent crash debris: line scraps that settle on the rock and survive respawns
  // (cleared only on a new cave). Caps so a long session stays cheap to draw.
  struct WreckPart {
    Vec2 a{}, b{};     // world-space endpoints of a scrap segment
    Vec2 vel{};
    float ang_vel = 0.f;
    float life = 1.f;  // 1 while airborne; not used for expiry, just settled flag via vel
    bool settled = false;
    Rgba col{};
  };
  std::vector<WreckPart> wreckage;

  // Ambient cave life: slow wanderers that answer a sonar ping (exploration flavour)
  struct Echo {
    Vec2 pos;
    Vec2 vel;
    float phase = 0.f;
    float cool = 0.f;  // after answering a ping, stay quiet briefly
  };
  std::vector<Echo> echoes;
};
