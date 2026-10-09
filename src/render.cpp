// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "render.hpp"

#include <cmath>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// --- Pixel font: 3x5 glyphs, rows top→bottom, '1' = lit ---------------------
// Glyph size is `font_scale_` (from UI_SCALE_LEVELS); 2X uses 3 (the historical default).
constexpr int FIRST_CHAR = 32, CHAR_COUNT = 95;

struct Glyph {
  char ch;
  const char* bits;
};
constexpr Glyph GLYPHS[] = {
    {'0', "111101101101111"}, {'1', "010010010010010"}, {'2', "111001111100111"}, {'3', "111001111001111"},
    {'4', "101101111001001"}, {'5', "111100111001111"}, {'6', "111100111101111"}, {'7', "111001001001001"},
    {'8', "111101111101111"}, {'9', "111101111001111"}, {'A', "010101111101101"}, {'B', "110101110101110"},
    {'C', "011100100100011"}, {'D', "110101101101110"}, {'E', "111100110100111"}, {'F', "111100110100100"},
    {'G', "011100101101011"}, {'H', "101101111101101"}, {'I', "111010010010111"}, {'J', "001001001101010"},
    {'K', "101110110101101"}, {'L', "100100100100111"}, {'M', "101111111101101"}, {'N', "101111111111101"},
    {'O', "010101101101010"}, {'P', "110101110100100"}, {'Q', "010101101111011"}, {'R', "110101110101101"},
    {'S', "011100010001110"}, {'T', "111010010010010"}, {'U', "101101101101111"}, {'V', "101101101101010"},
    {'W', "101101111111101"}, {'X', "101101010101101"}, {'Y', "101101010010010"}, {'Z', "111001010100111"},
    {'-', "000000111000000"}, {'.', "000000000010010"}, {':', "000010000010000"}, {'>', "100010001010100"},
    {'<', "001010100010001"}, {'/', "001001010100100"}, {'!', "010010010000010"}, {'+', "000010111010000"},
    {'=', "000111000111000"}, {'?', "111001010000010"}, {'(', "010100100100010"}, {')', "010001001001010"},
    {',', "000000000010100"}, {'_', "000000000000111"}, {'%', "101001010100101"},
};

// --- Rock tiles: one per depth band; deeper rock is dimmer with a fainter hatch ----
struct RockBand {
  uint8_t base_alpha, hatch_alpha;
};
constexpr RockBand ROCK_BANDS[Cave::MAX_DEPTH] = {{160, 100}, {140, 70}, {115, 40}, {90, 18}, {65, 0}};
constexpr int TILE = static_cast<int>(Cave::CELL);

// World chunks: 30x30 cells; divides the 7680 x 5760 (4:3) world exactly (16 x 12 chunks)
constexpr int CHUNK_CELLS = 30;
constexpr int CHUNK = CHUNK_CELLS * TILE;
constexpr int CHUNKS_X = static_cast<int>(Cave::WORLD_W) / CHUNK, CHUNKS_Y = static_cast<int>(Cave::WORLD_H) / CHUNK;

// Minimap: one texel per cave cell; panel is a 4:3 window around the ship
constexpr int MM_W = 240, MM_H = 180;
constexpr float MM_K = 1.f / Cave::CELL;  // screen px per world px

uint32_t pack(Rgba c) {  // RGBA32 byte order is R,G,B,A in memory
  uint32_t v;
  const uint8_t b[4] = {c.r, c.g, c.b, c.a};
  std::memcpy(&v, b, 4);
  return v;
}

// Cheap smooth flicker in [0,1]
float flicker(double t, float seed) {
  return 0.5f + 0.25f * std::sin(static_cast<float>(t) * 53.f + seed) +
         0.25f * std::sin(static_cast<float>(t) * 97.f + seed * 2.3f);
}

// Straight-alpha RGBA canvas for baking chunk textures
struct Canvas {
  int w, h;
  uint8_t* px;

  void blend(int x, int y, Rgba c) {
    if (x < 0 || y < 0 || x >= w || y >= h || c.a == 0) return;
    uint8_t* p = px + (y * w + x) * 4;
    if (c.a == 255 || p[3] == 0) {
      p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
      return;
    }
    const int da = p[3] * (255 - c.a) / 255;
    const int oa = c.a + da;
    p[0] = static_cast<uint8_t>((c.r * c.a + p[0] * da) / oa);
    p[1] = static_cast<uint8_t>((c.g * c.a + p[1] * da) / oa);
    p[2] = static_cast<uint8_t>((c.b * c.a + p[2] * da) / oa);
    p[3] = static_cast<uint8_t>(oa);
  }
  void rect(int x0, int y0, int rw, int rh, Rgba c) {
    for (int y = y0; y < y0 + rh; ++y)
      for (int x = x0; x < x0 + rw; ++x) blend(x, y, c);
  }
  void line(int x0, int y0, int x1, int y1, Rgba c) {  // Bresenham, clipped per pixel
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    for (int err = dx + dy;; ) {
      blend(x0, y0, c);
      if (x0 == x1 && y0 == y1) break;
      int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }
  // Phosphor glow: faint halo on the minor axis, then the bright core
  void glow_line(int x0, int y0, int x1, int y1, Rgba c) {
    const Rgba halo = with_alpha(c, static_cast<uint8_t>(c.a * 70 / 255));
    if (std::abs(x1 - x0) >= std::abs(y1 - y0)) {
      line(x0, y0 - 1, x1, y1 - 1, halo);
      line(x0, y0 + 1, x1, y1 + 1, halo);
    } else {
      line(x0 - 1, y0, x1 - 1, y1, halo);
      line(x0 + 1, y0, x1 + 1, y1, halo);
    }
    line(x0, y0, x1, y1, c);
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
bool Gfx::init(std::unique_ptr<Backend> backend) {
  be_ = std::move(backend);
  for (const Glyph& g : GLYPHS) glyph_bits_[g.ch - FIRST_CHAR] = g.bits;
  resize();
  return be_ != nullptr;
}

void Gfx::shutdown() {
  if (!be_) return;
  for (Texture** t : {&overlay_, &minimap_}) {
    be_->destroy_texture(*t);
    *t = nullptr;
  }
  drop_chunks();
  chunks_.clear();
  be_.reset();
}

void Gfx::resize() {
  be_->output_size(w_, h_);
  w_ = std::max(w_, 320);
  h_ = std::max(h_, 240);
  build_overlay();
}

// CRT look: scanlines + vignette in one screen-sized black overlay
void Gfx::build_overlay() {
  be_->destroy_texture(overlay_);
  std::vector<uint32_t> px(static_cast<size_t>(w_) * h_);
  for (int y = 0; y < h_; ++y) {
    float ny = (y + 0.5f) / h_ * 2.f - 1.f;
    for (int x = 0; x < w_; ++x) {
      float nx = (x + 0.5f) / w_ * 2.f - 1.f;
      float r2 = (nx * nx + ny * ny) * 0.5f;                // 0 centre … 1 corner
      float vig = 0.55f * r2 * r2;                           // darken the edges
      float scan = (y % 3 == 0) ? 45.f / 255.f : 0.f;
      float a = 1.f - (1.f - vig) * (1.f - scan);
      px[y * w_ + x] = pack({0, 0, 0, static_cast<uint8_t>(clampf(a, 0.f, 1.f) * 255.f)});
    }
  }
  overlay_ = be_->create_texture(w_, h_, reinterpret_cast<const uint8_t*>(px.data()), false);
}

// Cheap integer hash for radio-static fog (stable per cell, no extra state).
uint32_t fog_hash(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

void Gfx::build_minimap(const Cave& cave, const std::vector<uint8_t>& revealed) {
  be_->destroy_texture(minimap_);
  std::vector<uint32_t> px(static_cast<size_t>(Cave::GW) * Cave::GH);
  const bool has_fog = revealed.size() == static_cast<size_t>(Cave::GW) * Cave::GH;
  for (int i = 0; i < Cave::GW * Cave::GH; ++i) {
    if (has_fog && !revealed[static_cast<size_t>(i)]) {
      // Unexplored: green-tinted radio noise (not flat black) so the chart reads as "unknown"
      const int gx = i % Cave::GW, gy = i / Cave::GW;
      const uint32_t h = fog_hash(static_cast<uint32_t>(gx * 73856093u) ^ static_cast<uint32_t>(gy * 19349663u));
      const uint8_t n = static_cast<uint8_t>(h & 255u);
      const uint8_t n2 = static_cast<uint8_t>((h >> 8) & 255u);
      // Dense static with occasional brighter flecks
      const uint8_t v = static_cast<uint8_t>(18 + (n % 50) + ((n2 & 7) == 0 ? 40 : 0));
      px[static_cast<size_t>(i)] = pack({static_cast<uint8_t>(v / 3), v, static_cast<uint8_t>(v / 2), 220});
      continue;
    }
    // Solid returns: green rock; explored open air is pure black (only unexplored is noise)
    if (cave.solid[static_cast<size_t>(i)])
      px[static_cast<size_t>(i)] = pack(with_alpha(pal::MID, 180));
    else
      px[static_cast<size_t>(i)] = pack({0, 0, 0, 255});
  }
  minimap_ = be_->create_texture(Cave::GW, Cave::GH, reinterpret_cast<const uint8_t*>(px.data()), false);
  minimap_generation_ = cave.generation;
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------
// Bright core with a faint halo on the minor axis — the phosphor glow
void Gfx::line(int x0, int y0, int x1, int y1, Rgba c) const {
  const Rgba halo = with_alpha(c, static_cast<uint8_t>(c.a * 70 / 255));
  if (std::abs(x1 - x0) >= std::abs(y1 - y0)) {
    be_->line(x0, y0 - 1, x1, y1 - 1, halo);
    be_->line(x0, y0 + 1, x1, y1 + 1, halo);
  } else {
    be_->line(x0 - 1, y0, x1 - 1, y1, halo);
    be_->line(x0 + 1, y0, x1 + 1, y1, halo);
  }
  be_->line(x0, y0, x1, y1, c);
}

void Gfx::fill(int x, int y, int w, int h, Rgba c) const {
  const SDL_Rect r{x, y, w, h};
  be_->fill_rects(&r, 1, c);
}

void Gfx::outline(int x, int y, int w, int h, Rgba c) const {
  const SDL_Rect r[4] = {{x, y, w, 1}, {x, y + h - 1, w, 1}, {x, y + 1, 1, h - 2}, {x + w - 1, y + 1, 1, h - 2}};
  be_->fill_rects(r, 4, c);
}

// Text as horizontal pixel runs; `scale` multiplies the UI font size (the title logo uses big letters)
void Gfx::text(int x, int y, const char* s, Rgba c, int scale) const {
  static thread_local std::vector<SDL_Rect> rects;
  rects.clear();
  const int fs = font_scale_ * scale;
  for (; *s; ++s, x += (3 * fs + fs)) {
    int ch = *s;
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch < FIRST_CHAR || ch >= FIRST_CHAR + CHAR_COUNT || !glyph_bits_[ch - FIRST_CHAR]) continue;
    const char* bits = glyph_bits_[ch - FIRST_CHAR];
    for (int row = 0; row < 5; ++row)
      for (int col = 0; col < 3;) {
        if (bits[row * 3 + col] != '1') { ++col; continue; }
        int run = 1;
        while (col + run < 3 && bits[row * 3 + col + run] == '1') ++run;
        rects.push_back({x + col * fs, y + row * fs, run * fs, fs});
        col += run;
      }
  }
  be_->fill_rects(rects.data(), static_cast<int>(rects.size()), c);
}

int Gfx::text_width(const char* s, int scale) const { return static_cast<int>(std::strlen(s)) * cell_w() * scale; }

int Gfx::sx(const Camera&, float wx) const {
  return static_cast<int>(std::lround(static_cast<double>(wx) * scale_)) - view_.ox;
}
int Gfx::sy(float wy) const { return static_cast<int>(std::lround(static_cast<double>(wy) * scale_)) - view_.oy; }
int Gfx::Z(float world_len) const { return std::max(1, static_cast<int>(std::lround(world_len * scale_))); }

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------
// Bake one chunk at scale `s` (screen px per world px): stars (open space only), rock fill and hatching by
// depth, marching-squares contour. Everything is placed in global pixel coordinates P(world) so neighbouring
// chunks and their line glow meet exactly. Samples sit at cell centres so the outline lies on the collision
// boundary. Geometry scales with the zoom; line widths stay one pixel.
void Gfx::render_chunk(const Cave& cave, int cx, int cy, std::vector<uint8_t>& px, int& tex_w, int& tex_h) const {
  const double s = bake_scale_;
  auto P = [s](double world) { return static_cast<int>(std::lround(world * s)); };
  const int x0 = P(cx * CHUNK), y0 = P(cy * CHUNK);  // global px of the chunk's top-left
  tex_w = P((cx + 1) * CHUNK) - x0;
  tex_h = P((cy + 1) * CHUNK) - y0;
  px.assign(static_cast<size_t>(tex_w) * tex_h * 4, 0);
  Canvas cv{tex_w, tex_h, px.data()};
  const int gx0 = cx * CHUNK_CELLS, gy0 = cy * CHUNK_CELLS;

  for (const Star& st : cave.stars) {
    const int sy = P(st.y) - y0;
    const int sz = std::max(1, static_cast<int>(std::lround(st.size * s)));
    if (sy < -sz || sy > tex_h + sz) continue;
    const int sx = P(st.x) - x0;
    if (sx < -sz || sx > tex_w + sz) continue;
    cv.rect(sx - sz / 2, sy - sz / 2, sz, sz, with_alpha(pal::STAR, 200));
    if (sz >= 4) cv.rect(sx - sz / 2 + 1, sy - sz / 2 + 1, sz - 2, sz - 2, with_alpha(pal::BRIGHT, 180));
  }

  // Rock: translucent fill plus diagonal hatching whose pitch scales with the zoom (a hatch is 4 world px)
  const int pitch = std::max(2, static_cast<int>(std::lround(4.0 * s)));
  const int thick = s >= 2.5 ? 2 : 1;
  for (int j = 0; j < CHUNK_CELLS; ++j)
    for (int i = 0; i < CHUNK_CELLS; ++i) {
      const int d = cave.depth_at(gx0 + i, gy0 + j);
      if (!d) continue;
      const RockBand& band = ROCK_BANDS[std::min(d, Cave::MAX_DEPTH) - 1];
      const Rgba base = with_alpha(pal::DIM, band.base_alpha), hatch = with_alpha(pal::MID, band.hatch_alpha);
      const int cx0 = P((gx0 + i) * Cave::CELL), cx1 = P((gx0 + i + 1) * Cave::CELL);
      const int cy0 = P((gy0 + j) * Cave::CELL), cy1 = P((gy0 + j + 1) * Cave::CELL);
      for (int y = cy0; y < cy1; ++y)
        for (int x = cx0; x < cx1; ++x) {
          const int m = ((x - y) % pitch + pitch) % pitch;  // global px: the hatch runs across chunk seams
          cv.blend(x - x0, y - y0, (m < thick && band.hatch_alpha) ? hatch : base);
        }
    }

  constexpr float H = Cave::CELL * 0.5f;
  static constexpr float EDGE[4][2] = {{H, 0.f}, {Cave::CELL, H}, {H, Cave::CELL}, {0.f, H}};  // edge midpoints
  for (int gy = std::max(gy0 - 1, 0); gy <= std::min(gy0 + CHUNK_CELLS, Cave::GH - 1); ++gy)
    for (int gx = gx0 - 1; gx <= gx0 + CHUNK_CELLS; ++gx) {  // neighbours' lines reach into this chunk
      const ContourCase& cc = CONTOUR_CASES[cave.contour_at(gx, gy)];
      const double ox = gx * Cave::CELL + H, oy = gy * Cave::CELL + H;
      for (const auto& sg : cc.seg) {
        if (sg[0] < 0) continue;
        cv.glow_line(P(ox + EDGE[sg[0]][0]) - x0, P(oy + EDGE[sg[0]][1]) - y0, P(ox + EDGE[sg[1]][0]) - x0,
                     P(oy + EDGE[sg[1]][1]) - y0, pal::BRIGHT);
      }
    }
}

void Gfx::drop_chunks() {
  for (Chunk& c : chunks_) be_->destroy_texture(c.tex);
  chunks_.assign(CHUNKS_X * CHUNKS_Y, Chunk{});
  chunk_count_ = 0;
}

void Gfx::draw_world(const Game& g) {
  const Cave& cave = g.cave;
  // A new cave or a new zoom level re-rasterises the chunks at that level's scale
  if (chunk_generation_ != cave.generation || std::abs(chunk_scale_ - bake_scale_) > 1e-4f) {
    drop_chunks();
    chunk_generation_ = cave.generation;
    chunk_scale_ = bake_scale_;
  }

  const float vw_world = w_ / scale_, vh_world = h_ / scale_;
  const int ucx0 = std::max(0, static_cast<int>(std::floor(g.cam.x / CHUNK)));
  const int ucx1 = std::min(CHUNKS_X - 1, static_cast<int>(std::floor((g.cam.x + vw_world) / CHUNK)));
  const int cy0 = std::max(0, static_cast<int>(std::floor(g.cam.y / CHUNK)));
  const int cy1 = std::min(CHUNKS_Y - 1, static_cast<int>(std::floor((g.cam.y + vh_world) / CHUNK)));
  const int visible = std::max(0, ucx1 - ucx0 + 1) * std::max(0, cy1 - cy0 + 1);
  const int max_cached = std::max(12, visible + 6);

  auto edge_x = [&](int u) { return static_cast<int>(std::lround(static_cast<double>(u) * CHUNK * scale_)) - view_.ox; };
  auto edge_y = [&](int v) { return static_cast<int>(std::lround(static_cast<double>(v) * CHUNK * scale_)) - view_.oy; };

  for (int cy = cy0; cy <= cy1; ++cy)
    for (int ucx = ucx0; ucx <= ucx1; ++ucx) {
      const int cx = ucx;
      Chunk& ch = chunks_[cy * CHUNKS_X + cx];
      if (!ch.tex) {
        int tw = 0, th = 0;
        render_chunk(cave, cx, cy, chunk_px_, tw, th);
        if (chunk_count_ >= max_cached) {  // evict the least recently used chunk not on screen
          Chunk* victim = nullptr;
          for (Chunk& c : chunks_)
            if (c.tex && c.last_used != frame_ && (!victim || c.last_used < victim->last_used)) victim = &c;
          if (victim) { be_->destroy_texture(victim->tex); victim->tex = nullptr; --chunk_count_; }
        }
        // Filtered: drawn 1:1 at rest, stretched a little while the zoom glides
        Texture* tex = be_->create_texture(tw, th, chunk_px_.data(), true);
        if (!tex) continue;
        ch.tex = tex;
        ++chunk_count_;
      }
      ch.last_used = frame_;
      const int dx = edge_x(ucx), dy = edge_y(cy);
      SDL_Rect dst{dx, dy, edge_x(ucx + 1) - dx, edge_y(cy + 1) - dy};
      be_->copy(ch.tex, dst, {255, 255, 255, 255});
    }
}

void Gfx::draw_pads(const Game& g, double t) const {
  const auto& pads = g.cave.pads;
  for (int pi = 0; pi < static_cast<int>(pads.size()); ++pi) {
    const LandingPad& p = pads[pi];
    const int x0 = sx(g.cam, p.x0);
    const int x1 = x0 + Z(p.x1 - p.x0);
    const int y = sy(p.y);
    if (x1 < -40 || x0 > w_ + 40 || y < -Z(80) || y > h_ + Z(40)) continue;
    const int w = std::max(1, x1 - x0), mid = (x0 + x1) / 2;
    const int z2 = Z(2), z4 = Z(4), z6 = Z(6);

    // Deck slab with cross-hatch
    fill(x0, y - z2, w, Z(5), with_alpha(pal::PAD, 200));
    outline(x0, y - z2, w, Z(5), pal::BRIGHT);
    for (int i = 0; i < w - z4; i += z6) be_->line(x0 + i, y - z2, x0 + i + z4, y + z2, with_alpha(pal::MID, 180));

    // Support struts into the rock
    const Rgba strut = with_alpha(pal::PAD, 160);
    be_->line(x0 + z4, y + Z(3), x0 + z4, y + Z(18), strut);
    be_->line(x1 - z4, y + Z(3), x1 - z4, y + Z(18), strut);
    be_->line(mid, y + Z(3), mid, y + Z(22), strut);
    be_->line(x0 + z4, y + Z(18), x1 - z4, y + Z(18), strut);

    // Approach chevrons, chasing upward
    const int chase = static_cast<int>(t * 3.0) % 3;
    for (int c = 0; c < 3; ++c) {
      const int cy = y - Z(14) - c * Z(10), inset = Z(8) + c * z6;
      const Rgba chev = with_alpha(pal::WARN, c == chase ? 255 : 110);
      be_->line(x0 + inset, cy, mid, cy + z6, chev);
      be_->line(x1 - inset, cy, mid, cy + z6, chev);
    }

    // Alternating end beacons with a soft halo
    const bool on = (static_cast<int>(t * 2.5) + pi) % 2 == 0;
    const Rgba beacon = on ? pal::HOT : pal::PAD;
    for (int bxp : {x0, x1}) {
      fill(bxp - Z(5), y - Z(9), Z(10), Z(10), with_alpha(beacon, on ? 50 : 20));
      fill(bxp - z2, y - z6, z4, z4, beacon);
    }

    // Centre T-mark
    be_->line(mid - Z(8), y, mid + Z(8), y, pal::BRIGHT);
    be_->line(mid, y - z6, mid, y + z2, pal::BRIGHT);
  }
}

// Additive sparks, batched into a few colour buckets by age (one FillRects call each)
void Gfx::draw_particles(const Game& g) const {
  constexpr int BUCKETS = 8;
  static thread_local std::vector<SDL_Rect> rects[BUCKETS];
  static thread_local Rgba colors[BUCKETS];
  for (auto& v : rects) v.clear();
  g.ecs.view<Particle, Transform>([&](Entity, const Particle& p, const Transform& t) {
    const int x = sx(g.cam, t.pos.x), y = sy(t.pos.y);
    if (x < -8 || x > w_ + 8 || y < -8 || y > h_ + 8) return;
    const float age = 1.f - p.life / p.ttl;
    const int b = std::clamp(static_cast<int>(age * BUCKETS), 0, BUCKETS - 1);
    colors[b] = mix(p.from, p.to, (b + 0.5f) / BUCKETS);
    const int s = Z(p.size * (0.4f + 0.6f * (1.f - age)));
    rects[b].push_back({x - s / 2, y - s / 2, s, s});
  });
  be_->set_blend(Blend::Add);
  for (int b = 0; b < BUCKETS; ++b)
    if (!rects[b].empty()) be_->fill_rects(rects[b].data(), static_cast<int>(rects[b].size()), colors[b]);
  be_->set_blend(Blend::Alpha);
}

void Gfx::draw_ship(const Game& g, double t) const {
  const Transform& tf = g.ecs.get<Transform>(g.ship);
  const ShipDef& d = *g.ecs.get<Hull>(g.ship).def;
  const Flight& fl = g.ecs.get<Flight>(g.ship);
  const Thrusters& th = g.ecs.get<Thrusters>(g.ship);
  const Legs& legs = g.ecs.get<Legs>(g.ship);
  const HullGeom hg = hull_geom(d);
  const LegGeom lg = leg_geom(d);
  const float hw = hg.hw, ox = d.engine_offset_x, ey = d.eng_y();
  const bool top = d.engines_top;

  auto W = [&](Vec2 l) {
    Vec2 w = tf.pos + rotate(l, tf.angle);
    return SDL_Point{sx(g.cam, w.x), sy(w.y)};
  };
  const Rgba body = fl.state == FlightState::Crashed ? pal::HOT
                    : fl.state == FlightState::Landed ? pal::PAD
                                                      : pal::BRIGHT;
  auto seg = [&](Vec2 a, Vec2 b) {
    SDL_Point pa = W(a), pb = W(b);
    line(pa.x, pa.y, pb.x, pb.y, body);
  };

  const float nose_y = hg.nose_y, cabin_y = hg.cabin_y, belly = hg.belly_y;

  // Flames first so the hull draws over them
  if (fl.state != FlightState::Crashed) {
    be_->set_blend(Blend::Add);
    for (int i = 0; i < thruster_count(d); ++i) {
      const ThrusterPose tp = thruster_pose(d, i);
      const float lvl = th.level[tp.channel] * tp.power;
      if (lvl < 0.05f) continue;
      const float len = (10.f + 46.f * lvl) * (0.8f + 0.4f * flicker(t, i * 1.7f));
      const float wd = d.thruster_n ? 4.f + 3.f * tp.power : (top ? 8.f : 7.f);
      const Vec2 perp{-tp.flame.y, tp.flame.x};
      auto tri = [&](float half_w, float length, Rgba base, Rgba tip) {
        be_->gradient_triangle(W(tp.nozzle - perp * half_w), W(tp.nozzle + perp * half_w), W(tp.nozzle + tp.flame * length), base, tip);
      };
      tri(wd, len, with_alpha(pal::FLAME_EDGE, 220), with_alpha(pal::FLAME_EDGE, 0));
      tri(wd * 0.5f, len * 0.6f, with_alpha(pal::FLAME_CORE, 255), with_alpha(pal::FLAME_CORE, 0));
    }
    be_->set_blend(Blend::Alpha);
  }

  // Dark hull fill so rock and stars don't show through (convex fan around the nose)
  {
    const Vec2 outline_pts[] = {{0.f, nose_y}, {hg.cabin_hw(), cabin_y}, {hg.belly_hw(), belly},
                                {-hg.belly_hw(), belly}, {-hg.cabin_hw(), cabin_y}};
    constexpr int N = sizeof(outline_pts) / sizeof(outline_pts[0]);
    SDL_Point p[N];
    for (int i = 0; i < N; ++i) p[i] = W(outline_pts[i]);
    be_->polygon(p, N, pal::HULL_FILL);
  }

  // Cabin / nose
  seg({-hw * 0.25f, cabin_y}, {0.f, nose_y});
  seg({hw * 0.25f, cabin_y}, {0.f, nose_y});
  seg({-hw * 0.25f, cabin_y}, {hw * 0.25f, cabin_y});
  seg({-hw * 0.12f, cabin_y + 4.f}, {hw * 0.12f, cabin_y + 4.f});
  // Hull
  seg({-hg.cabin_hw(), cabin_y}, {-hg.belly_hw(), belly});
  seg({hg.cabin_hw(), cabin_y}, {hg.belly_hw(), belly});
  seg({-hg.belly_hw(), belly}, {hg.belly_hw(), belly});
  // Hull panel lines with a gap in the middle, like riveted plates
  auto hull_half = [&](float y) { return lerpf(hg.cabin_hw(), hg.belly_hw(), (y - cabin_y) / (belly - cabin_y)); };
  for (float f : {0.38f, 0.72f}) {
    const float y = lerpf(cabin_y, belly, f), w = hull_half(y);
    seg({-w, y}, {-w * 0.25f, y});
    seg({w * 0.25f, y}, {w, y});
  }
  // Windows on the cabin; a canopy replaces them with a glass dome
  if (d.style & STYLE_DOME) {
    const float r = hw * 0.2f, cy = cabin_y + 5.f;
    Vec2 prev{-r, cy};
    for (int i = 1; i <= 6; ++i) {
      const float a = PI - PI * i / 6.f;
      const Vec2 p{r * std::cos(a), cy - r * 1.2f * std::sin(a)};
      seg(prev, p);
      prev = p;
    }
    seg({-r * 0.5f, cy - r * 0.55f}, {-r * 0.1f, cy - r * 0.95f});  // glint
  } else {
    for (float s : {-1.f, 1.f}) seg({s * hw * 0.08f, cabin_y + 8.f}, {s * hw * 0.2f, cabin_y + 8.f});
  }
  if (d.style & STYLE_FINS) {
    for (float s : {-1.f, 1.f}) {
      seg({s * hg.belly_hw(), belly - 11.f}, {s * (hg.belly_hw() + 11.f), belly + 3.f});
      seg({s * (hg.belly_hw() + 11.f), belly + 3.f}, {s * hg.belly_hw(), belly});
    }
  }
  if (d.style & STYLE_TANKS) {  // fuel tanks beside the cabin (they collide too)
    const float th = std::max(6.f, 0.5f * (belly - cabin_y) - 2.f), cy = 0.5f * (belly + cabin_y);
    for (float s : {-1.f, 1.f}) {
      const float x0 = s * (hg.cabin_hw() + 3.f), x1 = s * (hg.cabin_hw() + 15.f);
      seg({x0, cy - th}, {x1, cy - th});
      seg({x1, cy - th}, {x1, cy + th});
      seg({x1, cy + th}, {x0, cy + th});
      seg({x0, cy + th}, {x0, cy - th});
      seg({x0, cy - th * 0.3f}, {x1, cy - th * 0.3f});
      seg({x0, cy + th * 0.4f}, {x1, cy + th * 0.4f});
    }
  }
  if (d.style & STYLE_DISH) {  // radar dish on a stalk
    seg({hw * 0.22f, cabin_y + 1.f}, {hw * 0.42f, cabin_y - 9.f});
    seg({hw * 0.42f - 5.f, cabin_y - 12.f}, {hw * 0.42f, cabin_y - 9.f});
    seg({hw * 0.42f + 5.f, cabin_y - 6.f}, {hw * 0.42f, cabin_y - 9.f});
    seg({hw * 0.42f - 5.f, cabin_y - 12.f}, {hw * 0.42f + 5.f, cabin_y - 6.f});
  }
  if (d.style & STYLE_STRIPES) {  // hazard stripes along the belly
    const float w = hg.belly_hw() - 3.f;
    for (float x = -w; x < w - 3.f; x += 6.f) seg({x, belly}, {x + 3.f, belly - 4.f});
  }
  // Winch housing under the belly, with the cable eye
  seg({-5.f, belly}, {-5.f, belly + 5.f});
  seg({5.f, belly}, {5.f, belly + 5.f});
  seg({-5.f, belly + 5.f}, {5.f, belly + 5.f});
  // Landing legs: a hydraulic strut (sleeve on the hull, rod out to the foot plate) with a hinge and a foot
  for (int i = 0; i < 2; ++i) {
    const float side = i == 0 ? -1.f : 1.f;
    const Vec2 attach{side * lg.attach_x, lg.attach_y};
    const Vec2 ax{side * lg.axis_x, lg.axis_y}, nx{-ax.y, ax.x};
    const float reach = lg.length + legs.trans[i];
    const Vec2 foot = attach + ax * reach;
    const float sleeve = lg.length * 0.5f;  // the sleeve is fixed to the hull, the rod slides in it
    const Vec2 sleeve_end = attach + ax * sleeve;
    seg(attach + nx * 1.8f, sleeve_end + nx * 1.8f);
    seg(attach - nx * 1.8f, sleeve_end - nx * 1.8f);
    seg(sleeve_end + nx * 1.8f, sleeve_end - nx * 1.8f);
    seg(attach + ax * 2.f, foot);  // rod
    seg({foot.x - lg.foot_half_w, foot.y}, {foot.x + lg.foot_half_w, foot.y});
    seg({foot.x - lg.foot_half_w, foot.y}, {foot.x - lg.foot_half_w + 2.f, foot.y - 3.f});
    seg({foot.x + lg.foot_half_w, foot.y}, {foot.x + lg.foot_half_w - 2.f, foot.y - 3.f});
    seg(foot, {foot.x, foot.y - 4.f});
    seg({attach.x - 1.5f, attach.y - 1.5f}, {attach.x + 1.5f, attach.y + 1.5f});  // hinge
    seg({attach.x - 1.5f, attach.y + 1.5f}, {attach.x + 1.5f, attach.y - 1.5f});
  }
  if (d.style & STYLE_DECK) {  // the wide deck the outboard thrusters hang from
    seg({-hw, belly - 1.f}, {hw, belly - 1.f});
    seg({-hw, belly + 3.f}, {hw, belly + 3.f});
    seg({-hw, belly - 1.f}, {-hw, belly + 3.f});
    seg({hw, belly - 1.f}, {hw, belly + 3.f});
    for (float x = -hw + 8.f; x < hw - 4.f; x += 14.f) seg({x, belly - 1.f}, {x, belly + 3.f});  // ribs
  }
  if (d.thruster_n == 0) {
    // Engine bells: the open end points ground-ward so top mounts still fire "down"
    for (float side : {-1.f, 1.f}) {
      float ex = side * ox;
      float back = top ? ey + 2.f : ey - 2.f, rim = top ? ey + 16.f : ey + 12.f;
      float bw = top ? 8.f : 7.f, rw = top ? 11.f : 10.f;
      seg({ex - bw, back}, {ex + bw, back});
      seg({ex - bw, back}, {ex - rw, rim});
      seg({ex + bw, back}, {ex + rw, rim});
      seg({ex - rw, rim}, {ex + rw, rim});
      const float mid = 0.5f * (back + rim), mw = 0.5f * (bw + rw);  // cooling rib and nozzle throat
      seg({ex - mw, mid}, {ex + mw, mid});
      seg({ex - rw * 0.55f, rim - 2.f}, {ex + rw * 0.55f, rim - 2.f});
    }
  } else {
    for (int i = 0; i < d.thruster_n; ++i) {  // a bell along each thruster's exhaust
      const ThrusterPose tp = thruster_pose(d, i);
      const Vec2 f = tp.flame, p{-f.y, f.x};
      const float w0 = 5.f + 2.f * tp.power, w1 = 8.f + 2.f * tp.power;
      const Vec2 b0 = tp.pos - f * 2.f, b1 = tp.pos + f * 12.f, mid = tp.pos + f * 5.f;
      seg(b0 - p * w0, b0 + p * w0);
      seg(b0 - p * w0, b1 - p * w1);
      seg(b0 + p * w0, b1 + p * w1);
      seg(b1 - p * w1, b1 + p * w1);
      seg(mid - p * (0.5f * (w0 + w1)), mid + p * (0.5f * (w0 + w1)));  // cooling rib
      seg(b1 - p * w1 * 0.55f - f * 2.f, b1 + p * w1 * 0.55f - f * 2.f);
      // mounting pylon to the nearest hull edge or deck, so no thruster floats free
      {
        const float ys = clampf(b0.y, cabin_y, belly);
        Vec2 hull_pt{(b0.x < 0.f ? -1.f : 1.f) * hull_half(ys), ys};
        if (d.style & STYLE_DECK) {
          const Vec2 deck_pt{clampf(b0.x, -hw, hw), belly + 1.f};
          const Vec2 a = b0 - hull_pt, b = b0 - deck_pt;
          if (dot(b, b) < dot(a, a)) hull_pt = deck_pt;
        }
        if (length(b0 - hull_pt) > 3.f) seg(b0, hull_pt);
      }
      // control channel tick: one mark per stick-driven thruster so the pairs are told apart
      if (tp.channel >= 2) seg(tp.pos, tp.pos + f * 5.f);
    }
  }
  if (fl.state == FlightState::Flying) {  // nose marker
    seg({0.f, nose_y}, {0.f, nose_y - 10.f});
    seg({-4.f, nose_y - 6.f}, {0.f, nose_y - 10.f});
    seg({4.f, nose_y - 6.f}, {0.f, nose_y - 10.f});
  }
}

// ---------------------------------------------------------------------------
// HUD / UI
// ---------------------------------------------------------------------------
// Crates: outlined boxes with a strap and a cross, in the cargo colour
void Gfx::draw_cargo(const Game& g, double t) const {
  const float margin = 100.f;
  g.ecs.view<Cargo, Transform>([&](Entity e, const Cargo& c, const Transform& tf) {
    if (tf.pos.x < g.cam.x - margin || tf.pos.x > g.cam.x + g.cam.vw + margin || tf.pos.y < g.cam.y - margin ||
        tf.pos.y > g.cam.y + g.cam.vh + margin)
      return;
    const bool held = g.ecs.get<Rope>(g.ship).held == e;
    const float pulse = held ? 0.7f + 0.3f * std::sin(static_cast<float>(t) * 8.f) : 1.f;
    const Rgba col = mix(pal::DIM, pal::CARGO, pulse);
    const float hw = c.def->half_w, hh = c.def->half_h;
    auto W = [&](float lx, float ly) {
      const Vec2 w = tf.pos + rotate({lx, ly}, tf.angle);
      return SDL_Point{sx(g.cam, w.x), sy(w.y)};
    };
    const SDL_Point q[4] = {W(-hw, -hh), W(hw, -hh), W(hw, hh), W(-hw, hh)};
    be_->polygon(q, 4, pal::HULL_FILL);
    auto seg = [&](SDL_Point a, SDL_Point b, Rgba cc) { line(a.x, a.y, b.x, b.y, cc); };
    for (int i = 0; i < 4; ++i) seg(q[i], q[(i + 1) % 4], col);
    const Rgba dim = with_alpha(col, 150);
    seg(W(-hw, -hh), W(hw, hh), dim);
    seg(W(hw, -hh), W(-hw, hh), dim);
    seg(W(-hw * 0.5f, -hh), W(-hw * 0.5f, hh), dim);  // straps
    seg(W(hw * 0.5f, -hh), W(hw * 0.5f, hh), dim);
  });
}

// The cable (a chain of ticked links, sagging when slack) and the hook
void Gfx::draw_rope(const Game& g, double t) const {
  const Rope& r = g.ecs.get<Rope>(g.ship);
  const Flight& fl = g.ecs.get<Flight>(g.ship);
  const Rgba col = fl.state == FlightState::Crashed ? pal::HOT : pal::ROPE;
  const Vec2 a = r.anchor, h = r.hook_pos;
  const float dist = length(h - a);
  if (dist > 2.f) {
    const Vec2 ctrl = (a + h) * 0.5f + Vec2{0.f, clampf(r.slack * 0.6f, 0.f, 90.f)};  // quadratic sag
    const int n = std::max(3, static_cast<int>(dist / 8.f));
    Vec2 prev = a;
    for (int i = 1; i <= n; ++i) {
      const float u = static_cast<float>(i) / n, v = 1.f - u;
      const Vec2 p = a * (v * v) + ctrl * (2.f * u * v) + h * (u * u);
      const SDL_Point pa{sx(g.cam, prev.x), sy(prev.y)}, pb{sx(g.cam, p.x), sy(p.y)};
      line(pa.x, pa.y, pb.x, pb.y, with_alpha(col, i % 2 ? 255 : 170));
      // tick across the link: a chain, not a thread
      Vec2 d = p - prev;
      const float dl = std::max(length(d), 0.001f);
      const Vec2 nrm{-d.y / dl * 2.f, d.x / dl * 2.f};
      const SDL_Point t0{sx(g.cam, p.x - nrm.x), sy(p.y - nrm.y)}, t1{sx(g.cam, p.x + nrm.x), sy(p.y + nrm.y)};
      line(t0.x, t0.y, t1.x, t1.y, col);
      prev = p;
    }
  }
  // Hook: a ring with two prongs
  auto H = [&](float lx, float ly) {
    const Vec2 w = h + rotate({lx, ly}, r.hook_angle);
    return SDL_Point{sx(g.cam, w.x), sy(w.y)};
  };
  const Rgba hc = r.held != NULL_ENTITY ? pal::CARGO : pal::BRIGHT;
  SDL_Point ring[8];
  for (int i = 0; i < 8; ++i) ring[i] = H(4.f * std::cos(i * PI / 4.f), 4.f * std::sin(i * PI / 4.f) - 2.f);
  for (int i = 0; i < 8; ++i) line(ring[i].x, ring[i].y, ring[(i + 1) % 8].x, ring[(i + 1) % 8].y, hc);
  for (float s : {-1.f, 1.f}) {
    const SDL_Point p0 = H(s * 3.f, 2.f), p1 = H(s * 6.f, 8.f), p2 = H(s * 2.f, 11.f);
    line(p0.x, p0.y, p1.x, p1.y, hc);
    line(p1.x, p1.y, p2.x, p2.y, hc);
  }
  (void)t;
}

void Gfx::draw_hud(const Game& g, const UiState& ui, const BindMap& binds) const {
  const bool pad = ui.device == InputDevice::Gamepad;  // hints name the device in use
  const Transform& tf = g.ecs.get<Transform>(g.ship);
  const Motion& m = g.ecs.get<Motion>(g.ship);
  const ShipDef& d = *g.ecs.get<Hull>(g.ship).def;
  const Flight& fl = g.ecs.get<Flight>(g.ship);
  const Thrusters& th = g.ecs.get<Thrusters>(g.ship);
  const int lh = cell_h() + L(4);
  const int m8 = L(8), m16 = L(16), m20 = L(20), m24 = L(24);

  const int bars = channel_count(d);
  const int bar_h = L(10), bar_w = L(120), bar_step = L(18);
  fill(m8, m8, L(210), bar_step * (bars + 1) + L(4) + lh * 7 + L(28), with_alpha(pal::MENU, 120));
  int y = m16;
  for (int i = 0; i < bars; ++i) {
    fill(m20, y, bar_w, bar_h, pal::DIM);
    float v = clampf(th.level[i], 0.f, 1.f);
    fill(m20, y, static_cast<int>(bar_w * v), bar_h, mix(pal::MID, pal::HOT, smoothstep((v - 0.6f) / 0.4f)));
    outline(m20, y, bar_w, bar_h, with_alpha(pal::MID, 120));
    text(L(148), y - L(3), i == 0 ? "L" : i == 1 ? "R" : i == 2 ? "LS" : "RS", pal::MID);
    y += bar_step;
  }
  y += L(4);
  // Fuel gauge (drains with thrust, refills on pads)
  fill(m20, y, bar_w, bar_h, pal::DIM);
  const float fuel = clampf(fl.fuel, 0.f, 1.f);
  fill(m20, y, static_cast<int>(bar_w * fuel), bar_h,
       fuel > 0.25f ? pal::PAD : mix(pal::HOT, pal::WARN, fuel / 0.25f));
  outline(m20, y, bar_w, bar_h, with_alpha(pal::MID, 120));
  text(L(148), y - L(3), "FUEL", fuel > 0.25f ? pal::MID : pal::HOT);
  y += bar_step;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s%s", d.name, d.engines_top ? " TOP" : "");
  text(m20, y, buf, pal::BRIGHT);
  y += lh;
  std::snprintf(buf, sizeof(buf), "SCORE %d", g.score);
  text(m20, y, buf, pal::BRIGHT);
  y += lh;
  char bname[24];
  action_bind_label(binds, Action::Pause, !pad, bname, sizeof bname);
  char linebuf[48];
  std::snprintf(linebuf, sizeof linebuf, "%s MENU", bname);
  text(m20, y, linebuf, pal::MID);
  y += lh;
  const Legs& legs = g.ecs.get<Legs>(g.ship);
  action_bind_label(binds, Action::Legs, !pad, bname, sizeof bname);
  char legbuf[40];
  std::snprintf(legbuf, sizeof legbuf, "LEGS %s  %s", legs.deployed ? "OUT" : "IN", bname);
  text(m20, y, legbuf, legs.deployed ? pal::MID : pal::WARN);
  y += lh;
  const Rope& rp = g.ecs.get<Rope>(g.ship);
  action_bind_label(binds, Action::Grip, !pad, bname, sizeof bname);
  if (rp.held != NULL_ENTITY)
    std::snprintf(legbuf, sizeof legbuf, "LOAD %.1f  %s", g.ecs.get<Cargo>(rp.held).def->mass, bname);
  else
    std::snprintf(legbuf, sizeof legbuf, "HOOK %s  %s", rp.out ? "OUT" : "IN", bname);
  text(m20, y, legbuf, rp.held != NULL_ENTITY ? pal::CARGO : pal::MID);
  y += lh + L(4);

  const float alt = g.cave.floor_below(tf.pos.x, tf.pos.y) - tf.pos.y;
  std::snprintf(buf, sizeof(buf), "ALT %.0f", alt);
  text(m20, y, buf, pal::BRIGHT);
  y += lh;
  std::snprintf(buf, sizeof(buf), "VX %.0f VY %.0f", m.vel.x, m.vel.y);
  text(m20, y, buf, pal::MID);

  const bool on_pad = g.cave.pad_below(tf.pos.x, tf.pos.y, 140.f) != nullptr;
  const bool ok_v = m.vel.y < tune::LAND_MAX_VY && std::abs(m.vel.x) < tune::LAND_MAX_VX;
  const bool ok_a = std::abs(tf.angle) < tune::LAND_MAX_ANGLE;
  const struct { const char* s; bool ok; } status[] = {
      {on_pad ? "PAD OK" : "NO PAD", on_pad}, {ok_v ? "SPEED OK" : "SPEED HI", ok_v}, {ok_a ? "ATT OK" : "ATT BAD", ok_a}};
  const int rx = w_ - text_width("SPEED OK") - m24;
  fill(rx - L(12), m8, w_ - rx + L(4), lh * 3 + m16, with_alpha(pal::MENU, 120));
  for (int i = 0; i < 3; ++i) text(rx, m16 + lh * i, status[i].s, status[i].ok ? pal::PAD : pal::HOT);

  if (g.notice_timer > 0.f)
    text_centered(w_ / 2, L(48) + (fl.state != FlightState::Flying ? 2 * lh + L(8) : 0), g.notice,
                  with_alpha(pal::CARGO, static_cast<uint8_t>(255 * clampf(g.notice_timer / 0.5f, 0.f, 1.f))));
  if (fl.state != FlightState::Flying) {
    const bool landed = fl.state == FlightState::Landed;
    float pulse = 0.65f + 0.35f * std::sin(fl.timer * 10.f);
    Rgba c = landed ? pal::PAD : pal::HOT;
    text_centered(w_ / 2, L(48), landed ? "LANDED" : "CRASH", with_alpha(c, static_cast<uint8_t>(255 * pulse)));
    if (landed) {
      text_centered(w_ / 2, L(48) + lh, "THRUST TO LIFT OFF", pal::MID);
    } else {
      char rb[24];
      action_bind_label(binds, Action::Respawn, !pad, rb, sizeof rb);
      char msg[40];
      std::snprintf(msg, sizeof msg, "%s TO RESPAWN", rb);
      text_centered(w_ / 2, L(48) + lh, msg, pal::MID);
    }
  }
}

void Gfx::draw_minimap(Game& g, double t) {
  if (!minimap_ || minimap_generation_ != g.cave.generation || g.reveal_dirty) {
    build_minimap(g.cave, g.revealed);
    g.reveal_dirty = false;
  }
  // Source window stays MM_W x MM_H cells; the on-screen panel scales with the UI.
  const int mm_w = L(MM_W), mm_h = L(MM_H), pad = L(4);
  const int x = (w_ - mm_w) / 2, y = h_ - mm_h - L(14);
  fill(x - pad, y - pad, mm_w + 2 * pad, mm_h + 2 * pad, with_alpha(pal::BG, 170));

  // The window scrolls with the ship and stops at the map's edges
  const Transform& tf = g.ecs.get<Transform>(g.ship);
  const float ox = clampf(tf.pos.x * MM_K - MM_W * 0.5f, 0.f, static_cast<float>(Cave::GW - MM_W));
  const float oy = clampf(tf.pos.y * MM_K - MM_H * 0.5f, 0.f, static_cast<float>(Cave::GH - MM_H));
  const int ix = static_cast<int>(ox), iy = static_cast<int>(oy);
  be_->copy_part(minimap_, SDL_Rect{ix, iy, MM_W, MM_H}, SDL_Rect{x, y, mm_w, mm_h}, {255, 255, 255, 255});
  outline(x - pad, y - pad, mm_w + 2 * pad, mm_h + 2 * pad, with_alpha(pal::MID, 140));

  // Map coordinates (world px) -> panel px; dots that fall outside are skipped
  const float sx_k = static_cast<float>(mm_w) / static_cast<float>(MM_W);
  const float sy_k = static_cast<float>(mm_h) / static_cast<float>(MM_H);
  auto dot = [&](float wx, float wy, int size, Rgba c) {
    const int px = x + static_cast<int>((wx * MM_K - ix) * sx_k) - size / 2;
    const int py = y + static_cast<int>((wy * MM_K - iy) * sy_k) - size / 2;
    if (px < x || py < y || px + size > x + mm_w || py + size > y + mm_h) return;
    fill(px, py, size, size, c);
  };
  const int d_pad = L(5), d_cargo = L(4), d_ship = L(5);
  auto is_rev = [&](float wx, float wy) {
    const int gx = static_cast<int>(wx / Cave::CELL), gy = static_cast<int>(wy / Cave::CELL);
    if (!Cave::in_grid(gx, gy) || g.revealed.empty()) return true;
    return g.revealed[static_cast<size_t>(gy * Cave::GW + gx)] != 0;
  };
  for (const LandingPad& p : g.cave.pads) {
    const float cx = 0.5f * (p.x0 + p.x1);
    if (is_rev(cx, p.y)) dot(cx, p.y, d_pad, pal::WARN);
  }
  g.ecs.view<Cargo, Transform>([&](Entity, const Cargo&, const Transform& ct) {
    if (is_rev(ct.pos.x, ct.pos.y)) dot(ct.pos.x, ct.pos.y, d_cargo, pal::CARGO);
  });
  // Viewport box, clipped to the panel
  const int vw = static_cast<int>(g.cam.vw * MM_K * sx_k), vh = static_cast<int>(g.cam.vh * MM_K * sy_k);
  const int bx = x + static_cast<int>((g.cam.x * MM_K - ix) * sx_k);
  const int by = y + static_cast<int>((g.cam.y * MM_K - iy) * sy_k);
  const int cx0 = std::max(bx, x), cy0 = std::max(by, y), cx1 = std::min(bx + vw, x + mm_w), cy1 = std::min(by + vh, y + mm_h);
  if (cx1 > cx0 && cy1 > cy0) outline(cx0, cy0, cx1 - cx0, cy1 - cy0, with_alpha(pal::BRIGHT, 120));
  if (std::fmod(t, 0.6) < 0.35) dot(tf.pos.x, tf.pos.y, d_ship, pal::HOT);
}

// The value shown beside a choice or slider item
void Gfx::item_value(const MenuItem& item, const Game& g, const UiState& ui, char* buf, size_t n) const {
  auto on_off = [&](bool v) { std::snprintf(buf, n, "%s", v ? "ON" : "OFF"); };
  switch (item.action) {
    case MenuAction::Ship: std::snprintf(buf, n, "%s", g.ecs.get<Hull>(g.ship).def->name); break;
    case MenuAction::Zoom: std::snprintf(buf, n, "%s", ZOOM_LEVELS[g.cam.zoom].name); break;
    case MenuAction::UiScale: std::snprintf(buf, n, "%s", UI_SCALE_LEVELS[ui.ui_scale].name); break;
    case MenuAction::SwapEngines: on_off(ui.swap_engines); break;
    case MenuAction::Crt: on_off(ui.crt); break;
    case MenuAction::Fullscreen: on_off(ui.fullscreen); break;
    default: buf[0] = '\0';
  }
}

static const char* menu_hint(const UiState& ui, bool options) {
  if (ui.device == InputDevice::Gamepad) return options ? "DPAD MOVE CHANGE  A OK  B BACK" : "DPAD MOVE  A OK  B BACK";
  return options ? "ARROWS MOVE CHANGE  ENTER OK  ESC BACK" : "UP DOWN MOVE  ENTER OK  ESC BACK";
}

// A boxed menu page (pause, options) over the dimmed game
void Gfx::draw_menu(const Game& g, const UiState& ui, const BindMap& binds) const {
  if (ui.page == MenuPage::Controls) {
    const int lh = cell_h() + L(9);
    const int rows = ACTION_COUNT + item_count(CONTROL_ITEMS);
    const int panel_w = std::min(w_ - L(20), L(640));
    const int panel_h = L(78) + rows * lh + L(44);
    const int px = w_ / 2 - panel_w / 2, py = std::max(L(8), h_ / 2 - panel_h / 2);
    fill(0, 0, w_, h_, with_alpha(pal::BG, 120));
    fill(px, py, panel_w, panel_h, pal::MENU);
    outline(px, py, panel_w, panel_h, pal::BRIGHT);
    outline(px + L(3), py + L(3), panel_w - L(6), panel_h - L(6), with_alpha(pal::MID, 90));
    text_centered(w_ / 2, py + L(18), "CONTROLS", pal::BRIGHT);
    fill(px + L(24), py + L(18) + cell_h() + L(8), panel_w - L(48), 1, with_alpha(pal::MID, 140));

    const bool kbd = ui.device != InputDevice::Gamepad;
    int row_y = py + L(18) + cell_h() + L(22);
    char val[40];
    for (int i = 0; i < ACTION_COUNT; ++i) {
      const bool sel = ui.controls_cursor == i;
      const int ly = row_y + i * lh;
      if (sel) {
        fill(px + L(12), ly - L(4), panel_w - L(24), cell_h() + L(8), with_alpha(pal::MID, 40));
        if (!ui.rebinding && std::fmod(ui.time, 0.8) < 0.55)
          text(px + L(36) - cell_w() - L(4), ly, ">", pal::WARN);
      }
      const Rgba col = sel ? pal::WARN : pal::MID;
      text(px + L(36), ly, ACTION_INFO[i].label, col);
      if (ui.rebinding && sel) {
        std::snprintf(val, sizeof val, kbd ? "PRESS KEY..." : "PRESS BUTTON...");
        text(px + panel_w - L(36) - text_width(val), ly, val, pal::HOT);
      } else {
        action_bind_label(binds, static_cast<Action>(i), kbd, val, sizeof val);
        text(px + panel_w - L(36) - text_width(val), ly, val, col);
      }
    }
    row_y += ACTION_COUNT * lh + L(6);
    for (int i = 0; i < item_count(CONTROL_ITEMS); ++i) {
      const int idx = ACTION_COUNT + i;
      const bool sel = ui.controls_cursor == idx;
      const int ly = row_y + i * lh;
      if (sel) {
        fill(px + L(12), ly - L(4), panel_w - L(24), cell_h() + L(8), with_alpha(pal::MID, 40));
        if (std::fmod(ui.time, 0.8) < 0.55) text(px + L(36) - cell_w() - L(4), ly, ">", pal::WARN);
      }
      text(px + L(36), ly, CONTROL_ITEMS[i].label, sel ? pal::WARN : pal::MID);
    }
    const char* hint = ui.rebinding
                           ? (kbd ? "ESC CANCEL" : "START CANCEL")
                           : (kbd ? "ENTER REBIND  TAB DEVICE  ESC BACK" : "A REBIND  Y DEVICE  B BACK");
    text_centered(w_ / 2, py + panel_h - cell_h() - L(14), hint, pal::MID);
    return;
  }

  const MenuPageDef& page = page_def(ui.page);
  const int lh = cell_h() + L(9);
  const int stat_rows = ui.page == MenuPage::Stats ? STAT_FIELD_COUNT : 0;  // read-only lines above the items
  const int panel_w = std::min(w_ - L(20), L(520));
  const int panel_h = L(78) + (stat_rows + page.count) * lh + (stat_rows ? L(10) : 0) + L(44);
  const int px = w_ / 2 - panel_w / 2, py = std::max(L(8), h_ / 2 - panel_h / 2);
  fill(0, 0, w_, h_, with_alpha(pal::BG, 120));
  fill(px, py, panel_w, panel_h, pal::MENU);
  outline(px, py, panel_w, panel_h, pal::BRIGHT);
  outline(px + L(3), py + L(3), panel_w - L(6), panel_h - L(6), with_alpha(pal::MID, 90));

  text_centered(w_ / 2, py + L(18), page.title, pal::BRIGHT);
  fill(px + L(24), py + L(18) + cell_h() + L(8), panel_w - L(48), 1, with_alpha(pal::MID, 140));

  int row_y = py + L(18) + cell_h() + L(22);
  for (int i = 0; i < stat_rows; ++i, row_y += lh) {
    char val[32];
    format_stat(STAT_FIELDS[i], g.stats, val, sizeof val);
    text(px + L(36), row_y, STAT_FIELDS[i].label, pal::MID);
    text(px + panel_w - L(36) - text_width(val), row_y, val, pal::BRIGHT);
  }
  if (stat_rows) row_y += L(10);
  for (int i = 0; i < page.count; ++i) {
    const MenuItem& item = page.items[i];
    const bool sel = i == ui.cursor;
    const int lx = px + L(36), ly = row_y + i * lh, rx = px + panel_w - L(36);
    if (sel) {
      fill(px + L(12), ly - L(4), panel_w - L(24), cell_h() + L(8), with_alpha(pal::MID, 40));
      if (std::fmod(ui.time, 0.8) < 0.55) text(lx - cell_w() - L(4), ly, ">", pal::WARN);
    }
    const Rgba col = sel ? pal::WARN : pal::MID;
    text(lx, ly, item.label, col);
    if (item.kind == ItemKind::Choice) {
      char val[24];
      item_value(item, g, ui, val, sizeof val);
      char shown[32];
      std::snprintf(shown, sizeof shown, sel ? "< %s >" : "%s", val);
      text(rx - text_width(shown), ly, shown, col);
    } else if (item.kind == ItemKind::Slider) {
      const int v = item.action == MenuAction::Music ? ui.music_vol : ui.sfx_vol;
      const int cell = L(9), gap = L(3), bars_w = SLIDER_MAX * (cell + gap) - gap;
      for (int c = 0; c < SLIDER_MAX; ++c)
        fill(rx - bars_w + c * (cell + gap), ly, cell, cell_h(), c < v ? col : with_alpha(pal::DIM, 255));
    }
  }
  text_centered(w_ / 2, py + panel_h - cell_h() - L(14), menu_hint(ui, ui.page == MenuPage::Options), pal::MID);
}

// Title screen: the cave drifts behind a glowing logo; the title page is plain centred text
void Gfx::draw_title(const Game& g, const UiState& ui) const {
  (void)g;
  fill(0, 0, w_, h_, with_alpha(pal::BG, 140));
  // Logo letter scale: about 70% of the width, never taller than a seventh of the screen
  const int S = std::clamp(std::min(static_cast<int>(w_ * 0.7f) / (10 * cell_w()), h_ / 70), 2, 9);
  const char* logo = "DUALTHRUST";
  const int ly = h_ / 8;
  const int lx = w_ / 2 - text_width(logo, S) / 2;
  const float pulse = 0.75f + 0.25f * std::sin(static_cast<float>(ui.time) * 2.f);
  for (int o = 3; o >= 1; --o)  // phosphor glow: soft copies around the sharp letters
    for (int d = 0; d < 4; ++d) {
      const int ox = (d == 0 ? -o : d == 1 ? o : 0), oy = (d == 2 ? -o : d == 3 ? o : 0);
      text(lx + ox, ly + oy, logo, with_alpha(pal::MID, static_cast<uint8_t>(26 * pulse)), S);
    }
  text(lx, ly, logo, pal::BRIGHT, S);
  const int rule_y = ly + 5 * font_scale_ * S + L(14);
  fill(lx, rule_y, text_width(logo, S) - font_scale_ * S, L(2), with_alpha(pal::MID, 180));
  text_centered(w_ / 2, rule_y + L(12), "CRT CAVE LANDER", pal::MID, 2);

  const MenuPageDef& page = page_def(ui.page);
  const int lh = cell_h() * 2 + L(14);
  const int top = std::max(rule_y + L(12) + cell_h() * 2 + L(28), h_ * 11 / 20);
  for (int i = 0; i < page.count; ++i) {
    const bool sel = i == ui.cursor;
    const int y = top + i * lh;
    const char* label = page.items[i].label;
    text_centered(w_ / 2, y, label, sel ? pal::WARN : pal::MID, 2);
    if (sel && std::fmod(ui.time, 0.8) < 0.55) {
      const int half = text_width(label, 2) / 2 + L(14);
      text(w_ / 2 - half - cell_w() * 2, y, ">", pal::WARN, 2);
      text(w_ / 2 + half, y, "<", pal::WARN, 2);
    }
  }
  text_centered(w_ / 2, h_ - cell_h() * 2 - L(22), menu_hint(ui, false), pal::MID);
  char ver[64];
  std::snprintf(ver, sizeof ver, "V%s", APP_VERSION);
  text(L(12), h_ - cell_h() - L(8), ver, with_alpha(pal::MID, 150));
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void Gfx::draw_sonar(const Game& g) const {
  if (!g.sonar.active) return;
  const Vec2 o = g.sonar.origin;
  const float r = g.sonar.radius;
  if (r < 1.f) return;
  // Expanding phosphor ring in world space
  const int segments = std::clamp(static_cast<int>(r / 8.f), 24, 96);
  const float fade = 1.f - r / std::max(g.sonar.max_radius, 1.f);
  const Rgba col = with_alpha(pal::CARGO, static_cast<uint8_t>(40 + 160 * fade));
  const Rgba dim = with_alpha(pal::CARGO, static_cast<uint8_t>(20 + 80 * fade));
  SDL_Point prev{sx(g.cam, o.x + r), sy(o.y)};
  for (int i = 1; i <= segments; ++i) {
    const float a = (static_cast<float>(i) / segments) * 6.2831853f;
    const SDL_Point cur{sx(g.cam, o.x + r * std::cos(a)), sy(o.y + r * std::sin(a))};
    line(prev.x, prev.y, cur.x, cur.y, col);
    // faint second ring slightly inside
    if (r > 12.f) {
      const float r2 = r - 6.f;
      const SDL_Point a0{sx(g.cam, o.x + r2 * std::cos(a - 6.2831853f / segments)),
                         sy(o.y + r2 * std::sin(a - 6.2831853f / segments))};
      const SDL_Point a1{sx(g.cam, o.x + r2 * std::cos(a)), sy(o.y + r2 * std::sin(a))};
      line(a0.x, a0.y, a1.x, a1.y, dim);
    }
    prev = cur;
  }
}

void Gfx::draw_full_map(const Game& g, const UiState& ui) const {
  (void)ui;
  if (!minimap_) return;
  fill(0, 0, w_, h_, with_alpha(pal::BG, 220));
  // Fit the whole cave (GW x GH texels) into the screen with a margin
  const float margin = 0.08f;
  const float aw = w_ * (1.f - 2.f * margin), ah = h_ * (1.f - 2.f * margin);
  const float scale = std::min(aw / Cave::GW, ah / Cave::GH);
  const int dw = std::max(1, static_cast<int>(Cave::GW * scale));
  const int dh = std::max(1, static_cast<int>(Cave::GH * scale));
  const int dx = (w_ - dw) / 2, dy = (h_ - dh) / 2;
  fill(dx - L(4), dy - L(4), dw + L(8), dh + L(8), with_alpha(pal::MENU, 200));
  outline(dx - L(4), dy - L(4), dw + L(8), dh + L(8), pal::BRIGHT);
  be_->copy(minimap_, SDL_Rect{dx, dy, dw, dh}, {255, 255, 255, 255});
  // Ship
  const Transform& tf = g.ecs.get<Transform>(g.ship);
  const int sx = dx + static_cast<int>(tf.pos.x / Cave::CELL * scale);
  const int sy = dy + static_cast<int>(tf.pos.y / Cave::CELL * scale);
  const int sz = std::max(3, L(6));
  fill(sx - sz / 2, sy - sz / 2, sz, sz, pal::HOT);
  text_centered(w_ / 2, dy - cell_h() - L(10), "MAP", pal::BRIGHT);
  text_centered(w_ / 2, dy + dh + L(8), "HOLD TO VIEW", pal::MID);
}

void Gfx::draw(Game& g, const UiState& ui, const BindMap& binds) {
  const int si = std::clamp(ui.ui_scale, 0, UI_SCALE_COUNT - 1);
  font_scale_ = UI_SCALE_LEVELS[si].font;
  scale_ = h_ / g.cam.vh;  // screen px per world px; animates while the zoom level changes
  bake_scale_ = std::min(h_ / ZOOM_LEVELS[g.cam.zoom].visible_h, 3.f);  // chunk resolution: the level's own scale
  view_.ox = static_cast<int>(std::lround(g.cam.x * static_cast<double>(scale_) + g.cam.shake_off.x * scale_));
  view_.oy = static_cast<int>(std::lround(g.cam.y * static_cast<double>(scale_) + g.cam.shake_off.y * scale_));
  const double t = ui.time;
  const bool title = ui.screen == Screen::Title;

  be_->begin_frame(pal::BG);
  ++frame_;
  draw_world(g);
  draw_pads(g, t);
  draw_particles(g);
  draw_cargo(g, t);
  draw_rope(g, t);
  draw_ship(g, t);
  draw_sonar(g);

  const Flight& fl = g.ecs.get<Flight>(g.ship);
  if (!title && fl.state == FlightState::Crashed) {
    float flash = (0.5f + 0.5f * std::sin(fl.timer * 20.f)) * std::exp(-fl.timer * 1.5f);
    fill(0, 0, w_, h_, with_alpha(pal::HOT, static_cast<uint8_t>(8 + 100 * flash)));
  }
  if (!title) {
    draw_hud(g, ui, binds);
    if (ui.toast_timer > 0.f && !ui.in_menu())  // short message above the minimap, fading out
      text_centered(w_ / 2, h_ - L(MM_H) - L(14) - cell_h() - L(18), ui.toast,
                    with_alpha(pal::BRIGHT, static_cast<uint8_t>(255 * clampf(ui.toast_timer / 0.5f, 0.f, 1.f))));
    draw_minimap(g, t);
  }
  if (title && ui.page == MenuPage::Title) draw_title(g, ui);
  else if (ui.in_menu()) draw_menu(g, ui, binds);
  if (ui.show_map && !title) draw_full_map(g, ui);

  if (ui.crt) be_->copy(overlay_, SDL_Rect{0, 0, w_, h_}, {255, 255, 255, 255});
}

bool Gfx::save_screenshot(const char* path) const {
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
  if (!be_->read_pixels(rgba, w, h)) return false;
  SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(rgba.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
  if (!s) return false;
  const bool ok = SDL_SaveBMP(s, path) == 0;
  SDL_FreeSurface(s);
  return ok;
}
