// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "cave.hpp"

#include <algorithm>

namespace {

constexpr int GW = Cave::GW;
constexpr int GH = Cave::GH;
constexpr float CELL = Cave::CELL;
constexpr int ROWS_LO = 3, ROWS_HI = GH - 3;  // automaton rows; roof/floor crust stays untouched

using Grid = std::vector<uint8_t>;

int at(const Grid& g, int gx, int gy) { return g[gy * GW + Cave::wrap_gx(gx)]; }

int count8(const Grid& g, int gx, int gy) {
  int n = 0;
  for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
      if (dx || dy) n += at(g, gx + dx, gy + dy);
  return n;
}
int count4(const Grid& g, int gx, int gy) {
  return at(g, gx + 1, gy) + at(g, gx - 1, gy) + at(g, gx, gy + 1) + at(g, gx, gy - 1);
}

// One synchronous cellular-automaton pass over the interior rows.
template <class Rule>
void automaton(Grid& g, Rule rule) {
  Grid next = g;
  for (int gy = ROWS_LO; gy < ROWS_HI; ++gy)
    for (int gx = 0; gx < GW; ++gx)
      next[gy * GW + gx] = rule(g, gx, gy, g[gy * GW + gx]);
  g.swap(next);
}

void carve_disc(Grid& g, int cx, int cy, int rad) {
  for (int dy = -rad; dy <= rad; ++dy)
    for (int dx = -rad; dx <= rad; ++dx) {
      int yy = cy + dy;
      if (dx * dx + dy * dy <= rad * rad && yy >= 3 && yy < GH - 3)
        g[yy * GW + Cave::wrap_gx(cx + dx)] = 0;
    }
}

// Wide meandering gallery; the ends are blended so the tunnel is continuous across the X seam.
void carve_main_tunnel(Grid& g, Rng& rng) {
  float cy = Cave::WORLD_H * 0.55f;
  float half = 520.f;
  std::vector<float> cys(GW), halves(GW);
  for (int gx = 0; gx < GW; ++gx) {
    cy += (rng.next() - 0.5f) * 16.f;
    cy = clampf(cy, Cave::WORLD_H * 0.35f, Cave::WORLD_H * 0.70f);
    half += (rng.next() - 0.5f) * 14.f;
    half = clampf(half, 380.f, 900.f);
    if (rng.next() < 0.012f) half = clampf(half + 200.f, 380.f, 1100.f);
    if (rng.next() < 0.008f) half = clampf(half - 60.f, 300.f, 1100.f);
    cys[gx] = cy;
    halves[gx] = half;
  }
  constexpr int BLEND = 160;
  for (int gx = GW - BLEND; gx < GW; ++gx) {
    float t = smoothstep(static_cast<float>(gx - (GW - BLEND)) / BLEND);
    cys[gx] = lerpf(cys[gx], cys[0], t);
    halves[gx] = lerpf(halves[gx], halves[0], t);
  }
  for (int gx = 0; gx < GW; ++gx) {
    int y0 = std::max(3, static_cast<int>((cys[gx] - halves[gx]) / CELL));
    int y1 = std::min(GH - 4, static_cast<int>((cys[gx] + halves[gx]) / CELL));
    for (int gy = y0; gy <= y1; ++gy) g[gy * GW + gx] = 0;
  }
}

void carve_branches(Grid& g, Rng& rng) {
  for (int b = 0; b < 28; ++b) {
    int gx = rng.range_i(0, GW - 1);
    int gy = rng.range_i(GH / 4, GH * 3 / 4);
    int len = rng.range_i(80, 260);
    int rad = rng.range_i(6, 14);
    float dir = rng.next() * 2.f * PI;
    for (int s = 0; s < len; ++s) {
      dir += (rng.next() - 0.5f) * 0.4f;
      gx = Cave::wrap_gx(static_cast<int>(gx + std::cos(dir) * 1.4f));
      gy = static_cast<int>(clampf(static_cast<float>(static_cast<int>(gy + std::sin(dir) * 1.4f)), 4.f,
                                   static_cast<float>(GH - 5)));
      carve_disc(g, gx, gy, rad);
    }
  }
}

// Short stalactites (from roof) and stalagmites (from floor)
void add_speleothems(Grid& g, Rng& rng) {
  for (int n = 0; n < 120; ++n) {
    int gx = rng.range_i(0, GW - 1);
    bool down = rng.next() < 0.5f;
    int len = rng.range_i(2, 6);
    if (down) {
      for (int gy = 3; gy < GH - 4; ++gy)
        if (g[gy * GW + gx] && !g[(gy + 1) * GW + gx]) {
          for (int k = 1; k <= len && gy + k < GH - 3; ++k) g[(gy + k) * GW + gx] = 1;
          break;
        }
    } else {
      for (int gy = GH - 4; gy > 3; --gy)
        if (g[gy * GW + gx] && !g[(gy - 1) * GW + gx]) {
          for (int k = 1; k <= len && gy - k > 3; ++k) g[(gy - k) * GW + gx] = 1;
          break;
        }
    }
  }
}

void add_pillars(Grid& g, Rng& rng) {
  for (int n = 0; n < 16; ++n) {
    int gx = rng.range_i(0, GW - 1);
    for (int gy = 4; gy < GH - 4; ++gy) {
      bool near_open = false;
      for (int o = -4; o <= 4; ++o) {
        int yy = gy + o;
        if (yy >= 0 && yy < GH && !g[yy * GW + gx]) near_open = true;
      }
      if (near_open && rng.next() < 0.55f) g[gy * GW + gx] = 1;
    }
  }
}

void smooth_and_clean(Grid& g) {
  for (int pass = 0; pass < 8; ++pass)
    automaton(g, [](const Grid& s, int x, int y, uint8_t v) -> uint8_t {
      int n = count8(s, x, y);
      if (v && n <= 4) return 0;
      if (!v && n >= 8) return 1;
      return v;
    });
  // Open dilation: rock touching air erodes
  for (int pass = 0; pass < 2; ++pass)
    automaton(g, [](const Grid& s, int x, int y, uint8_t v) -> uint8_t {
      return v && count8(s, x, y) == 8 ? 1 : 0;
    });
  // Remove lone rock pixels / thin spurs and lone air holes
  for (int pass = 0; pass < 4; ++pass)
    automaton(g, [](const Grid& s, int x, int y, uint8_t v) -> uint8_t {
      int n4 = count4(s, x, y);
      if (v && n4 <= 1) return 0;
      if (!v && n4 >= 3) return 1;
      return v;
    });
  for (int pass = 0; pass < 2; ++pass)
    automaton(g, [](const Grid& s, int x, int y, uint8_t) -> uint8_t { return count8(s, x, y) >= 5; });
  // Solid crust (roof + deep floor)
  for (int gx = 0; gx < GW; ++gx)
    for (int gy = 0; gy < 4; ++gy) g[gy * GW + gx] = g[(GH - 1 - gy) * GW + gx] = 1;
}

// Mask of the largest connected open region (4-neighbourhood, wraps in X)
Grid largest_open_region(const Grid& g) {
  std::vector<int32_t> label(GW * GH, 0);
  std::vector<uint32_t> stack;
  int best_label = 0, best_size = 0, next_label = 0;
  for (int start = 0; start < GW * GH; ++start) {
    if (g[start] || label[start]) continue;
    int id = ++next_label, size = 0;
    label[start] = id;
    stack.push_back(start);
    while (!stack.empty()) {
      int i = static_cast<int>(stack.back());
      stack.pop_back();
      ++size;
      int gx = i % GW, gy = i / GW;
      const int nx[4] = {gx + 1, gx - 1, gx, gx}, ny[4] = {gy, gy, gy + 1, gy - 1};
      for (int k = 0; k < 4; ++k) {
        if (ny[k] < 0 || ny[k] >= GH) continue;
        int j = ny[k] * GW + Cave::wrap_gx(nx[k]);
        if (!label[j] && !g[j]) { label[j] = id; stack.push_back(j); }
      }
    }
    if (size > best_size) { best_size = size; best_label = id; }
  }
  Grid mask(GW * GH, 0);
  for (int i = 0; i < GW * GH; ++i) mask[i] = label[i] == best_label;
  return mask;
}

}  // namespace

bool Cave::is_open_box(float wx, float wy, float half_w, float half_h) const {
  const float step = CELL * 0.5f;
  for (float dy = -half_h; dy <= half_h; dy += step)
    for (float dx = -half_w; dx <= half_w; dx += step)
      if (is_solid_world(wx + dx, wy + dy)) return false;
  return true;
}

// Nearest solid surface below a free point (for the ALT readout)
float Cave::floor_below(float wx, float wy) const {
  int gx = static_cast<int>(wrap_x(wx) / CELL);
  for (int gy = std::max(0, static_cast<int>(wy / CELL)); gy < GH; ++gy)
    if (is_solid_cell(gx, gy)) return static_cast<float>(gy) * CELL;
  return WORLD_H;
}

bool Cave::on_pad(float wx, float wy) const {
  wx = wrap_x(wx);
  for (const auto& p : pads)
    if (wx >= p.x0 && wx <= p.x1 && std::abs(wy - p.y) < CELL * 2.f) return true;
  return false;
}

// Pad whose span contains wx and whose surface is 0..max_dist below wy
const LandingPad* Cave::pad_below(float wx, float wy, float max_dist) const {
  wx = wrap_x(wx);
  for (const auto& p : pads)
    if (wx >= p.x0 && wx <= p.x1 && p.y >= wy && p.y - wy <= max_dist) return &p;
  return nullptr;
}

// Clear a vertical shaft of open air above a floor cell (for pads / spawn)
void Cave::clear_shaft(int gx0, int gx1, int floor_gy, int clearance_cells) {
  for (int gx = gx0; gx <= gx1; ++gx) {
    int xx = wrap_gx(gx);
    solid[floor_gy * GW + xx] = 1;
    for (int uy = 1; uy <= clearance_cells; ++uy)
      if (floor_gy - uy > 2) solid[(floor_gy - uy) * GW + xx] = 0;
  }
}

// Derive per-cell draw data: rock depth (BFS from air) and the contour case.
void Cave::bake() {
  depth.assign(GW * GH, 0);
  std::vector<uint32_t> queue;
  queue.reserve(GW * GH);
  std::vector<uint8_t> seen(GW * GH, 0);
  for (int i = 0; i < GW * GH; ++i)
    if (!solid[i]) { seen[i] = 1; queue.push_back(i); }
  for (size_t head = 0; head < queue.size(); ++head) {
    int i = static_cast<int>(queue[head]);
    int gx = i % GW, gy = i / GW;
    if (depth[i] >= MAX_DEPTH) continue;
    const int nx[4] = {gx + 1, gx - 1, gx, gx}, ny[4] = {gy, gy, gy + 1, gy - 1};
    for (int k = 0; k < 4; ++k) {
      if (ny[k] < 0 || ny[k] >= GH) continue;
      int j = ny[k] * GW + wrap_gx(nx[k]);
      if (seen[j]) continue;
      seen[j] = 1;
      depth[j] = depth[i] + 1;
      queue.push_back(j);
    }
  }
  for (int i = 0; i < GW * GH; ++i)
    if (solid[i] && !depth[i]) depth[i] = MAX_DEPTH;  // unreachable (cannot happen on open maps)

  contour.assign(GW * GH, 0);
  for (int gy = 0; gy < GH; ++gy)
    for (int gx = 0; gx < GW; ++gx)
      contour[gy * GW + gx] = static_cast<uint8_t>(
          (is_solid_cell(gx, gy) ? 1 : 0) | (is_solid_cell(gx + 1, gy) ? 2 : 0) |
          (is_solid_cell(gx + 1, gy + 1) ? 4 : 0) | (is_solid_cell(gx, gy + 1) ? 8 : 0));
}

void Cave::generate(unsigned s) {
  seed = s ? s : 1u;
  ++generation;
  solid.assign(GW * GH, 1);
  pads.clear();
  stars.clear();
  Rng rng(seed);

  carve_main_tunnel(solid, rng);
  carve_branches(solid, rng);
  add_speleothems(solid, rng);
  add_pillars(solid, rng);
  smooth_and_clean(solid);

  // --- Landing pads with tall cleared shafts for safe spawn ---
  constexpr int PAD_CLEARANCE = 18;    // cells of open air above a pad
  constexpr float PAD_MIN_DX = 480.f;  // min horizontal separation
  constexpr float PAD_MIN_DY = 220.f;  // min vertical separation if near in X
  const Grid main_cave = largest_open_region(solid);  // pads in sealed pockets would trap the player
  auto pads_overlap = [&](float x0, float x1, float y) {
    float cx = 0.5f * (x0 + x1), half = 0.5f * (x1 - x0);
    for (const auto& p : pads) {
      float dx = std::abs(wrap_delta(cx, 0.5f * (p.x0 + p.x1)));
      float dy = std::abs(y - p.y);
      float min_gap = half + 0.5f * (p.x1 - p.x0) + 80.f;
      if (dx < std::max(min_gap, PAD_MIN_DX) && dy < PAD_MIN_DY) return true;
      if (dx < min_gap) return true;  // same horizontal slot even if Y differs a lot
    }
    return false;
  };

  for (int attempt = 0; attempt < 200 && pads.size() < 14; ++attempt) {
    int gx0 = rng.range_i(20, GW - 50);
    int width = rng.range_i(10, 18);
    int mid = (gx0 + width / 2) % GW;
    int floor_gy = -1;
    for (int gy = 5; gy < GH - 6; ++gy)
      if (!solid[gy * GW + mid] && solid[(gy + 1) * GW + mid]) { floor_gy = gy + 1; break; }
    if (floor_gy < PAD_CLEARANCE + 5) continue;
    int open_count = 0;
    for (int uy = 1; uy <= 6; ++uy)
      if (!solid[(floor_gy - uy) * GW + mid]) ++open_count;
    if (open_count < 3 || !main_cave[(floor_gy - 1) * GW + mid]) continue;

    float x0 = gx0 * CELL, x1 = (gx0 + width) * CELL, y = floor_gy * CELL;
    if (pads_overlap(x0, x1, y)) continue;
    clear_shaft(gx0, gx0 + width - 1, floor_gy, PAD_CLEARANCE);
    pads.push_back({x0, x1, y});
  }
  if (pads.empty()) {
    int gx0 = GW / 2, floor_gy = GH * 2 / 3;
    clear_shaft(gx0, gx0 + 16, floor_gy, PAD_CLEARANCE);
    pads.push_back({gx0 * CELL, (gx0 + 16) * CELL, floor_gy * CELL});
  }

  bake();

  // Background dots; only those in open space are kept
  for (int i = 0; i < 2200; ++i) {
    float sx = rng.next() * WORLD_W, sy = rng.next() * WORLD_H;
    uint32_t h = static_cast<uint32_t>(i) * 2654435761u;
    if (is_solid_world(sx, sy)) continue;
    stars.push_back({sx, sy, static_cast<uint8_t>(2 + (h >> 24) % 4), static_cast<uint8_t>(h >> 8)});
  }
}
