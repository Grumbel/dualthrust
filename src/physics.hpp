// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Box2D (v3, C API) wrapper: the world, the cave as streamed static terrain, and the ship's rigid bodies.
//
// Game code keeps working in pixels (+y down); Box2D works in metres, PPM pixels per metre. Terrain is built
// on demand around the ship from the cave's marching-squares contour (the very line the renderer draws) as
// one-sided chain shapes, in 30x30-cell chunks keyed by chunk coordinate. The map is bounded and ringed by rock.

#include <box2d/box2d.h>

#include <cstdint>
#include <unordered_map>

#include "cave.hpp"
#include "defs.hpp"
#include "math.hpp"

inline constexpr float PPM = 32.f;  // px per metre

inline b2Vec2 to_b2(Vec2 v) { return {v.x / PPM, v.y / PPM}; }
inline Vec2 from_b2(b2Vec2 v) { return {v.x * PPM, v.y * PPM}; }

// Collision categories
inline constexpr uint64_t CAT_TERRAIN = 1, CAT_SHIP = 2, CAT_CARGO = 4, CAT_HOOK = 8;

// Shape user data tags (Box2D hands the pointer back in contact events)
enum class Part : intptr_t { Terrain = 0, Hull = 1, Foot = 2, Strut = 3, Cargo = 4, Hook = 5 };
inline bool is_ship_part(Part p) { return p == Part::Hull || p == Part::Foot || p == Part::Strut; }
inline void* part_tag(Part p) { return reinterpret_cast<void*>(static_cast<intptr_t>(p)); }
inline Part shape_part(b2ShapeId s) { return static_cast<Part>(reinterpret_cast<intptr_t>(b2Shape_GetUserData(s))); }

// The ship's bodies: hull plus two legs (0 = left, 1 = right) on prismatic joints, and the hook on its cable
// (a distance joint used as a rope: free between ~0 and its maximum length). `grip` joins the hook to a crate.
struct ShipBodies {
  b2BodyId hull = b2_nullBodyId;
  b2BodyId leg[2] = {b2_nullBodyId, b2_nullBodyId};
  b2JointId joint[2] = {b2_nullJointId, b2_nullJointId};
  b2BodyId hook = b2_nullBodyId;
  b2JointId cable = b2_nullJointId;
  b2JointId grip = b2_nullJointId;
};

class Physics {
 public:
  Physics() = default;
  Physics(const Physics&) = delete;
  Physics& operator=(const Physics&) = delete;
  ~Physics() { shutdown(); }

  void init();
  void shutdown();
  bool ready() const { return b2World_IsValid(world_); }
  b2WorldId world() const { return world_; }
  void step(float dt);

  // Terrain. reset() drops all chunks (new cave); stream() creates the chunks around `anchor` (px), at most `max_new` per call, and drops far ones.
  void reset_terrain();
  // Anchors are the places that need ground: the ship and anything it carries
  void stream(const Cave& cave, const Vec2* anchors, int count, int max_new = 2);
  void stream(const Cave& cave, Vec2 anchor, int max_new = 2) { stream(cave, &anchor, 1, max_new); }
  bool terrain_at(Vec2 px) const;  // is the ground around this point built?
  int chunk_count() const { return static_cast<int>(chunks_.size()); }
  unsigned terrain_generation() const { return generation_; }

  // Ship rigid bodies at pos (px) / angle / velocity (px/s)
  ShipBodies create_ship(const ShipDef& def, Vec2 pos, float angle, Vec2 vel, float ang_vel);
  void destroy_ship(ShipBodies& s);
  // A crate; created disabled when there is no ground under it yet (see Game's cargo activation)
  b2BodyId create_cargo(Vec2 pos, float angle, const CargoDef& def);

 private:
  b2WorldId world_ = b2_nullWorldId;
  std::unordered_map<uint64_t, b2BodyId> chunks_;
  unsigned generation_ = ~0u;
  const Cave* built_for_ = nullptr;
};
