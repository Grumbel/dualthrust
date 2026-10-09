// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "render.hpp"
#include "debug.hpp"
#include "systems.hpp"

#include <cmath>

#include <cstdio>
#include <cstring>
#include <vector>
#include <utility>
#include <algorithm>

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

// Minimap: one texel per cave cell; on-screen panel is fixed, source window zooms.
constexpr int MM_ZOOM_W[] = {120, 240, 480};  // cells wide: near / mid / far
constexpr int MM_ZOOM_H[] = {90, 180, 360};
constexpr int MM_ZOOM_COUNT = 3;
constexpr int MM_W = 240, MM_H = 180;  // default mid + panel aspect reference
constexpr float MM_K = 1.f / Cave::CELL;  // cells per world px

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
  const int ncells = Cave::GW * Cave::GH;
  std::vector<uint32_t> px(static_cast<size_t>(ncells));
  const bool has_fog = revealed.size() == static_cast<size_t>(ncells);
  auto solid_at = [&](int gx, int gy) {
    return Cave::in_grid(gx, gy) && cave.solid[static_cast<size_t>(gy * Cave::GW + gx)] != 0;
  };
  auto noise_at = [&](int gx, int gy) -> uint32_t {
    const uint32_t h = fog_hash(static_cast<uint32_t>(gx * 73856093u) ^ static_cast<uint32_t>(gy * 19349663u));
    const uint8_t n = static_cast<uint8_t>(h & 255u);
    const uint8_t n2 = static_cast<uint8_t>((h >> 8) & 255u);
    const uint8_t v = static_cast<uint8_t>(18 + (n % 50) + ((n2 & 7) == 0 ? 40 : 0));
    return pack({static_cast<uint8_t>(v / 3), v, static_cast<uint8_t>(v / 2), 220});
  };
  auto lerp_u8 = [](uint8_t a, uint8_t b, float t) -> uint8_t {
    return static_cast<uint8_t>(a + (b - a) * t);
  };
  for (int i = 0; i < ncells; ++i) {
    const int gx = i % Cave::GW, gy = i / Cave::GW;
    const uint8_t str = has_fog ? revealed[static_cast<size_t>(i)] : static_cast<uint8_t>(255);
    if (str == 0) {
      px[static_cast<size_t>(i)] = noise_at(gx, gy);
      continue;
    }
    // Fully revealed look
    uint8_t fr, fg, fb;
    if (cave.solid[static_cast<size_t>(i)]) {
      fr = 20; fg = 90; fb = 40;  // rock fill
    } else {
      fr = 0; fg = 0; fb = 0;  // open void
    }
    if (str >= 250) {
      px[static_cast<size_t>(i)] = pack({fr, fg, fb, 255});
    } else {
      // Outer fade band: blend revealed into radio noise by strength
      const float t = str / 255.f;
      const uint32_t nh = fog_hash(static_cast<uint32_t>(gx * 73856093u) ^ static_cast<uint32_t>(gy * 19349663u));
      const uint8_t nv = static_cast<uint8_t>(18 + ((nh & 255u) % 50));
      px[static_cast<size_t>(i)] = pack({
          lerp_u8(static_cast<uint8_t>(nv / 3), fr, t),
          lerp_u8(nv, fg, t),
          lerp_u8(static_cast<uint8_t>(nv / 2), fb, t),
          255});
    }
  }
  // Second pass: thick high-contrast edge on well-revealed surface rock
  const uint32_t edge_hi = pack({160, 255, 180, 255});
  const uint32_t edge_md = pack({80, 200, 110, 255});
  for (int gy = 0; gy < Cave::GH; ++gy) {
    for (int gx = 0; gx < Cave::GW; ++gx) {
      const uint8_t str = has_fog ? revealed[static_cast<size_t>(gy * Cave::GW + gx)] : static_cast<uint8_t>(255);
      if (str < 100) continue;  // too faint for a hard edge
      if (!solid_at(gx, gy)) continue;
      bool surface = false;
      for (int oy = -1; oy <= 1 && !surface; ++oy)
        for (int ox = -1; ox <= 1 && !surface; ++ox)
          if ((ox || oy) && !solid_at(gx + ox, gy + oy)) surface = true;
      if (!surface) continue;
      // pack is R,G,B,A in memory - careful: our pack() order
      if (str >= 250)
        px[static_cast<size_t>(gy * Cave::GW + gx)] = edge_hi;
      else {
        const float t = str / 255.f;
        const uint8_t eg = static_cast<uint8_t>(40 + 215 * t);
        px[static_cast<size_t>(gy * Cave::GW + gx)] = pack({static_cast<uint8_t>(eg * 2 / 3), eg, static_cast<uint8_t>(eg * 3 / 4), 255});
      }
      for (int oy = -1; oy <= 1; ++oy)
        for (int ox = -1; ox <= 1; ++ox) {
          if (!ox && !oy) continue;
          const int nx = gx + ox, ny = gy + oy;
          if (!Cave::in_grid(nx, ny) || solid_at(nx, ny)) continue;
          const uint8_t ns = has_fog ? revealed[static_cast<size_t>(ny * Cave::GW + nx)] : static_cast<uint8_t>(255);
          if (ns < 80) continue;
          px[static_cast<size_t>(ny * Cave::GW + nx)] = edge_md;
        }
    }
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
  // Preferred delivery pad while hauling — highlighted, and drawn even if not yet activated
  int dest_pi = -1;
  if (g.ship != NULL_ENTITY && g.ecs.has<Rope>(g.ship)) {
    const Rope& rp = g.ecs.get<Rope>(g.ship);
    if (rp.held != NULL_ENTITY && g.ecs.has<Cargo>(rp.held))
      dest_pi = g.ecs.get<Cargo>(rp.held).dest_pad;
  }

  for (int pi = 0; pi < static_cast<int>(pads.size()); ++pi) {
    const LandingPad& p = pads[pi];
    const bool is_dest = (pi == dest_pi);
    if (!p.active && !is_dest) continue;  // sonar lights pads; dest ghosts still show while hauling
    const int x0 = sx(g.cam, p.x0);
    const int x1 = x0 + Z(p.x1 - p.x0);
    const int y = sy(p.y);
    if (x1 < -40 || x0 > w_ + 40 || y < -Z(80) || y > h_ + Z(40)) continue;
    const int w = std::max(1, x1 - x0), mid = (x0 + x1) / 2;
    const int z2 = Z(2), z4 = Z(4), z6 = Z(6);
    const bool ghost = !p.active && is_dest;
    const bool online = p.active;  // sonar-activated (or landed-discovered)
    const float pulse = 0.55f + 0.45f * static_cast<float>(0.5 + 0.5 * std::sin(t * 6.0));
    const float pulse_fast = 0.5f + 0.5f * static_cast<float>(0.5 + 0.5 * std::sin(t * 10.0));

    // Deck colour: dest = hot, online = bright pad green, ghost = dim
    const Rgba deck_col = is_dest ? pal::HOT : (online ? pal::PAD : pal::DIM);
    const Rgba edge_col = is_dest ? pal::HOT : (online ? pal::BRIGHT : pal::MID);

    // Deck slab with cross-hatch
    const uint8_t deck_a = ghost ? static_cast<uint8_t>(50 + 40 * pulse) : 220;
    fill(x0, y - z2, w, Z(5), with_alpha(deck_col, deck_a));
    outline(x0, y - z2, w, Z(5), edge_col);
    if (!ghost) {
      for (int i = 0; i < w - z4; i += z6)
        be_->line(x0 + i, y - z2, x0 + i + z4, y + z2, with_alpha(pal::MID, 180));
    }

    // Activated: lit edge rails along the deck so the pad reads as "powered"
    if (online) {
      const uint8_t rail_a = static_cast<uint8_t>(140 + 100 * pulse);
      fill(x0, y - z2 - Z(2), w, Z(2), with_alpha(pal::BRIGHT, rail_a));
      fill(x0, y + Z(3), w, Z(2), with_alpha(pal::BRIGHT, static_cast<uint8_t>(rail_a * 0.7f)));
      // Corner ticks
      fill(x0 - z2, y - z4, z4, z4, with_alpha(pal::BRIGHT, rail_a));
      fill(x1 - z2, y - z4, z4, z4, with_alpha(pal::BRIGHT, rail_a));
    }

    // Support struts into the rock
    const Rgba strut = with_alpha(is_dest ? pal::HOT : (online ? pal::PAD : pal::DIM), ghost ? 80 : 180);
    be_->line(x0 + z4, y + Z(3), x0 + z4, y + Z(18), strut);
    be_->line(x1 - z4, y + Z(3), x1 - z4, y + Z(18), strut);
    be_->line(mid, y + Z(3), mid, y + Z(22), strut);
    be_->line(x0 + z4, y + Z(18), x1 - z4, y + Z(18), strut);

    // Approach chevrons, chasing upward (faster / brighter when online or dest)
    const int chase = static_cast<int>(t * (is_dest ? 5.0 : (online ? 4.0 : 2.5))) % 3;
    for (int c = 0; c < 3; ++c) {
      const int cy = y - Z(14) - c * Z(10), inset = Z(8) + c * z6;
      Rgba chev_base = is_dest ? pal::HOT : (online ? pal::BRIGHT : pal::WARN);
      const Rgba chev = with_alpha(chev_base, c == chase ? 255 : (online || is_dest ? 160 : 100));
      be_->line(x0 + inset, cy, mid, cy + z6, chev);
      be_->line(x1 - inset, cy, mid, cy + z6, chev);
    }

    // End beacons — activated pads blink hard with a wide halo
    const bool on = (static_cast<int>(t * (is_dest ? 4.0 : (online ? 3.5 : 2.0))) + pi) % 2 == 0;
    const Rgba beacon = on ? (is_dest ? pal::HOT : pal::BRIGHT) : (is_dest ? pal::WARN : (online ? pal::PAD : pal::DIM));
    for (int bxp : {x0, x1}) {
      if (online || is_dest) {
        // Outer halo so activated pads read at a glance
        fill(bxp - Z(8), y - Z(12), Z(16), Z(16), with_alpha(beacon, on ? 55 : 18));
        fill(bxp - Z(5), y - Z(9), Z(10), Z(10), with_alpha(beacon, on ? 90 : 30));
      } else {
        fill(bxp - Z(5), y - Z(9), Z(10), Z(10), with_alpha(beacon, on ? 40 : 15));
      }
      fill(bxp - z2, y - z6, z4, z4, beacon);
    }

    // Centre T-mark
    be_->line(mid - Z(8), y, mid + Z(8), y, edge_col);
    be_->line(mid, y - z6, mid, y + z2, edge_col);

    // Status label above the pad — the clear activated / dest cue
    if (is_dest) {
      const char* label = ghost ? "DEST?" : "DEST";
      const int tw = text_width(label);
      text(mid - tw / 2, y - Z(52), label, with_alpha(pal::HOT, static_cast<uint8_t>(160 + 95 * pulse)));
    } else if (online) {
      // ACTIVE pulses; VISITED pads add a second line so hangar targets are obvious
      const char* label = p.visited ? "ACTIVE" : "ONLINE";
      const int tw = text_width(label);
      const uint8_t la = static_cast<uint8_t>(150 + 105 * pulse_fast);
      text(mid - tw / 2, y - Z(52), label, with_alpha(pal::BRIGHT, la));
      // Small status lamp under the text
      fill(mid - Z(3), y - Z(58), Z(6), Z(4), with_alpha(pal::BRIGHT, la));
    }

    // Visited mark: filled diamond (teleport/hangar target); online-only: open ring
    if (!ghost) {
      const int mx = mid, my = y - Z(32);
      const int rr = Z(5);
      if (p.visited) {
        be_->line(mx, my - rr, mx + rr, my, pal::BRIGHT);
        be_->line(mx + rr, my, mx, my + rr, pal::BRIGHT);
        be_->line(mx, my + rr, mx - rr, my, pal::BRIGHT);
        be_->line(mx - rr, my, mx, my - rr, pal::BRIGHT);
        fill(mx - Z(2), my - Z(2), Z(4), Z(4), pal::BRIGHT);
      } else if (online) {
        outline(mx - rr, my - rr, rr * 2, rr * 2, with_alpha(pal::WARN, 220));
      }
    }
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
  auto seg = [&](Vec2 a, Vec2 b, Rgba c = {}) {
    if (c.a == 0 && c.r == 0 && c.g == 0 && c.b == 0) c = body;
    SDL_Point pa = W(a), pb = W(b);
    line(pa.x, pa.y, pb.x, pb.y, c);
  };

  const float nose_y = hg.nose_y, cabin_y = hg.cabin_y, belly = hg.belly_y;

  // Flames first so the hull draws over them. Length follows thruster output (damage + flutter)
  // so the plume matches the force the sim is applying this step.
  if (fl.state != FlightState::Crashed) {
    be_->set_blend(Blend::Add);
    for (int i = 0; i < thruster_count(d); ++i) {
      const ThrusterPose tp = thruster_pose(d, i);
      const float dmg = i < Thrusters::MAX ? clampf(th.damage[i], 0.f, 1.f) : 0.f;
      if (dmg >= tune::ENGINE_DEAD) continue;
      const float lvl = i < Thrusters::MAX ? th.output[i] : 0.f;
      if (lvl < 0.05f) continue;
      const float len = (10.f + 46.f * lvl) * (0.8f + 0.4f * flicker(t, i * 1.7f));
      const float wd = d.thruster_n ? 4.f + 3.f * tp.power : (top ? 8.f : 7.f);
      const Vec2 perp{-tp.flame.y, tp.flame.x};
      auto tri = [&](float half_w, float length, Rgba base, Rgba tip) {
        be_->gradient_triangle(W(tp.nozzle - perp * half_w), W(tp.nozzle + perp * half_w), W(tp.nozzle + tp.flame * length), base, tip);
      };
      // Damaged flames go sooty / amber instead of clean white-orange
      const Rgba edge = dmg > 0.05f ? mix(pal::FLAME_EDGE, pal::SMOKE, 0.35f + 0.5f * dmg) : pal::FLAME_EDGE;
      const Rgba core = dmg > 0.05f ? mix(pal::FLAME_CORE, pal::SPARK, 0.2f + 0.5f * dmg) : pal::FLAME_CORE;
      const uint8_t edge_a = static_cast<uint8_t>(220 - 80 * dmg);
      tri(wd, len, with_alpha(edge, edge_a), with_alpha(edge, 0));
      tri(wd * 0.5f, len * 0.6f, with_alpha(core, 255), with_alpha(core, 0));
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
  // Engine damage colour: healthy = body, hurt = amber, dead = hot
  auto eng_col = [&](int ti) -> Rgba {
    if (ti < 0 || ti >= Thrusters::MAX) return body;
    const float dmg = clampf(th.damage[ti], 0.f, 1.f);
    if (dmg < 0.04f) return body;
    if (dmg >= tune::ENGINE_DEAD) return pal::HOT;
    if (dmg >= tune::ENGINE_SPUTTER)
      return mix(pal::WARN, pal::HOT, (dmg - tune::ENGINE_SPUTTER) / std::max(1.f - tune::ENGINE_SPUTTER, 0.01f));
    return mix(body, pal::WARN, dmg / std::max(tune::ENGINE_SPUTTER, 0.01f));
  };
  auto eng_mark = [&](int ti, Vec2 c, Vec2 f, Vec2 perp, float scale) {
    if (ti < 0 || ti >= Thrusters::MAX) return;
    const float dmg = clampf(th.damage[ti], 0.f, 1.f);
    if (dmg < 0.04f) return;
    const Rgba mc = eng_col(ti);
    seg(c - perp * scale, c + perp * scale * 0.3f + f * scale * 0.4f, mc);
    seg(c + perp * scale * 0.5f - f * scale * 0.2f, c - perp * scale * 0.2f + f * scale * 0.5f, mc);
    if (dmg >= tune::ENGINE_SPUTTER) {
      const Vec2 tip = c + f * scale * 1.2f;
      seg(tip, tip - f * scale * 0.5f + perp * scale * 0.6f, mc);
      seg(tip, tip - f * scale * 0.5f - perp * scale * 0.6f, mc);
    }
    if (dmg >= tune::ENGINE_DEAD) {
      const float s = scale * 0.9f;
      seg(c - perp * s - f * s * 0.3f, c + perp * s + f * s * 0.3f, pal::HOT);
      seg(c + perp * s - f * s * 0.3f, c - perp * s + f * s * 0.3f, pal::HOT);
    }
  };

  if (d.thruster_n == 0) {
    // Engine bells: the open end points ground-ward so top mounts still fire "down"
    int ti = 0;
    for (float side : {-1.f, 1.f}) {
      float ex = side * ox;
      float back = top ? ey + 2.f : ey - 2.f, rim = top ? ey + 16.f : ey + 12.f;
      float bw = top ? 8.f : 7.f, rw = top ? 11.f : 10.f;
      const Rgba ec = eng_col(ti);
      seg({ex - bw, back}, {ex + bw, back}, ec);
      seg({ex - bw, back}, {ex - rw, rim}, ec);
      seg({ex + bw, back}, {ex + rw, rim}, ec);
      seg({ex - rw, rim}, {ex + rw, rim}, ec);
      const float mid = 0.5f * (back + rim), mw = 0.5f * (bw + rw);  // cooling rib and nozzle throat
      seg({ex - mw, mid}, {ex + mw, mid}, ec);
      seg({ex - rw * 0.55f, rim - 2.f}, {ex + rw * 0.55f, rim - 2.f}, ec);
      eng_mark(ti, {ex, mid}, {0.f, 1.f}, {1.f, 0.f}, rw * 0.55f);
      ++ti;
    }
  } else {
    for (int i = 0; i < d.thruster_n; ++i) {  // a bell along each thruster's exhaust
      const ThrusterPose tp = thruster_pose(d, i);
      const Vec2 f = tp.flame, p{-f.y, f.x};
      const float w0 = 5.f + 2.f * tp.power, w1 = 8.f + 2.f * tp.power;
      const Vec2 b0 = tp.pos - f * 2.f, b1 = tp.pos + f * 12.f, mid = tp.pos + f * 5.f;
      const Rgba ec = eng_col(i);
      seg(b0 - p * w0, b0 + p * w0, ec);
      seg(b0 - p * w0, b1 - p * w1, ec);
      seg(b0 + p * w0, b1 + p * w1, ec);
      seg(b1 - p * w1, b1 + p * w1, ec);
      seg(mid - p * (0.5f * (w0 + w1)), mid + p * (0.5f * (w0 + w1)), ec);  // cooling rib
      seg(b1 - p * w1 * 0.55f - f * 2.f, b1 + p * w1 * 0.55f - f * 2.f, ec);
      eng_mark(i, mid, f, p, w1 * 0.5f);
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
  fill(m8, m8, L(210), bar_step * (bars + 2) + L(4) + lh * 12 + L(28), with_alpha(pal::MENU, 120));
  int y = m16;
  for (int i = 0; i < bars; ++i) {
    fill(m20, y, bar_w, bar_h, pal::DIM);
    float v = clampf(th.level[i], 0.f, 1.f);
    fill(m20, y, static_cast<int>(bar_w * v), bar_h, mix(pal::MID, pal::HOT, smoothstep((v - 0.6f) / 0.4f)));
    outline(m20, y, bar_w, bar_h, with_alpha(pal::MID, 120));
    static const char* labels[] = {"L", "R", "LS", "RS", "LSd", "RSd"};
    text(L(148), y - L(3), labels[i < 6 ? i : 0], pal::MID);
    y += bar_step;
  }
  // Per-engine damage row (thruster index order, not control channel)
  float max_eng_dmg = 0.f;
  int dead_eng = 0, hurt_eng = 0;
  {
    const int n_eng = std::min(thruster_count(d), Thrusters::MAX);
    if (n_eng > 0) {
      const int cell = std::max(L(8), bar_w / n_eng - L(2));
      for (int i = 0; i < n_eng; ++i) {
        const float dmg = clampf(th.damage[i], 0.f, 1.f);
        max_eng_dmg = std::max(max_eng_dmg, dmg);
        if (dmg >= tune::ENGINE_DEAD) ++dead_eng;
        else if (dmg >= tune::ENGINE_SPUTTER) ++hurt_eng;
        const int x = m20 + i * (cell + L(2));
        fill(x, y, cell, bar_h, pal::DIM);
        if (dmg > 0.02f) {
          Rgba col = dmg >= tune::ENGINE_DEAD ? pal::HOT : mix(pal::WARN, pal::HOT, dmg);
          fill(x, y, cell, static_cast<int>(bar_h * dmg), col);
        }
        // Flash outline when sputtering or dead
        const bool flash = dmg >= tune::ENGINE_SPUTTER && std::fmod(ui.time, 0.5) < 0.25;
        outline(x, y, cell, bar_h, flash ? pal::HOT : with_alpha(pal::MID, 120));
      }
      text(L(148), y - L(3), dead_eng ? "OUT" : (hurt_eng ? "HURT" : "ENG"),
           dead_eng ? pal::HOT : (hurt_eng ? pal::WARN : pal::MID));
      y += bar_step;
    }
  }
  y += L(4);
  // Fuel gauge (drains with thrust, refills on pads)
  fill(m20, y, bar_w, bar_h, pal::DIM);
  const float fuel = clampf(fl.fuel, 0.f, 1.f);
  fill(m20, y, static_cast<int>(bar_w * fuel), bar_h,
       fuel > tune::FUEL_LIMP ? pal::PAD : mix(pal::WARN, pal::PAD, fuel / tune::FUEL_LIMP));
  outline(m20, y, bar_w, bar_h, with_alpha(pal::MID, 120));
  text(L(148), y - L(3), fuel > tune::FUEL_LIMP ? "FUEL" : "LIMP",
       fuel > tune::FUEL_LIMP ? pal::MID : pal::WARN);
  y += bar_step;
  if (fl.hurt > 0.02f) {
    fill(m20, y, bar_w, bar_h, pal::DIM);
    fill(m20, y, static_cast<int>(bar_w * clampf(fl.hurt, 0.f, 1.f)), bar_h, mix(pal::WARN, pal::HOT, fl.hurt));
    outline(m20, y, bar_w, bar_h, with_alpha(pal::MID, 120));
    text(L(148), y - L(3), "DMG", pal::WARN);
    y += bar_step;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s%s", d.name, d.engines_top ? " TOP" : "");
  text(m20, y, buf, pal::BRIGHT);
  y += lh;
  std::snprintf(buf, sizeof(buf), "SCORE %d", g.score);
  text(m20, y, buf, pal::BRIGHT);
  y += lh;
  {
    int found = 0;
    for (const Game::Signal& s : g.signals) if (s.found) ++found;
    const int total = static_cast<int>(g.signals.size());
    std::snprintf(buf, sizeof(buf), "SIG %d/%d", found, total);
    text(m20, y, buf, found == total && total > 0 ? pal::CARGO : pal::MID);
    y += lh;
  }
  char bname[24];
  action_bind_label(binds, Action::Pause, !pad, bname, sizeof bname);
  char linebuf[48];
  std::snprintf(linebuf, sizeof linebuf, "%s MENU", bname);
  text(m20, y, linebuf, pal::MID);
  y += lh;
  const Legs& legs = g.ecs.get<Legs>(g.ship);
  action_bind_label(binds, Action::Legs, !pad, bname, sizeof bname);
  char legbuf[64];
  std::snprintf(legbuf, sizeof legbuf, "LEGS %s  %s", legs.deployed ? "OUT" : "IN", bname);
  text(m20, y, legbuf, legs.deployed ? pal::MID : pal::WARN);
  y += lh;
  const Rope& rp = g.ecs.get<Rope>(g.ship);
  action_bind_label(binds, Action::Grip, !pad, bname, sizeof bname);
  // Winch length bar (MIN_LEN..OUT_LEN)
  {
    const float span = std::max(1.f, rope::OUT_LEN - rope::MIN_LEN);
    const float u = clampf((rp.length - rope::MIN_LEN) / span, 0.f, 1.f);
    fill(m20, y, bar_w, bar_h, pal::DIM);
    fill(m20, y, static_cast<int>(bar_w * u), bar_h, mix(pal::MID, pal::CARGO, u));
    outline(m20, y, bar_w, bar_h, with_alpha(pal::MID, 120));
    text(L(148), y - L(3), "WINCH", pal::MID);
    y += bar_step;
  }
  if (rp.held != NULL_ENTITY) {
    const Cargo& held = g.ecs.get<Cargo>(rp.held);
    std::snprintf(legbuf, sizeof legbuf, "LOAD %s %.2f  %s", held.def->name, held.def->mass, bname);
    text(m20, y, legbuf, pal::CARGO);
    y += lh;
    if (held.dest_pad >= 0 && held.dest_pad < static_cast<int>(g.cave.pads.size())) {
      const LandingPad& dp = g.cave.pads[static_cast<size_t>(held.dest_pad)];
      const float cx = 0.5f * (dp.x0 + dp.x1);
      const float dx = cx - tf.pos.x, dy = dp.y - tf.pos.y;
      const float dist = std::sqrt(dx * dx + dy * dy);
      std::snprintf(legbuf, sizeof legbuf, "DEST %s %.0f", dp.active ? "PAD" : "???", dist / PPM);
      text(m20, y, legbuf, dp.active ? pal::WARN : pal::DIM);
      y += lh;
    }
  } else {
    std::snprintf(legbuf, sizeof legbuf, "HOOK %s  %s", rp.out ? "OUT" : "IN", bname);
    text(m20, y, legbuf, pal::MID);
    y += lh;
    // Proximity cue: MAGNET (auto) or NEAR (manual reach) when a crate is close to the hook
    if (rp.out) {
      const float reach = rope::AUTO_GRAB ? rope::AUTO_GRAB_REACH : rope::GRAB_REACH;
      bool near = false;
      g.ecs.view<Cargo, Transform>([&](Entity, const Cargo& c, const Transform& ct) {
        if (!c.def) return;
        const float dx = std::max(std::abs(ct.pos.x - rp.hook_pos.x) - c.def->half_w, 0.f);
        const float dy = std::max(std::abs(ct.pos.y - rp.hook_pos.y) - c.def->half_h, 0.f);
        if (std::sqrt(dx * dx + dy * dy) < reach) near = true;
      });
      if (near) {
        text(m20, y, rope::AUTO_GRAB ? "MAGNET" : "NEAR", pal::CARGO);
        y += lh;
      }
    }
  }
  {
    char pb[24], qb[24];
    action_bind_label(binds, Action::PrevPad, !pad, pb, sizeof pb);
    action_bind_label(binds, Action::NextPad, !pad, qb, sizeof qb);
    std::snprintf(legbuf, sizeof legbuf, "PAD %s/%s", pb, qb);
    text(m20, y, legbuf, pal::MID);
  }
  y += lh;
  // Hangar: ship select + teleport when settled on a pad
  if (fl.state == FlightState::Landed && g.cave.pad_below(tf.pos.x, tf.pos.y, 140.f)) {
    char hb[24];
    action_bind_label(binds, Action::Hangar, !pad, hb, sizeof hb);
    std::snprintf(legbuf, sizeof legbuf, "HANGAR %s", hb);
    text(m20, y, legbuf, pal::PAD);
    y += lh;
  }
  y += L(4);

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
  fill(rx - L(12), m8, w_ - rx + L(4), lh * 4 + m16, with_alpha(pal::MENU, 120));
  for (int i = 0; i < 3; ++i) text(rx, m16 + lh * i, status[i].s, status[i].ok ? pal::PAD : pal::HOT);
  // Compass: cargo destination while hauling, otherwise nearest unfound signal
  {
    float best_d = 1e12f;
    Vec2 best{};
    bool any = false;
    const char* tag = "SIG";
    Rgba col = pal::CARGO;
    const Rope& rope = g.ecs.get<Rope>(g.ship);
    if (rope.held != NULL_ENTITY) {
      const Cargo& held = g.ecs.get<Cargo>(rope.held);
      if (held.dest_pad >= 0 && held.dest_pad < static_cast<int>(g.cave.pads.size())) {
        const LandingPad& dp = g.cave.pads[static_cast<size_t>(held.dest_pad)];
        best = {0.5f * (dp.x0 + dp.x1) - tf.pos.x, dp.y - tf.pos.y};
        best_d = best.x * best.x + best.y * best.y;
        any = true;
        tag = dp.active ? "DEST" : "DEST?";
        col = pal::WARN;
      }
    }
    if (!any) {
      for (const Game::Signal& sig : g.signals) {
        if (sig.found) continue;
        const float dx = sig.pos.x - tf.pos.x, dy = sig.pos.y - tf.pos.y;
        const float d = dx * dx + dy * dy;
        if (d < best_d) { best_d = d; best = {dx, dy}; any = true; }
      }
      tag = "SIG";
      col = pal::CARGO;
    }
    if (any) {
      const float len = std::sqrt(best_d);
      const float ang = std::atan2(best.y, best.x);
      char cbuf[32];
      std::snprintf(cbuf, sizeof cbuf, "%s %.0f", tag, len / PPM);
      text(rx, m16 + lh * 3, cbuf, col);
      const int ax = rx - L(18), ay = m16 + lh * 3 + cell_h() / 2;
      const int ex = ax + static_cast<int>(std::cos(ang) * L(10));
      const int ey = ay + static_cast<int>(std::sin(ang) * L(10));
      line(ax, ay, ex, ey, col);
    } else if (!g.signals.empty()) {
      text(rx, m16 + lh * 3, "SIG DONE", pal::PAD);
    }
  }

  if (g.notice_timer > 0.f)
    text_centered(w_ / 2, L(48) + (fl.state != FlightState::Flying ? 2 * lh + L(8) : 0), g.notice,
                  with_alpha(pal::CARGO, static_cast<uint8_t>(255 * clampf(g.notice_timer / 0.5f, 0.f, 1.f))));
  // Persistent engine-damage warning (separate from the transient notice)
  if (max_eng_dmg >= 0.04f && fl.state != FlightState::Crashed) {
    const bool flash = std::fmod(ui.time, 0.6) < 0.35;
    const char* msg = dead_eng ? "ENGINE OUT" : (max_eng_dmg >= tune::ENGINE_SPUTTER ? "ENGINE DAMAGE" : "ENGINE WEAR");
    Rgba wc = dead_eng ? pal::HOT : (max_eng_dmg >= tune::ENGINE_SPUTTER ? pal::WARN : mix(pal::MID, pal::WARN, 0.6f));
    if (flash || max_eng_dmg < tune::ENGINE_SPUTTER)
      text_centered(w_ / 2, L(48) + lh + (fl.state != FlightState::Flying ? 2 * lh + L(8) : 0) +
                            (g.notice_timer > 0.f ? lh : 0),
                    msg, wc);
  }
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
      char msg[64];
      std::snprintf(msg, sizeof msg, "%s TO RESPAWN", rb);
      text_centered(w_ / 2, L(48) + lh, msg, pal::MID);
    }
  }
}

void Gfx::draw_minimap(Game& g, const UiState& ui, double t) {
  if (!minimap_ || minimap_generation_ != g.cave.generation || g.reveal_dirty) {
    build_minimap(g.cave, g.revealed);
    g.reveal_dirty = false;
  }
  // On-screen panel size is fixed; source window (cells) follows minimap_zoom.
  const int z = std::clamp(ui.minimap_zoom, 0, MM_ZOOM_COUNT - 1);
  const int src_w = std::min(MM_ZOOM_W[z], Cave::GW);
  const int src_h = std::min(MM_ZOOM_H[z], Cave::GH);
  const int mm_w = L(MM_W), mm_h = L(MM_H), pad = L(4);
  // Bottom-left corner (out of the way of centred HUD / notices)
  const int x = L(14), y = h_ - mm_h - L(14);
  fill(x - pad, y - pad, mm_w + 2 * pad, mm_h + 2 * pad, with_alpha(pal::BG, 170));

  // The window scrolls with the ship and stops at the map's edges
  const Transform& tf = g.ecs.get<Transform>(g.ship);
  const float ox = clampf(tf.pos.x * MM_K - src_w * 0.5f, 0.f, static_cast<float>(Cave::GW - src_w));
  const float oy = clampf(tf.pos.y * MM_K - src_h * 0.5f, 0.f, static_cast<float>(Cave::GH - src_h));
  const int ix = static_cast<int>(ox), iy = static_cast<int>(oy);
  be_->copy_part(minimap_, SDL_Rect{ix, iy, src_w, src_h}, SDL_Rect{x, y, mm_w, mm_h}, {255, 255, 255, 255});
  outline(x - pad, y - pad, mm_w + 2 * pad, mm_h + 2 * pad, with_alpha(pal::MID, 140));

  // Sonar ring + reflection arcs on the chart
  if (g.sonar.active) {
    const float fade = clampf(g.sonar.fade, 0.f, 1.f);
    const float sx_k = static_cast<float>(mm_w) / static_cast<float>(src_w);
    const float sy_k = static_cast<float>(mm_h) / static_cast<float>(src_h);
    const float ox_w = g.sonar.origin.x * MM_K - ix;
    const float oy_w = g.sonar.origin.y * MM_K - iy;
    auto mm_pt = [&](float ang, float rr) {
      return std::pair<int, int>{
          x + static_cast<int>((ox_w + rr * std::cos(ang)) * sx_k),
          y + static_cast<int>((oy_w + rr * std::sin(ang)) * sy_k)};
    };
    auto on_panel = [&](int px, int py) {
      return px >= x && py >= y && px < x + mm_w && py < y + mm_h;
    };
    if (g.sonar.radius > 1.f && fade > 0.05f) {
      const float rr = g.sonar.radius * MM_K;
      const Rgba rc = with_alpha(pal::CARGO, static_cast<uint8_t>(40 + 100 * fade));
      const int segs = 48;
      auto [px0, py0] = mm_pt(0.f, rr);
      for (int i = 1; i <= segs; ++i) {
        const float a = (static_cast<float>(i) / segs) * 6.2831853f;
        auto [px1, py1] = mm_pt(a, rr);
        if (on_panel(px0, py0) && on_panel(px1, py1)) line(px0, py0, px1, py1, rc);
        px0 = px1;
        py0 = py1;
      }
    }
    // Reflection segments: chord-mirrored arcs (bulge toward origin on the chart)
    for (const SonarReflection& e : g.sonar.echoes) {
      if (e.age > e.life) continue;
      const float life_u = 1.f - e.age / e.life;
      Rgba base = pal::CARGO;
      if (e.kind == SonarReflection::Kind::Pad) base = pal::WARN;
      else if (e.kind == SonarReflection::Kind::Signal) base = pal::BRIGHT;
      const float arc = 0.28f;
      const float rr = e.hit_r * MM_K;
      const float a0 = e.angle - arc, a1 = e.angle + arc;
      const float half = 0.5f * (a1 - a0);
      const float amid = 0.5f * (a0 + a1);
      const float ux = std::cos(amid), uy = std::sin(amid);
      const float d_chord = rr * std::cos(half);
      // Origin of the ping in panel-local (pre-scale) coords is (ox_w, oy_w)
      const float ocx = ox_w + 2.f * d_chord * ux;
      const float ocy = oy_w + 2.f * d_chord * uy;
      const float c0x = ox_w + rr * std::cos(a0), c0y = oy_w + rr * std::sin(a0);
      const float mrad = std::sqrt((c0x - ocx) * (c0x - ocx) + (c0y - ocy) * (c0y - ocy));
      float ma0 = std::atan2(oy_w + rr * std::sin(a0) - ocy, c0x - ocx);
      float ma1 = std::atan2(oy_w + rr * std::sin(a1) - ocy, ox_w + rr * std::cos(a1) - ocx);
      float d = ma1 - ma0;
      while (d > PI) d -= 2.f * PI;
      while (d < -PI) d += 2.f * PI;
      const int segs = 6;
      if (mrad < 0.5f) continue;
      auto mm_m = [&](float a) -> std::pair<int, int> {
        const float lx = ocx + mrad * std::cos(a);
        const float ly = ocy + mrad * std::sin(a);
        return {x + static_cast<int>(lx * sx_k), y + static_cast<int>(ly * sy_k)};
      };
      auto [px0, py0] = mm_m(ma0);
      for (int i = 1; i <= segs; ++i) {
        const float a = ma0 + d * (static_cast<float>(i) / segs);
        auto [px1, py1] = mm_m(a);
        if (on_panel(px0, py0) && on_panel(px1, py1))
          line(px0, py0, px1, py1, with_alpha(base, static_cast<uint8_t>(80 + 160 * life_u)));
        px0 = px1;
        py0 = py1;
      }
    }
  }

  // Map coordinates (world px) -> panel px
  const float sx_k = static_cast<float>(mm_w) / static_cast<float>(src_w);
  const float sy_k = static_cast<float>(mm_h) / static_cast<float>(src_h);
  auto to_panel = [&](float wx, float wy) -> std::pair<int, int> {
    return {x + static_cast<int>((wx * MM_K - ix) * sx_k),
            y + static_cast<int>((wy * MM_K - iy) * sy_k)};
  };
  auto on_panel = [&](int px, int py) {
    return px >= x && py >= y && px < x + mm_w && py < y + mm_h;
  };
  // Filled downward triangle (landing pad marker)
  auto tri_down = [&](int cx, int cy, int s, Rgba c) {
    if (!on_panel(cx, cy)) return;
    SDL_Point pts[3] = {{cx, cy + s}, {cx - s, cy - s / 2}, {cx + s, cy - s / 2}};
    be_->polygon(pts, 3, c);
  };
  // Filled diamond (signals / cargo)
  auto diamond = [&](int cx, int cy, int s, Rgba c) {
    if (!on_panel(cx, cy)) return;
    SDL_Point pts[4] = {{cx, cy - s}, {cx + s, cy}, {cx, cy + s}, {cx - s, cy}};
    be_->polygon(pts, 4, c);
  };
  auto is_rev = [&](float wx, float wy) {
    const int gx = static_cast<int>(wx / Cave::CELL), gy = static_cast<int>(wy / Cave::CELL);
    if (!Cave::in_grid(gx, gy) || g.revealed.empty()) return true;
    return g.revealed[static_cast<size_t>(gy * Cave::GW + gx)] >= 60;
  };
  int dest_pi = -1;
  if (g.ecs.get<Rope>(g.ship).held != NULL_ENTITY)
    dest_pi = g.ecs.get<Cargo>(g.ecs.get<Rope>(g.ship).held).dest_pad;

  // Landing pads: downward triangle (▼) — larger when dest, brighter when visited
  const int pad_s = L(5);
  for (int pi = 0; pi < static_cast<int>(g.cave.pads.size()); ++pi) {
    const LandingPad& p = g.cave.pads[static_cast<size_t>(pi)];
    if (!p.active) continue;
    const float cxw = 0.5f * (p.x0 + p.x1);
    if (!is_rev(cxw, p.y)) continue;
    const bool is_dest = (pi == dest_pi);
    Rgba col = is_dest ? pal::HOT : (p.visited ? pal::BRIGHT : pal::WARN);
    auto [px, py] = to_panel(cxw, p.y);
    tri_down(px, py, is_dest ? pad_s + L(2) : pad_s, col);
  }
  // Signals: diamond
  for (const Game::Signal& sig : g.signals) {
    if (!sig.found) continue;
    auto [px, py] = to_panel(sig.pos.x, sig.pos.y);
    diamond(px, py, L(4), pal::CARGO);
  }
  // Echoes that just answered a ping flash on the chart
  for (const Game::Echo& e : g.echoes) {
    if (e.cool < 6.5f || e.cool > 8.f) continue;
    auto [px, py] = to_panel(e.pos.x, e.pos.y);
    diamond(px, py, L(3), with_alpha(pal::BRIGHT, 200));
  }
  // Cargo: small square
  g.ecs.view<Cargo, Transform>([&](Entity, const Cargo&, const Transform& ct) {
    if (!is_rev(ct.pos.x, ct.pos.y)) return;
    auto [px, py] = to_panel(ct.pos.x, ct.pos.y);
    const int s = L(4);
    if (on_panel(px, py)) fill(px - s / 2, py - s / 2, s, s, pal::CARGO);
  });

  // Viewport box, clipped to the panel
  const int vw = static_cast<int>(g.cam.vw * MM_K * sx_k), vh = static_cast<int>(g.cam.vh * MM_K * sy_k);
  const int bx = x + static_cast<int>((g.cam.x * MM_K - ix) * sx_k);
  const int by = y + static_cast<int>((g.cam.y * MM_K - iy) * sy_k);
  const int cx0 = std::max(bx, x), cy0 = std::max(by, y), cx1 = std::min(bx + vw, x + mm_w), cy1 = std::min(by + vh, y + mm_h);
  if (cx1 > cx0 && cy1 > cy0) outline(cx0, cy0, cx1 - cx0, cy1 - cy0, with_alpha(pal::BRIGHT, 120));

  // Ship: filled triangle pointing along the nose (angle 0 = up / -Y)
  {
    auto [sx, sy] = to_panel(tf.pos.x, tf.pos.y);
    if (on_panel(sx, sy)) {
      const float ang = tf.angle;
      // Nose direction in world (and panel — same axes)
      const float nx = std::sin(ang), ny = -std::cos(ang);
      const float px = -ny, py = nx;  // perpendicular
      const float tip = static_cast<float>(L(7));
      const float back = tip * 0.55f, half = tip * 0.55f;
      SDL_Point pts[3] = {
          {sx + static_cast<int>(nx * tip), sy + static_cast<int>(ny * tip)},
          {sx + static_cast<int>(-nx * back + px * half), sy + static_cast<int>(-ny * back + py * half)},
          {sx + static_cast<int>(-nx * back - px * half), sy + static_cast<int>(-ny * back - py * half)},
      };
      // Always draw ship (pulse brightness only)
      const bool flash = std::fmod(t, 0.6) < 0.4;
      be_->polygon(pts, 3, flash ? pal::HOT : with_alpha(pal::HOT, 200));
      // Outline for contrast on bright rock
      line(pts[0].x, pts[0].y, pts[1].x, pts[1].y, pal::BRIGHT);
      line(pts[1].x, pts[1].y, pts[2].x, pts[2].y, pal::BRIGHT);
      line(pts[2].x, pts[2].y, pts[0].x, pts[0].y, pal::BRIGHT);
    }
  }

  // Zoom level cue under the panel
  static const char* ZOOM_LABEL[] = {"NEAR", "MID", "FAR"};
  text(x, y + mm_h + L(2), ZOOM_LABEL[z], with_alpha(pal::MID, 180));
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
    char val[64];
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

  if (ui.page == MenuPage::Debug) {
    const int lh = cell_h() + L(6);
    const int rows = DEBUG_PARAM_COUNT + item_count(DEBUG_ITEMS);
    const int panel_w = std::min(w_ - L(16), L(560));
    // Scroll so the selected row stays visible when the list is taller than the panel
    const int max_vis = std::max(8, (h_ - L(100)) / lh);
    const int panel_h = L(70) + std::min(rows, max_vis) * lh + L(40);
    const int px = w_ / 2 - panel_w / 2, py = std::max(L(4), h_ / 2 - panel_h / 2);
    fill(0, 0, w_, h_, with_alpha(pal::BG, 120));
    fill(px, py, panel_w, panel_h, pal::MENU);
    outline(px, py, panel_w, panel_h, pal::BRIGHT);
    outline(px + L(3), py + L(3), panel_w - L(6), panel_h - L(6), with_alpha(pal::MID, 90));
    text_centered(w_ / 2, py + L(14), "DEBUG", pal::BRIGHT);
    fill(px + L(20), py + L(14) + cell_h() + L(6), panel_w - L(40), 1, with_alpha(pal::MID, 140));

    int scroll = 0;
    if (rows > max_vis) {
      scroll = std::clamp(ui.debug_cursor - max_vis / 2, 0, rows - max_vis);
    }
    DebugParam* params = debug_params(const_cast<Game&>(g));
    char val[32];
    int row_y = py + L(14) + cell_h() + L(16);
    for (int vis = 0; vis < std::min(rows, max_vis); ++vis) {
      const int i = scroll + vis;
      const bool sel = ui.debug_cursor == i;
      const bool dirty = i < DEBUG_PARAM_COUNT && debug_param_changed(const_cast<Game&>(g), i);
      const int ly = row_y + vis * lh;
      if (sel) {
        fill(px + L(10), ly - L(3), panel_w - L(20), cell_h() + L(6), with_alpha(pal::MID, 40));
        if (std::fmod(ui.time, 0.8) < 0.55) text(px + L(28) - cell_w() - L(4), ly, ">", pal::WARN);
      }
      // Changed-from-default rows use HOT; selected still WARN; else MID
      const Rgba col = sel ? pal::WARN : (dirty ? pal::HOT : pal::MID);
      if (i < DEBUG_PARAM_COUNT) {
        text(px + L(28), ly, params[i].name, col);
        const float v = debug_param_value(const_cast<Game&>(g), params[i]);
        if (params[i].kind == DebugParam::Kind::SonarMode) {
          const int mi = static_cast<int>(v);
          std::snprintf(val, sizeof val, "%s",
                        (mi >= 0 && mi < tune::SONAR_MODE_COUNT) ? tune::SONAR_MODES[mi].name : "?");
        } else if (params[i].kind == DebugParam::Kind::PassiveExplore ||
                   params[i].kind == DebugParam::Kind::AutoGrab) {
          std::snprintf(val, sizeof val, "%s", v >= 0.5f ? "ON" : "OFF");
        } else if (v >= 100.f || (v == std::floor(v) && std::fabs(v) >= 10.f))
          std::snprintf(val, sizeof val, "%.0f", v);
        else if (v >= 10.f)
          std::snprintf(val, sizeof val, "%.1f", v);
        else
          std::snprintf(val, sizeof val, "%.2f", v);
        char buf[64];
        if (sel)
          std::snprintf(buf, sizeof buf, "< %s >", val);
        else
          std::snprintf(buf, sizeof buf, "%s", val);
        text(px + panel_w - L(28) - text_width(buf), ly, buf, col);
      } else {
        text(px + L(28), ly, DEBUG_ITEMS[i - DEBUG_PARAM_COUNT].label, col);
      }
    }
    const bool kbd = ui.device != InputDevice::Gamepad;
    text_centered(w_ / 2, py + panel_h - cell_h() - L(12),
                  kbd ? "LEFT/RIGHT TWEAK  Y RESET  ESC BACK" : "D-PAD TWEAK  Y RESET  B BACK", pal::MID);
    return;
  }

  if (ui.page == MenuPage::Hangar) {
    // Dynamic rows: SHIP + each visited pad + CLOSE footer
    std::vector<int> visited;
    for (int i = 0; i < static_cast<int>(g.cave.pads.size()); ++i)
      if (g.cave.pads[static_cast<size_t>(i)].visited) visited.push_back(i);
    const int pad_rows = static_cast<int>(visited.size());
    const int rows = 1 + pad_rows + item_count(HANGAR_ITEMS);  // ship + pads + close
    const int lh = cell_h() + L(8);
    const int panel_w = std::min(w_ - L(16), L(560));
    const int max_vis = std::max(8, (h_ - L(120)) / lh);
    const int ship_stat_h = L(12) + 4 * (cell_h() + L(3));
    const int panel_h = L(70) + std::min(rows, max_vis) * lh + ship_stat_h + L(44);
    const int px = w_ / 2 - panel_w / 2, py = std::max(L(4), h_ / 2 - panel_h / 2);
    fill(0, 0, w_, h_, with_alpha(pal::BG, 120));
    fill(px, py, panel_w, panel_h, pal::MENU);
    outline(px, py, panel_w, panel_h, pal::BRIGHT);
    outline(px + L(3), py + L(3), panel_w - L(6), panel_h - L(6), with_alpha(pal::MID, 90));
    text_centered(w_ / 2, py + L(14), "HANGAR", pal::BRIGHT);
    fill(px + L(20), py + L(14) + cell_h() + L(6), panel_w - L(40), 1, with_alpha(pal::MID, 140));

    int scroll = 0;
    if (rows > max_vis)
      scroll = std::clamp(ui.hangar_cursor - max_vis / 2, 0, rows - max_vis);

    int row_y = py + L(14) + cell_h() + L(16);
    for (int vis = 0; vis < std::min(rows, max_vis); ++vis) {
      const int i = scroll + vis;
      const bool sel = ui.hangar_cursor == i;
      const int ly = row_y + vis * lh;
      const Rgba col = sel ? pal::WARN : pal::MID;
      if (sel) {
        fill(px + L(12), ly - L(4), panel_w - L(24), cell_h() + L(8), with_alpha(pal::MID, 40));
        if (std::fmod(ui.time, 0.8) < 0.55) text(px + L(28) - cell_w() - L(4), ly, ">", pal::WARN);
      }
      if (i == 0) {
        // Ship row
        text(px + L(28), ly, "SHIP", col);
        char val[40];
        const char* name = g.ecs.has<Hull>(g.ship) ? g.ecs.get<Hull>(g.ship).def->name : "?";
        if (sel)
          std::snprintf(val, sizeof val, "< %s >", name);
        else
          std::snprintf(val, sizeof val, "%s", name);
        text(px + panel_w - L(28) - text_width(val), ly, val, col);
      } else if (i <= pad_rows) {
        const int pi = visited[static_cast<size_t>(i - 1)];
        char lab[40];
        const bool here = (pi == g.last_pad);
        std::snprintf(lab, sizeof lab, "PAD %d%s", pi + 1, here ? "  (HERE)" : "");
        text(px + L(28), ly, lab, col);
        if (!here) {
          const char* go = sel ? "TELEPORT >" : "TELEPORT";
          text(px + panel_w - L(28) - text_width(go), ly, go, sel ? pal::HOT : pal::DIM);
        }
      } else {
        text(px + L(28), ly, HANGAR_ITEMS[i - 1 - pad_rows].label, col);
      }
    }
    // Ship stats under the list
    if (g.ecs.has<Hull>(g.ship)) {
      const ShipPerf sp = ship_perf(*g.ecs.get<Hull>(g.ship).def);
      int iy = py + panel_h - cell_h() - L(12) - 4 * (cell_h() + L(3)) - L(8);
      fill(px + L(20), iy - L(6), panel_w - L(40), 1, with_alpha(pal::MID, 100));
      char line[64];
      auto srow = [&](const char* lab, const char* val, Rgba vc) {
        text(px + L(28), iy, lab, pal::DIM);
        text(px + panel_w - L(28) - text_width(val), iy, val, vc);
        iy += cell_h() + L(3);
      };
      std::snprintf(line, sizeof line, "%.2f   I %.0f", sp.mass, sp.inertia);
      srow("MASS", line, pal::BRIGHT);
      std::snprintf(line, sizeof line, "%.0f  (pwr %.2f)", sp.max_thrust, sp.total_power);
      srow("THRUST", line, pal::BRIGHT);
      std::snprintf(line, sizeof line, "%.2f", sp.twr);
      srow("TWR", line, sp.twr >= 1.f ? pal::PAD : pal::WARN);
      std::snprintf(line, sizeof line, "%+.2f  spin %.2f", sp.yaw_bias, sp.spin);
      srow("YAW BIAS", line, std::fabs(sp.yaw_bias) > 0.05f ? pal::HOT : pal::BRIGHT);
    }
    const bool kbd = ui.device != InputDevice::Gamepad;
    text_centered(w_ / 2, py + panel_h - cell_h() - L(12),
                  kbd ? "LEFT/RIGHT SHIP  ENTER GO  ESC CLOSE" : "D-PAD SHIP  A GO  B CLOSE", pal::MID);
    return;
  }

  const MenuPageDef& page = page_def(ui.page);
  const int lh = cell_h() + L(9);
  const int stat_rows = ui.page == MenuPage::Stats ? STAT_FIELD_COUNT : 0;  // read-only lines above the items
  // Options: ship performance block under the list (mass, inertia, TWR, bias…)
  const int ship_info_rows = (ui.page == MenuPage::Options) ? 5 : 0;
  const int panel_w = std::min(w_ - L(20), L(560));
  const int panel_h = L(78) + (stat_rows + page.count) * lh + (stat_rows ? L(10) : 0) +
                      (ship_info_rows ? L(12) + ship_info_rows * (cell_h() + L(4)) : 0) + L(44);
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

  // Ship performance readout on Options (updates as the roster cycles)
  if (ui.page == MenuPage::Options && g.ecs.has<Hull>(g.ship)) {
    const ShipDef& def = *g.ecs.get<Hull>(g.ship).def;
    const ShipPerf sp = ship_perf(def);
    int iy = row_y + page.count * lh + L(10);
    fill(px + L(24), iy - L(6), panel_w - L(48), 1, with_alpha(pal::MID, 100));
    char line[64];
    auto row = [&](const char* lab, const char* val, Rgba vc) {
      text(px + L(36), iy, lab, pal::DIM);
      text(px + panel_w - L(36) - text_width(val), iy, val, vc);
      iy += cell_h() + L(4);
    };
    std::snprintf(line, sizeof line, "%.2f", sp.mass);
    row("MASS", line, pal::BRIGHT);
    std::snprintf(line, sizeof line, "%.0f", sp.inertia);
    row("INERTIA", line, pal::BRIGHT);
    std::snprintf(line, sizeof line, "%.0f  x%.2f", sp.max_thrust, sp.total_power);
    row("THRUST", line, pal::BRIGHT);
    std::snprintf(line, sizeof line, "%.2f", sp.twr);
    row("TWR", line, sp.twr >= 1.f ? pal::PAD : pal::WARN);
    // Bias: residual yaw with equal mains; Spin: peak differential yaw authority
    std::snprintf(line, sizeof line, "%+.2f  spin %.2f", sp.yaw_bias, sp.spin);
    row("YAW BIAS", line, std::fabs(sp.yaw_bias) > 0.05f ? pal::HOT : pal::BRIGHT);
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

void Gfx::draw_residues(const Game& g) const {
  // Permanent crash wreckage (line scraps)
  for (const Game::WreckPart& w : g.wreckage) {
    const int x0 = sx(g.cam, w.a.x), y0 = sy(w.a.y);
    const int x1 = sx(g.cam, w.b.x), y1 = sy(w.b.y);
    // Cull if both ends far off-screen
    if ((x0 < -40 && x1 < -40) || (x0 > w_ + 40 && x1 > w_ + 40) ||
        (y0 < -40 && y1 < -40) || (y0 > h_ + 40 && y1 > h_ + 40))
      continue;
    line(x0, y0, x1, y1, w.settled ? with_alpha(w.col, 200) : w.col);
  }
  for (const Game::Residue& r : g.residues) {
    const float u = clampf(r.life / tune::RESIDUE_TTL, 0.f, 1.f);
    const int s = std::max(2, Z(3));
    const int x = sx(g.cam, r.pos.x) - s / 2, y = sy(r.pos.y) - s / 2;
    fill(x, y, s, s, with_alpha(pal::CARGO, static_cast<uint8_t>(30 + 140 * u)));
  }
  // Ambient life: brief bright mote when it answers a ping
  for (const Game::Echo& e : g.echoes) {
    if (e.cool < 6.f || e.cool > 8.f) continue;
    const float u = (e.cool - 6.f) / 2.f;
    const int s = std::max(2, Z(4));
    fill(sx(g.cam, e.pos.x) - s / 2, sy(e.pos.y) - s / 2, s, s,
         with_alpha(pal::BRIGHT, static_cast<uint8_t>(80 + 140 * u)));
  }
}

void Gfx::draw_sonar(const Game& g) const {
  if (!g.sonar.active) return;
  const Vec2 o = g.sonar.origin;
  const float r = g.sonar.radius;
  const float fade = clampf(g.sonar.fade, 0.f, 1.f);
  if (fade < 0.02f && g.sonar.echoes.empty()) return;

  auto W = [&](float wx, float wy) { return SDL_Point{sx(g.cam, wx), sy(wy)}; };

  // Mirrored circular segment: the arc from a0..a1 on the ring is reflected across
  // its chord so it bulges the other way (toward the origin for the short arc).
  // That is the echo "segment" — the inverse of the ring arc.
  auto stroke_mirrored_segment = [&](float radius, float a0, float a1, int segs, Rgba col) {
    if (radius < 1.f || segs < 1) return;
    const float half = 0.5f * (a1 - a0);
    if (std::fabs(half) < 1e-4f) return;
    // Chord is perpendicular to the mid-radius; distance origin→chord = r·cos(half)
    const float amid = 0.5f * (a0 + a1);
    const float ux = std::cos(amid), uy = std::sin(amid);
    const float d_chord = radius * std::cos(half);
    // Reflect the ring centre across the chord → mirrored arc centre
    const float ocx = o.x + 2.f * d_chord * ux;
    const float ocy = o.y + 2.f * d_chord * uy;
    // Endpoints on the ring
    const float c0x = o.x + radius * std::cos(a0), c0y = o.y + radius * std::sin(a0);
    const float c1x = o.x + radius * std::cos(a1), c1y = o.y + radius * std::sin(a1);
    const float mrad = std::sqrt((c0x - ocx) * (c0x - ocx) + (c0y - ocy) * (c0y - ocy));
    if (mrad < 1.f) return;
    float ma0 = std::atan2(c0y - ocy, c0x - ocx);
    float ma1 = std::atan2(c1y - ocy, c1x - ocx);
    // Shortest angular path (the mirrored minor segment)
    float d = ma1 - ma0;
    while (d > PI) d -= 2.f * PI;
    while (d < -PI) d += 2.f * PI;
    bool have = false;
    SDL_Point prev{};
    for (int i = 0; i <= segs; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(segs);
      const float a = ma0 + d * t;
      const SDL_Point cur = W(ocx + mrad * std::cos(a), ocy + mrad * std::sin(a));
      if (have) line(prev.x, prev.y, cur.x, cur.y, col);
      prev = cur;
      have = true;
    }
  };

  // Main expanding ring (dim search wave)
  if (r > 1.f && fade > 0.02f) {
    const int segments = std::clamp(static_cast<int>(r / 8.f), 24, 96);
    auto seg_alpha = [&](float wx, float wy, float base_a) -> uint8_t {
      bool rock = g.cave.is_solid_world(wx, wy);
      if (!rock) {
        const float nx = (wx - o.x) / std::max(r, 1.f), ny = (wy - o.y) / std::max(r, 1.f);
        rock = g.cave.is_solid_world(wx + nx * 4.f, wy + ny * 4.f) ||
               g.cave.is_solid_world(wx - nx * 3.f, wy - ny * 3.f);
      }
      float a = base_a * fade;
      if (rock) {
        const float contact = g.sonar.fading ? (fade * fade * fade) : 0.35f;
        a *= contact;
      }
      return static_cast<uint8_t>(clampf(a, 0.f, 255.f));
    };
    bool have_prev = false;
    SDL_Point prev{};
    for (int i = 0; i <= segments; ++i) {
      const float a = (static_cast<float>(i) / segments) * 6.2831853f;
      const float wx = o.x + r * std::cos(a), wy = o.y + r * std::sin(a);
      const SDL_Point cur = W(wx, wy);
      const uint8_t aa = seg_alpha(wx, wy, 160.f);
      if (aa > 8) {
        if (have_prev) line(prev.x, prev.y, cur.x, cur.y, with_alpha(pal::CARGO, aa));
        prev = cur;
        have_prev = true;
      } else {
        have_prev = false;
      }
    }
  }

  // Reflections: circular segments *reversed* across their chord (bulge toward the
  // origin) so they read as echo returns rather than pieces of the search ring.
  // Returning pulse uses the same mirrored geometry at a shrinking radius.
  constexpr float ARC = 0.35f;  // half-width of the reflected segment (radians)
  for (const SonarReflection& e : g.sonar.echoes) {
    if (e.age > e.life) continue;
    const float life_u = 1.f - e.age / e.life;
    const float ring_bright = life_u * (g.sonar.fading ? fade : 1.f);
    Rgba base = pal::CARGO;
    if (e.kind == SonarReflection::Kind::Pad) base = pal::WARN;
    else if (e.kind == SonarReflection::Kind::Signal) base = pal::BRIGHT;

    const float a0 = e.angle - ARC;
    const float a1 = e.angle + ARC;

    // Bright reversed segment on the expanding wave while the front is near the hit
    if (r > 1.f && std::abs(r - e.hit_r) < 50.f && ring_bright > 0.05f) {
      const uint8_t aa = static_cast<uint8_t>(90 + 165 * ring_bright);
      stroke_mirrored_segment(r, a0, a1, 12, with_alpha(base, aa));
      // Slightly thicker twin, a little farther in
      stroke_mirrored_segment(r - 4.f, a0, a1, 12, with_alpha(base, static_cast<uint8_t>(aa * 0.55f)));
    }

    // Returning reversed segment: same angular width, radius shrinks toward origin
    const float ret_r = e.hit_r - e.age * g.sonar.speed * 0.85f;
    if (ret_r > 8.f && life_u > 0.05f) {
      const uint8_t aa = static_cast<uint8_t>(70 + 170 * life_u);
      stroke_mirrored_segment(ret_r, a0, a1, 12, with_alpha(base, aa));
      stroke_mirrored_segment(ret_r - 4.f, a0, a1, 12, with_alpha(base, static_cast<uint8_t>(aa * 0.5f)));
    }
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
  // Pads: bright = visited (teleport), warn = sonar only, hot = dest
  int dest_pi = -1;
  if (g.ship != NULL_ENTITY && g.ecs.has<Rope>(g.ship) && g.ecs.get<Rope>(g.ship).held != NULL_ENTITY)
    dest_pi = g.ecs.get<Cargo>(g.ecs.get<Rope>(g.ship).held).dest_pad;
  int n_vis = 0, n_act = 0;
  for (int pi = 0; pi < static_cast<int>(g.cave.pads.size()); ++pi) {
    const LandingPad& pad = g.cave.pads[static_cast<size_t>(pi)];
    if (!pad.active) continue;
    ++n_act;
    if (pad.visited) ++n_vis;
    const float cx = 0.5f * (pad.x0 + pad.x1);
    const int px = dx + static_cast<int>(cx / Cave::CELL * scale);
    const int py = dy + static_cast<int>(pad.y / Cave::CELL * scale);
    const int ps = std::max(3, L(5));
    Rgba col = (pi == dest_pi) ? pal::HOT : (pad.visited ? pal::BRIGHT : pal::WARN);
    fill(px - ps / 2, py - ps / 2, ps, ps, col);
    if (!pad.visited)  // hollow look: dark centre for unvisited
      fill(px - ps / 4, py - ps / 4, std::max(1, ps / 2), std::max(1, ps / 2), with_alpha(pal::BG, 220));
  }
  // Found signals on the chart
  for (const Game::Signal& sig : g.signals) {
    if (!sig.found) continue;
    const int px = dx + static_cast<int>(sig.pos.x / Cave::CELL * scale);
    const int py = dy + static_cast<int>(sig.pos.y / Cave::CELL * scale);
    fill(px - 2, py - 2, 4, 4, pal::CARGO);
  }
  int found = 0;
  for (const Game::Signal& s : g.signals) if (s.found) ++found;
  const int pct = explore_percent(g);
  char line[72];
  text_centered(w_ / 2, dy - cell_h() - L(10), "MAP  (HOLD TO PAUSE)", pal::BRIGHT);
  std::snprintf(line, sizeof line, "EXPLORED %d%%   PADS %d/%d   SIGNALS %d/%d   SCORE %d", pct, n_vis,
                n_act, found, static_cast<int>(g.signals.size()), g.score);
  text_centered(w_ / 2, dy + dh + L(8), line, pal::MID);
  text_centered(w_ / 2, dy + dh + L(8) + cell_h() + L(4),
                "PAD: BRIGHT=VISITED  AMBER=FOUND", pal::DIM);
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
  draw_residues(g);
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
    draw_minimap(g, ui, t);
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
