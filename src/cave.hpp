// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <cstdint>
#include <vector>

#include "math.hpp"

struct LandingPad {
  float x0, x1, y;  // world surface y of the pad
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

// Toroidal in X, tall in Y. Rock is a boolean grid; draw-side data is baked in generate().
struct Cave {
  static constexpr float WORLD_W = 24000.f;
  static constexpr float WORLD_H = 4800.f;
  static constexpr float CELL = 16.f;
  static constexpr int GW = static_cast<int>(WORLD_W / CELL);  // 1500
  static constexpr int GH = static_cast<int>(WORLD_H / CELL);  // 300
  static constexpr int MAX_DEPTH = 5;

  std::vector<uint8_t> solid;    // GW * GH, 1 = rock
  std::vector<uint8_t> depth;    // 0 open, n = cells from the nearest open cell (capped)
  std::vector<uint8_t> contour;  // marching-squares case per cell
  std::vector<LandingPad> pads;
  std::vector<Star> stars;
  unsigned seed = 1;
  unsigned generation = 0;  // bumps on every generate(), for cache invalidation

  static float wrap_x(float wx) {
    wx = std::fmod(wx, WORLD_W);
    return wx < 0.f ? wx + WORLD_W : wx;
  }
  // Shortest signed X distance from → to on the torus
  static float wrap_delta(float from, float to) {
    return std::fmod(to - from + WORLD_W * 1.5f, WORLD_W) - WORLD_W * 0.5f;
  }
  static int wrap_gx(int gx) { return ((gx % GW) + GW) % GW; }

  bool is_solid_cell(int gx, int gy) const {
    if (gy < 0 || gy >= GH) return true;  // outside vertical = solid
    return solid[gy * GW + wrap_gx(gx)] != 0;
  }
  bool is_solid_world(float wx, float wy) const {
    if (wy < 0.f || wy >= WORLD_H) return true;
    return is_solid_cell(static_cast<int>(wrap_x(wx) / CELL), static_cast<int>(wy / CELL));
  }
  uint8_t depth_at(int gx, int gy) const { return depth[gy * GW + wrap_gx(gx)]; }
  uint8_t contour_at(int gx, int gy) const { return contour[gy * GW + wrap_gx(gx)]; }

  bool is_open_box(float wx, float wy, float half_w, float half_h) const;
  float floor_below(float wx, float wy) const;
  bool on_pad(float wx, float wy) const;
  const LandingPad* pad_below(float wx, float wy, float max_dist) const;

  void generate(unsigned seed);

 private:
  void clear_shaft(int gx0, int gx1, int floor_gy, int clearance_cells);
  void bake();
};
