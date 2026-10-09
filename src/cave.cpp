// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "cave.hpp"

#include <algorithm>

namespace {

constexpr int GW = Cave::GW;
constexpr int GH = Cave::GH;
constexpr float CELL = Cave::CELL;
constexpr int ROWS_LO = 3, ROWS_HI = GH - 3;  // automaton rows; roof/floor crust stays untouched
constexpr int EDGE = 4;                         // columns of solid rock at each side of the map

using Grid = std::vector<uint8_t>;

int at(const Grid& g, int gx, int gy) { return Cave::in_grid(gx, gy) ? g[gy * GW + gx] : 1; }

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
      const int xx = cx + dx;
      if (dx * dx + dy * dy <= rad * rad && yy >= 3 && yy < GH - 3 && xx >= EDGE && xx < GW - EDGE) g[yy * GW + xx] = 0;
    }
}

// Smooth value noise in [0,1), deterministic per seed
float hash01(int x, int y, uint32_t seed) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return (h & 0xffffff) / static_cast<float>(1 << 24);
}
float vnoise(float x, float y, uint32_t seed) {
  const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
  const float fx = smoothstep(x - xi), fy = smoothstep(y - yi);
  return lerpf(lerpf(hash01(xi, yi, seed), hash01(xi + 1, yi, seed), fx),
               lerpf(hash01(xi, yi + 1, seed), hash01(xi + 1, yi + 1, seed), fx), fy);
}
float fbm(float x, float y, uint32_t seed, int octaves = 4) {
  float sum = 0.f, amp = 0.5f, norm = 0.f;
  for (int o = 0; o < octaves; ++o, x *= 2.f, y *= 2.f, amp *= 0.5f) {
    sum += amp * vnoise(x, y, seed + static_cast<uint32_t>(o) * 101u);
    norm += amp;
  }
  return sum / norm;
}

// Centre line of the big gallery: a slow wander around mid-height
float spine_y(int gx, uint32_t seed) {
  return Cave::WORLD_H * 0.52f + (fbm(gx * CELL / 3600.f, 0.5f, seed + 1, 3) - 0.5f) * Cave::WORLD_H * 0.40f;
}

// The main cavern: a random fill whose rock density follows a warped noise field around the spine, so walls
// are bumpy and overhung instead of straight. The gallery thins out toward both map edges, ending in fingers
// of passage rather than a sheer wall. The automaton passes then turn the noise into rounded rock.
void fill_cavern(Grid& g, Rng& rng, uint32_t seed) {
  const float edge_zone = GW * 0.24f;
  std::vector<float> spine(GW), halves(GW);  // per column
  for (int gx = EDGE; gx < GW - EDGE; ++gx) {
    const float side = std::min(gx - EDGE, GW - EDGE - 1 - gx) / edge_zone;
    spine[gx] = spine_y(gx, seed);
    halves[gx] = (420.f + 520.f * fbm(gx * CELL / 2600.f, 3.1f, seed + 2, 3)) * lerpf(0.28f, 1.f, smoothstep(side));
  }
  for (int gy = ROWS_LO; gy < ROWS_HI; ++gy)
    for (int gx = EDGE; gx < GW - EDGE; ++gx) {
      const float wx = gx * CELL, wy = gy * CELL;
      const float half = halves[gx];
      const float warp = (fbm(wx / 520.f, wy / 520.f, seed + 3) - 0.5f) * 1.1f +
                         (fbm(wx / 1700.f, wy / 1700.f, seed + 4, 3) - 0.5f) * 1.3f;
      const float d = std::abs(wy - spine[gx]) / half + warp;
      const float rock = smoothstep((d - 0.55f) / 0.9f);  // solid beyond ~1.45, open inside ~0.55, noisy between
      g[gy * GW + gx] = rng.next() < rock ? 1 : 0;
    }
}

// Side chambers: rounded bays off the gallery
void carve_chambers(Grid& g, Rng& rng, uint32_t seed) {
  for (int n = 0; n < 30; ++n) {
    const int gx = rng.range_i(EDGE + 30, GW - EDGE - 30);
    const float off = (rng.next() < 0.5f ? -1.f : 1.f) * rng.range(250.f, 900.f);
    const int gy = static_cast<int>((spine_y(gx, seed) + off) / CELL);
    const int rad = rng.range_i(8, 26);
    carve_disc(g, gx, gy, rad);
    carve_disc(g, gx + rng.range_i(-10, 10), gy + rng.range_i(-8, 8), rad * 2 / 3);
  }
}

// Winding side passages: a random walk with a steadily turning heading and a breathing radius
void carve_branches(Grid& g, Rng& rng, uint32_t seed) {
  for (int b = 0; b < 18; ++b) {
    float fx = static_cast<float>(rng.range_i(EDGE + 20, GW - EDGE - 20));
    float fy = clampf(spine_y(static_cast<int>(fx), seed) / CELL + rng.range(-60.f, 60.f), 10.f, GH - 11.f);
    const int len = rng.range_i(60, 200);
    const float rad0 = rng.range(4.f, 9.f);
    float dir = rng.next() * 2.f * PI, turn = 0.f;
    for (int s = 0; s < len; ++s) {
      turn = turn * 0.92f + (rng.next() - 0.5f) * 0.35f;  // smooth, meandering curvature
      dir += turn;
      fx = clampf(fx + std::cos(dir) * 1.3f, EDGE + 2.f, GW - EDGE - 3.f);
      fy = clampf(fy + std::sin(dir) * 1.3f, 8.f, GH - 9.f);
      const float rad = rad0 * (1.f + 0.35f * std::sin(s * 0.11f + b)) ;
      carve_disc(g, static_cast<int>(fx), static_cast<int>(fy), static_cast<int>(rad));
    }
  }
}

void fill_disc(Grid& g, int cx, int cy, int rad) {
  for (int dy = -rad; dy <= rad; ++dy)
    for (int dx = -rad; dx <= rad; ++dx) {
      const int xx = cx + dx, yy = cy + dy;
      if (dx * dx + dy * dy <= rad * rad && yy >= 3 && yy < GH - 3 && xx >= EDGE && xx < GW - EDGE) g[yy * GW + xx] = 1;
    }
}
// Stalactites (from the roof) and stalagmites (from the floor): tapering cones, not one-cell spikes
void add_speleothems(Grid& g, Rng& rng) {
  for (int n = 0; n < 110; ++n) {
    int gx = rng.range_i(EDGE + 4, GW - EDGE - 5);
    const bool down = rng.next() < 0.5f;
    const float base = rng.range(2.f, 4.5f);
    const int len = static_cast<int>(base * rng.range(1.3f, 2.3f));  // squat cones: base-to-tip ratio under ~2.3
    auto cone = [&](int gy, int dir) {
      for (int k = 1; k <= len; ++k)
        fill_disc(g, gx, gy + dir * k, std::max(1, static_cast<int>(base * (1.f - static_cast<float>(k) / (len + 1)) + 0.7f)));
    };
    if (down) {
      for (int gy = 3; gy < GH - 4; ++gy)
        if (g[gy * GW + gx] && !g[(gy + 1) * GW + gx]) { cone(gy, +1); break; }
    } else {
      for (int gy = GH - 4; gy > 3; --gy)
        if (g[gy * GW + gx] && !g[(gy - 1) * GW + gx]) { cone(gy, -1); break; }
    }
  }
}

// Rock columns joining roof and floor in the big chambers, thick at the ends and waisted in the middle
void add_rock_islands(Grid& g, Rng& rng) {
  for (int n = 0; n < 24; ++n) {
    const int gx = rng.range_i(EDGE + 20, GW - EDGE - 20), gy = rng.range_i(ROWS_LO + 20, ROWS_HI - 20);
    if (g[gy * GW + gx]) continue;
    // only in open space: the whole blob area must be air
    bool air = true;
    for (int dy = -16; dy <= 16 && air; dy += 4)
      for (int dx = -16; dx <= 16; dx += 4)
        if (at(g, gx + dx, gy + dy)) { air = false; break; }
    if (!air) continue;
    float x = static_cast<float>(gx), y = static_cast<float>(gy);
    const int lumps = rng.range_i(3, 7);
    for (int i = 0; i < lumps; ++i) {
      fill_disc(g, static_cast<int>(x), static_cast<int>(y), rng.range_i(3, 8));
      x += rng.range(-7.f, 7.f);
      y += rng.range(-6.f, 6.f);
    }
  }
}

void smooth_and_clean(Grid& g) {
  // Majority smoothing rounds the noise into blobs
  for (int pass = 0; pass < 5; ++pass)
    automaton(g, [](const Grid& s, int x, int y, uint8_t v) -> uint8_t { return count8(s, x, y) + v >= 5; });
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
  // Solid crust (roof, deep floor, both sides)
  for (int gx = 0; gx < GW; ++gx)
    for (int gy = 0; gy < 4; ++gy) g[gy * GW + gx] = g[(GH - 1 - gy) * GW + gx] = 1;
  for (int gy = 0; gy < GH; ++gy)
    for (int gx = 0; gx < EDGE; ++gx) g[gy * GW + gx] = g[gy * GW + GW - 1 - gx] = 1;
}

// Mask of the largest connected open region (4-neighbourhood)
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
        if (!Cave::in_grid(nx[k], ny[k])) continue;
        int j = ny[k] * GW + nx[k];
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
  int gx = std::clamp(static_cast<int>(wx / CELL), 0, GW - 1);
  for (int gy = std::max(0, static_cast<int>(wy / CELL)); gy < GH; ++gy)
    if (is_solid_cell(gx, gy)) return static_cast<float>(gy) * CELL;
  return WORLD_H;
}

bool Cave::on_pad(float wx, float wy) const {
  for (const auto& p : pads)
    if (wx >= p.x0 && wx <= p.x1 && std::abs(wy - p.y) < CELL * 2.f) return true;
  return false;
}

// Pad whose span contains wx and whose surface is 0..max_dist below wy
const LandingPad* Cave::pad_below(float wx, float wy, float max_dist) const {
  for (const auto& p : pads)
    if (p.active && wx >= p.x0 && wx <= p.x1 && p.y >= wy && p.y - wy <= max_dist) return &p;
  return nullptr;
}

// Clear a vertical shaft of open air above a floor cell (for pads / spawn)
void Cave::clear_shaft(int gx0, int gx1, int floor_gy, int clearance_cells) {
  for (int gx = gx0; gx <= gx1; ++gx) {
    solid[floor_gy * GW + gx] = 1;
    for (int uy = 1; uy <= clearance_cells; ++uy)
      if (floor_gy - uy > 2) solid[(floor_gy - uy) * GW + gx] = 0;
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
      if (!in_grid(nx[k], ny[k])) continue;
      int j = ny[k] * GW + nx[k];
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
  cargo.clear();
  Rng rng(seed);

  const uint32_t nseed = seed * 2654435761u + 17u;
  fill_cavern(solid, rng, nseed);
  carve_chambers(solid, rng, nseed);
  carve_branches(solid, rng, nseed);
  smooth_and_clean(solid);
  add_rock_islands(solid, rng);  // after smoothing: its erosion passes would thin them away
  add_speleothems(solid, rng);

  // --- Landing pads with tall cleared shafts for safe spawn ---
  constexpr int PAD_CLEARANCE = 18;    // cells of open air above a pad
  constexpr float PAD_MIN_DX = 360.f;  // min horizontal separation (4:3 map)
  constexpr float PAD_MIN_DY = 220.f;  // min vertical separation if near in X
  const Grid main_cave = largest_open_region(solid);  // pads in sealed pockets would trap the player
  auto pads_overlap = [&](float x0, float x1, float y) {
    float cx = 0.5f * (x0 + x1), half = 0.5f * (x1 - x0);
    for (const auto& p : pads) {
      float dx = std::abs(cx - 0.5f * (p.x0 + p.x1));
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
    int mid = gx0 + width / 2;
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

  // --- Cargo: crates on flat floor in the main cave, away from the pads and from each other ---
  cargo.clear();
  const int kinds = 6;  // matches CARGO_DEFS (Parcel..Anvil)
  for (int attempt = 0; attempt < 600 && static_cast<int>(cargo.size()) < 12; ++attempt) {
    const int gx = rng.range_i(EDGE + 12, GW - EDGE - 12);
    int floor_gy = -1;
    for (int gy = 8; gy < GH - 8; ++gy)
      if (!solid[gy * GW + gx] && solid[(gy + 1) * GW + gx]) { floor_gy = gy + 1; if (rng.next() < 0.5f) break; }
    if (floor_gy < 0 || !main_cave[(floor_gy - 1) * GW + gx]) continue;
    bool flat = true, open_above = true;
    for (int dx = -3; dx <= 3 && flat; ++dx) {
      flat = solid[floor_gy * GW + gx + dx] && !solid[(floor_gy - 1) * GW + gx + dx];
      for (int up = 1; up <= 5; ++up) if (solid[(floor_gy - up) * GW + gx + dx]) open_above = false;
    }
    if (!flat || !open_above) continue;
    const float x = (gx + 0.5f) * CELL;
    bool clash = false;
    for (const auto& p : pads) if (x > p.x0 - 200.f && x < p.x1 + 200.f && std::abs(floor_gy * CELL - p.y) < 400.f) clash = true;
    for (const auto& k : cargo) if (std::abs(k.x - x) < 320.f && std::abs(k.floor_y - floor_gy * CELL) < 300.f) clash = true;
    if (clash) continue;
    // Depth bias: light parcels near the surface, clamps/anvils in the deep
    const float depth = static_cast<float>(floor_gy) / static_cast<float>(GH);
    int kind;
    const float r = rng.next();
    if (depth < 0.35f) {
      kind = (r < 0.55f) ? 0 : 1;                     // Parcel / Crate
    } else if (depth < 0.55f) {
      kind = (r < 0.35f) ? 0 : (r < 0.70f ? 1 : 2); // Parcel / Crate / Barrel
    } else if (depth < 0.72f) {
      kind = (r < 0.25f) ? 1 : (r < 0.55f ? 2 : (r < 0.85f ? 3 : 4)); // Crate..Heavy
    } else {
      kind = (r < 0.30f) ? 3 : (r < 0.70f ? 4 : 5); // Container / Heavy / Anvil
    }
    kind = std::clamp(kind, 0, kinds - 1);
    cargo.push_back({x, floor_gy * CELL, kind});
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
