// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "render.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// --- Pixel font: 3x5 glyphs, rows top→bottom, '1' = lit ---------------------
constexpr int FONT_SCALE = 3;
constexpr int GLYPH_W = 3 * FONT_SCALE, FONT_CELL_W = GLYPH_W + FONT_SCALE;
constexpr int FONT_CELL_H = 5 * FONT_SCALE;
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

// World chunks: 30x30 cells; divides the 24000 x 4800 world exactly (50 x 10 chunks)
constexpr int CHUNK_CELLS = 30;
constexpr int CHUNK = CHUNK_CELLS * TILE;
constexpr int CHUNKS_X = static_cast<int>(Cave::WORLD_W) / CHUNK, CHUNKS_Y = static_cast<int>(Cave::WORLD_H) / CHUNK;

constexpr int MM_DOWNSCALE = 4;
constexpr int MM_W = Cave::GW / MM_DOWNSCALE, MM_H = Cave::GH / MM_DOWNSCALE;

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

void Gfx::build_minimap(const Cave& cave) {
  be_->destroy_texture(minimap_);
  std::vector<uint32_t> px(static_cast<size_t>(MM_W) * MM_H);
  for (int my = 0; my < MM_H; ++my)
    for (int mx = 0; mx < MM_W; ++mx) {
      int n = 0;
      for (int dy = 0; dy < MM_DOWNSCALE; ++dy)
        for (int dx = 0; dx < MM_DOWNSCALE; ++dx)
          n += cave.solid[(my * MM_DOWNSCALE + dy) * Cave::GW + mx * MM_DOWNSCALE + dx];
      px[my * MM_W + mx] = pack(with_alpha(pal::MID, static_cast<uint8_t>(n * 150 / (MM_DOWNSCALE * MM_DOWNSCALE))));
    }
  minimap_ = be_->create_texture(MM_W, MM_H, reinterpret_cast<const uint8_t*>(px.data()), false);
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

// Text as horizontal pixel runs; `scale` multiplies the font size (the title logo uses big letters)
void Gfx::text(int x, int y, const char* s, Rgba c, int scale) const {
  static thread_local std::vector<SDL_Rect> rects;
  rects.clear();
  const int fs = FONT_SCALE * scale;
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

int Gfx::text_width(const char* s, int scale) const { return static_cast<int>(std::strlen(s)) * FONT_CELL_W * scale; }

int Gfx::sx(const Camera& cam, float wx) const {
  return static_cast<int>(std::lround(static_cast<double>(cam.continuous_x(wx)) * scale_)) - view_.ox;
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
    for (float shift : {0.f, -Cave::WORLD_W, Cave::WORLD_W}) {  // stars straddling the X seam
      const int sx = P(st.x + shift) - x0;
      if (sx < -sz || sx > tex_w + sz) continue;
      cv.rect(sx - sz / 2, sy - sz / 2, sz, sz, with_alpha(pal::STAR, 200));
      if (sz >= 4) cv.rect(sx - sz / 2 + 1, sy - sz / 2 + 1, sz - 2, sz - 2, with_alpha(pal::BRIGHT, 180));
    }
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
  const int ucx0 = static_cast<int>(std::floor(g.cam.x / CHUNK));
  const int ucx1 = static_cast<int>(std::floor((g.cam.x + vw_world) / CHUNK));
  const int cy0 = std::max(0, static_cast<int>(std::floor(g.cam.y / CHUNK)));
  const int cy1 = std::min(CHUNKS_Y - 1, static_cast<int>(std::floor((g.cam.y + vh_world) / CHUNK)));
  const int visible = (ucx1 - ucx0 + 1) * std::max(0, cy1 - cy0 + 1);
  const int max_cached = std::max(12, visible + 6);

  auto edge_x = [&](int u) { return static_cast<int>(std::lround(static_cast<double>(u) * CHUNK * scale_)) - view_.ox; };
  auto edge_y = [&](int v) { return static_cast<int>(std::lround(static_cast<double>(v) * CHUNK * scale_)) - view_.oy; };

  for (int cy = cy0; cy <= cy1; ++cy)
    for (int ucx = ucx0; ucx <= ucx1; ++ucx) {
      const int cx = ((ucx % CHUNKS_X) + CHUNKS_X) % CHUNKS_X;
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
  const float hw = d.half_w * 0.85f, hh = d.half_h * 0.9f, ox = d.engine_offset_x, ey = d.eng_y();
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

  const float nose_y = -hh, cabin_y = -hh * 0.35f, foot = hh * 0.85f;
  float belly = top ? hh * 0.35f : std::min(std::abs(ey) - 6.f, hh * 0.45f);
  if (belly < cabin_y + 8.f) belly = cabin_y + 12.f;

  // Flames first so the hull draws over them
  if (fl.state == FlightState::Flying) {
    be_->set_blend(Blend::Add);
    for (int i = 0; i < 2; ++i) {
      float lvl = th.level[i];
      if (lvl < 0.05f) continue;
      float lx = i == 0 ? -ox : ox;
      float len = (10.f + 46.f * lvl) * (0.8f + 0.4f * flicker(t, i * 1.7f));
      const float wd = top ? 8.f : 7.f;
      float y0 = d.nozzle_y();
      auto tri = [&](float half_w, float length, Rgba base, Rgba tip) {
        be_->gradient_triangle(W({lx - half_w, y0}), W({lx + half_w, y0}), W({lx, y0 + length}), base, tip);
      };
      tri(wd, len, with_alpha(pal::FLAME_EDGE, 220), with_alpha(pal::FLAME_EDGE, 0));
      tri(wd * 0.5f, len * 0.6f, with_alpha(pal::FLAME_CORE, 255), with_alpha(pal::FLAME_CORE, 0));
    }
    be_->set_blend(Blend::Alpha);
  }

  // Dark hull fill so rock and stars don't show through (convex fan around the nose)
  {
    const Vec2 outline_pts[] = {{0.f, nose_y}, {hw * 0.45f, cabin_y}, {hw * 0.55f, belly}, {hw * 0.9f, foot},
                                {-hw * 0.9f, foot}, {-hw * 0.55f, belly}, {-hw * 0.45f, cabin_y}};
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
  seg({-hw * 0.45f, cabin_y}, {-hw * 0.55f, belly});
  seg({hw * 0.45f, cabin_y}, {hw * 0.55f, belly});
  seg({-hw * 0.55f, belly}, {hw * 0.55f, belly});
  // Landing feet
  seg({-hw * 0.55f, belly}, {-hw * 0.9f, foot});
  seg({hw * 0.55f, belly}, {hw * 0.9f, foot});
  seg({-hw * 0.9f, foot}, {-hw * 0.7f, foot});
  seg({hw * 0.7f, foot}, {hw * 0.9f, foot});
  // Engine bells: the open end points ground-ward so top mounts still fire "down"
  for (float side : {-1.f, 1.f}) {
    float ex = side * ox;
    float back = top ? ey + 2.f : ey - 2.f, rim = top ? ey + 16.f : ey + 12.f;
    float bw = top ? 8.f : 7.f, rw = top ? 11.f : 10.f;
    seg({ex - bw, back}, {ex + bw, back});
    seg({ex - bw, back}, {ex - rw, rim});
    seg({ex + bw, back}, {ex + rw, rim});
    seg({ex - rw, rim}, {ex + rw, rim});
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
void Gfx::draw_hud(const Game& g, const UiState& ui) const {
  const bool pad = ui.device == InputDevice::Gamepad;  // hints name the device in use
  const Transform& tf = g.ecs.get<Transform>(g.ship);
  const Motion& m = g.ecs.get<Motion>(g.ship);
  const ShipDef& d = *g.ecs.get<Hull>(g.ship).def;
  const Flight& fl = g.ecs.get<Flight>(g.ship);
  const Thrusters& th = g.ecs.get<Thrusters>(g.ship);
  const int lh = FONT_CELL_H + 4;

  fill(8, 8, 210, 18 * 2 + 4 + lh * 4 + 28, with_alpha(pal::MENU, 120));
  int y = 16;
  for (int i = 0; i < 2; ++i) {
    fill(20, y, 120, 10, pal::DIM);
    float v = clampf(th.level[i], 0.f, 1.f);
    fill(20, y, static_cast<int>(120 * v), 10, mix(pal::MID, pal::HOT, smoothstep((v - 0.6f) / 0.4f)));
    outline(20, y, 120, 10, with_alpha(pal::MID, 120));
    text(148, y - 3, i == 0 ? "L" : "R", pal::MID);
    y += 18;
  }
  y += 4;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s%s", d.name, d.engines_top ? " TOP" : "");
  text(20, y, buf, pal::BRIGHT);
  y += lh;
  text(20, y, pad ? "START MENU" : "ESC MENU", pal::MID);
  y += lh + 4;

  const float alt = g.cave.floor_below(tf.pos.x, tf.pos.y) - tf.pos.y;
  std::snprintf(buf, sizeof(buf), "ALT %.0f", alt);
  text(20, y, buf, pal::BRIGHT);
  y += lh;
  std::snprintf(buf, sizeof(buf), "VX %.0f VY %.0f", m.vel.x, m.vel.y);
  text(20, y, buf, pal::MID);

  const bool on_pad = g.cave.pad_below(tf.pos.x, tf.pos.y, 140.f) != nullptr;
  const bool ok_v = m.vel.y < tune::LAND_MAX_VY && std::abs(m.vel.x) < tune::LAND_MAX_VX;
  const bool ok_a = std::abs(tf.angle) < tune::LAND_MAX_ANGLE;
  const struct { const char* s; bool ok; } status[] = {
      {on_pad ? "PAD OK" : "NO PAD", on_pad}, {ok_v ? "SPEED OK" : "SPEED HI", ok_v}, {ok_a ? "ATT OK" : "ATT BAD", ok_a}};
  const int rx = w_ - text_width("SPEED OK") - 24;
  fill(rx - 12, 8, w_ - rx + 4, lh * 3 + 16, with_alpha(pal::MENU, 120));
  for (int i = 0; i < 3; ++i) text(rx, 16 + lh * i, status[i].s, status[i].ok ? pal::PAD : pal::HOT);

  if (fl.state != FlightState::Flying) {
    const bool landed = fl.state == FlightState::Landed;
    float pulse = 0.65f + 0.35f * std::sin(fl.timer * 10.f);
    Rgba c = landed ? pal::PAD : pal::HOT;
    text_centered(w_ / 2, 48, landed ? "LANDED" : "CRASH", with_alpha(c, static_cast<uint8_t>(255 * pulse)));
    text_centered(w_ / 2, 48 + lh, landed ? "THRUST TO LIFT OFF" : (pad ? "A TO RESPAWN" : "ENTER TO RESPAWN"), pal::MID);
  }
}

void Gfx::draw_minimap(const Game& g, double t) {
  if (!minimap_ || minimap_generation_ != g.cave.generation) build_minimap(g.cave);
  const int x = (w_ - MM_W) / 2, y = h_ - MM_H - 14;
  fill(x - 4, y - 4, MM_W + 8, MM_H + 8, with_alpha(pal::BG, 170));
  be_->copy(minimap_, SDL_Rect{x, y, MM_W, MM_H}, {255, 255, 255, 255});
  outline(x - 4, y - 4, MM_W + 8, MM_H + 8, with_alpha(pal::MID, 140));

  const float k = MM_W / Cave::WORLD_W;
  for (const LandingPad& p : g.cave.pads)  // 5x5 dots: easy to see on a handheld too
    fill(x + static_cast<int>(0.5f * (p.x0 + p.x1) * k) - 2, y + static_cast<int>(p.y * k) - 2, 5, 5, pal::WARN);
  // Viewport box (drawn twice when it wraps the seam)
  const float vx = Cave::wrap_x(g.cam.x) * k;
  const int vw = static_cast<int>(g.cam.vw * k), vh = static_cast<int>(g.cam.vh * k);
  const int vy = y + static_cast<int>(clampf(g.cam.y, 0.f, Cave::WORLD_H) * k);
  for (float off : {0.f, -static_cast<float>(MM_W)}) {
    int bx = x + static_cast<int>(vx + off);
    if (bx + vw < x || bx > x + MM_W) continue;
    outline(bx, vy, vw, vh, with_alpha(pal::BRIGHT, 120));
  }
  const Transform& tf = g.ecs.get<Transform>(g.ship);
  if (std::fmod(t, 0.6) < 0.35)
    fill(x + static_cast<int>(Cave::wrap_x(tf.pos.x) * k) - 2, y + static_cast<int>(tf.pos.y * k) - 2, 5, 5, pal::HOT);
}

// The value shown beside a choice or slider item
void Gfx::item_value(const MenuItem& item, const Game& g, const UiState& ui, char* buf, size_t n) const {
  auto on_off = [&](bool v) { std::snprintf(buf, n, "%s", v ? "ON" : "OFF"); };
  switch (item.action) {
    case MenuAction::Ship: std::snprintf(buf, n, "%s", g.ecs.get<Hull>(g.ship).def->name); break;
    case MenuAction::Zoom: std::snprintf(buf, n, "%s", ZOOM_LEVELS[g.cam.zoom].name); break;
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
void Gfx::draw_menu(const Game& g, const UiState& ui) const {
  const MenuPageDef& page = page_def(ui.page);
  const int lh = FONT_CELL_H + 9;
  const int panel_w = std::min(w_ - 20, 520), panel_h = 78 + page.count * lh + 44;
  const int px = w_ / 2 - panel_w / 2, py = std::max(8, h_ / 2 - panel_h / 2);
  fill(0, 0, w_, h_, with_alpha(pal::BG, 120));
  fill(px, py, panel_w, panel_h, pal::MENU);
  outline(px, py, panel_w, panel_h, pal::BRIGHT);
  outline(px + 3, py + 3, panel_w - 6, panel_h - 6, with_alpha(pal::MID, 90));

  text_centered(w_ / 2, py + 18, page.title, pal::BRIGHT);
  fill(px + 24, py + 18 + FONT_CELL_H + 8, panel_w - 48, 1, with_alpha(pal::MID, 140));

  const int row_y = py + 18 + FONT_CELL_H + 22;
  for (int i = 0; i < page.count; ++i) {
    const MenuItem& item = page.items[i];
    const bool sel = i == ui.cursor;
    const int lx = px + 36, ly = row_y + i * lh, rx = px + panel_w - 36;
    if (sel) {
      fill(px + 12, ly - 4, panel_w - 24, FONT_CELL_H + 8, with_alpha(pal::MID, 40));
      if (std::fmod(ui.time, 0.8) < 0.55) text(lx - FONT_CELL_W - 4, ly, ">", pal::WARN);
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
      const int cell = 9, gap = 3, bars_w = SLIDER_MAX * (cell + gap) - gap;
      for (int c = 0; c < SLIDER_MAX; ++c)
        fill(rx - bars_w + c * (cell + gap), ly, cell, FONT_CELL_H, c < v ? col : with_alpha(pal::DIM, 255));
    }
  }
  text_centered(w_ / 2, py + panel_h - FONT_CELL_H - 14, menu_hint(ui, ui.page == MenuPage::Options), pal::MID);
}

// Title screen: the cave drifts behind a glowing logo; the title page is plain centred text
void Gfx::draw_title(const Game& g, const UiState& ui) const {
  (void)g;
  fill(0, 0, w_, h_, with_alpha(pal::BG, 140));
  // Logo letter scale: about 70% of the width, never taller than a seventh of the screen
  const int S = std::clamp(std::min(static_cast<int>(w_ * 0.7f) / (10 * FONT_CELL_W), h_ / 70), 2, 9);
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
  const int rule_y = ly + 5 * FONT_SCALE * S + 14;
  fill(lx, rule_y, text_width(logo, S) - FONT_SCALE * S, 2, with_alpha(pal::MID, 180));
  text_centered(w_ / 2, rule_y + 12, "CRT CAVE LANDER", pal::MID, 2);

  const MenuPageDef& page = page_def(ui.page);
  const int lh = FONT_CELL_H * 2 + 14;
  const int top = std::max(rule_y + 12 + FONT_CELL_H * 2 + 28, h_ * 11 / 20);
  for (int i = 0; i < page.count; ++i) {
    const bool sel = i == ui.cursor;
    const int y = top + i * lh;
    const char* label = page.items[i].label;
    text_centered(w_ / 2, y, label, sel ? pal::WARN : pal::MID, 2);
    if (sel && std::fmod(ui.time, 0.8) < 0.55) {
      const int half = text_width(label, 2) / 2 + 14;
      text(w_ / 2 - half - FONT_CELL_W * 2, y, ">", pal::WARN, 2);
      text(w_ / 2 + half, y, "<", pal::WARN, 2);
    }
  }
  text_centered(w_ / 2, h_ - FONT_CELL_H * 2 - 22, menu_hint(ui, false), pal::MID);
  char ver[64];
  std::snprintf(ver, sizeof ver, "V%s", APP_VERSION);
  text(12, h_ - FONT_CELL_H - 8, ver, with_alpha(pal::MID, 150));
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
void Gfx::draw(const Game& g, const UiState& ui) {
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
  draw_ship(g, t);

  const Flight& fl = g.ecs.get<Flight>(g.ship);
  if (!title && fl.state == FlightState::Crashed) {
    float flash = (0.5f + 0.5f * std::sin(fl.timer * 20.f)) * std::exp(-fl.timer * 1.5f);
    fill(0, 0, w_, h_, with_alpha(pal::HOT, static_cast<uint8_t>(8 + 100 * flash)));
  }
  if (!title) {
    draw_hud(g, ui);
    if (ui.toast_timer > 0.f && !ui.in_menu())  // short message above the minimap, fading out
      text_centered(w_ / 2, h_ - MM_H - 14 - FONT_CELL_H - 18, ui.toast,
                    with_alpha(pal::BRIGHT, static_cast<uint8_t>(255 * clampf(ui.toast_timer / 0.5f, 0.f, 1.f))));
    draw_minimap(g, t);
  }
  if (title && ui.page == MenuPage::Title) draw_title(g, ui);
  else if (ui.in_menu()) draw_menu(g, ui);

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
