// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <cstdint>
#include <vector>

#include "math.hpp"

struct LandingPad {
  float x0, x1, y;  // world surface y of the pad
  bool active = false;   // true once a sonar ping has painted the deck
  bool visited = false;  // true once the ship has settled on this pad (teleport target)
};

struct CargoSpot {
  float x, floor_y;  // crate stands on the floor at floor_y
  int kind;          // index into CARGO_DEFS
};

struct Star {
  float x, y;
  uint8_t size;   // px
  uint8_t phase;  // twinkle offset
};

// Marching-squares segments per case. Edges: 0 top, 1 right, 2 bottom, 3 left; -1 = none.
// Corner bits: 1 TL, 2 TR, 4 BR, 8 BL (sampled at cell centres).
struct ContourCase {
  int8_t seg[2][2];
};
inline constexpr ContourCase CONTOUR_CASES[16] = {
    {{{-1, -1}, {-1, -1}}}, {{{3, 0}, {-1, -1}}}, {{{0, 1}, {-1, -1}}}, {{{3, 1}, {-1, -1}}},
    {{{1, 2}, {-1, -1}}},   {{{3, 0}, {1, 2}}},   {{{0, 2}, {-1, -1}}}, {{{3, 2}, {-1, -1}}},
    {{{3, 2}, {-1, -1}}},   {{{0, 2}, {-1, -1}}}, {{{0, 1}, {3, 2}}},   {{{1, 2}, {-1, -1}}},
    {{{3, 1}, {-1, -1}}},   {{{0, 1}, {-1, -1}}}, {{{3, 0}, {-1, -1}}}, {{{-1, -1}, {-1, -1}}},
};

// A bounded map ringed by rock on all sides. Rock is a boolean grid; draw-side data is baked in generate().
struct Cave {
  // 4:3 playfield (was 5:1 ultra-wide). Chunk tiles are 480 px; both axes must divide cleanly.
  static constexpr float WORLD_W = 7680.f;   // 16 chunks
  static constexpr float WORLD_H = 5760.f;   // 12 chunks
  static constexpr float CELL = 16.f;
  static constexpr int GW = static_cast<int>(WORLD_W / CELL);  // 480
  static constexpr int GH = static_cast<int>(WORLD_H / CELL);  // 360
  static constexpr int MAX_DEPTH = 5;

  std::vector<uint8_t> solid;    // GW * GH, 1 = rock
  std::vector<uint8_t> depth;    // 0 open, n = cells from the nearest open cell (capped)
  std::vector<uint8_t> contour;  // marching-squares case per cell
  std::vector<LandingPad> pads;
  std::vector<CargoSpot> cargo;  // where crates start
  std::vector<Star> stars;
  unsigned seed = 1;
  unsigned generation = 0;  // bumps on every generate(), for cache invalidation

  static bool in_grid(int gx, int gy) { return gx >= 0 && gx < GW && gy >= 0 && gy < GH; }
  // Everything outside the map is rock
  bool is_solid_cell(int gx, int gy) const { return !in_grid(gx, gy) || solid[gy * GW + gx] != 0; }
  bool is_solid_world(float wx, float wy) const {
    if (wx < 0.f || wx >= WORLD_W || wy < 0.f || wy >= WORLD_H) return true;
    return is_solid_cell(static_cast<int>(wx / CELL), static_cast<int>(wy / CELL));
  }
  uint8_t depth_at(int gx, int gy) const { return in_grid(gx, gy) ? depth[gy * GW + gx] : MAX_DEPTH; }
  uint8_t contour_at(int gx, int gy) const { return in_grid(gx, gy) ? contour[gy * GW + gx] : 15; }

  bool is_open_box(float wx, float wy, float half_w, float half_h) const;
  float floor_below(float wx, float wy) const;
  bool on_pad(float wx, float wy) const;
  const LandingPad* pad_below(float wx, float wy, float max_dist) const;

  void generate(unsigned seed);

 private:
  void clear_shaft(int gx0, int gx1, int floor_gy, int clearance_cells);
  void bake();
};
