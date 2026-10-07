// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
// Co-authored-by: Grok <grok@x.ai>

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int WINDOW_W = 1280;
constexpr int WINDOW_H = 720;
constexpr float PI = 3.14159265358979323846f;
constexpr float TIME_SCALE = 0.62f;

constexpr float GRAVITY = 120.0f;
constexpr float LINEAR_DRAG = 0.08f;
constexpr float ANGULAR_DRAG = 1.2f;
constexpr float MAX_ANGULAR_VEL = 8.0f;

constexpr float LAND_MAX_VY = 55.0f;
constexpr float LAND_MAX_VX = 40.0f;
constexpr float LAND_MAX_ANGLE = 0.22f;
constexpr float LAND_MAX_ANGVEL = 1.2f;

constexpr SDL_Color CRT_BG{0, 10, 4, 255};
constexpr SDL_Color CRT_DIM{16, 64, 32, 255};
constexpr SDL_Color CRT_MID{40, 180, 80, 255};
constexpr SDL_Color CRT_BRIGHT{140, 255, 160, 255};
constexpr SDL_Color CRT_WARN{220, 200, 60, 255};
constexpr SDL_Color CRT_HOT{255, 120, 40, 255};
constexpr SDL_Color CRT_PAD{80, 220, 140, 255};
constexpr SDL_Color CRT_STAR{60, 140, 80, 255};
constexpr SDL_Color CRT_MENU{20, 40, 24, 230};

struct Vec2 {
  float x = 0.f, y = 0.f;
  Vec2() = default;
  Vec2(float x_, float y_) : x(x_), y(y_) {}
  Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
  Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
  Vec2 operator*(float s) const { return {x * s, y * s}; }
  Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
};

inline float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// ---------------------------------------------------------------------------
// Ship presets
// ---------------------------------------------------------------------------
struct ShipConfig {
  const char* name;
  float half_w, half_h;
  float engine_offset_x, engine_offset_y;
  float mass, inertia, max_thrust;
};

constexpr ShipConfig SHIP_CONFIGS[] = {
    {"Narrow", 22.f, 30.f, 12.f, 20.f, 0.85f, 450.f, 380.f},
    {"Medium", 32.f, 32.f, 20.f, 22.f, 1.0f, 900.f, 400.f},
    {"Wide", 48.f, 28.f, 36.f, 18.f, 1.25f, 1600.f, 420.f},
    {"Barge", 64.f, 26.f, 52.f, 16.f, 1.6f, 2800.f, 440.f},
    {"Long", 26.f, 42.f, 14.f, 30.f, 1.1f, 1100.f, 390.f},
};
constexpr int SHIP_CONFIG_COUNT = static_cast<int>(sizeof(SHIP_CONFIGS) / sizeof(SHIP_CONFIGS[0]));

// ---------------------------------------------------------------------------
// 2D cave world (toroidal in X, tall in Y)
// ---------------------------------------------------------------------------
struct LandingPad {
  float x0, x1, y;  // world surface y of the pad
};

struct Cave {
  static constexpr float WORLD_W = 24000.f;
  static constexpr float WORLD_H = 4800.f;
  static constexpr float CELL = 16.f;
  static constexpr int GW = static_cast<int>(WORLD_W / CELL);  // 1500
  static constexpr int GH = static_cast<int>(WORLD_H / CELL);  // 300

  std::vector<uint8_t> solid;  // GW * GH, 1 = rock
  std::vector<LandingPad> pads;
  std::vector<Vec2> stars;  // background dots in world space
  unsigned seed = 1;

  static float wrap_x(float wx) {
    wx = std::fmod(wx, WORLD_W);
    if (wx < 0.f) wx += WORLD_W;
    return wx;
  }
  static float wrap_delta(float from, float to) {
    float d = to - from;
    d = std::fmod(d + WORLD_W * 1.5f, WORLD_W) - WORLD_W * 0.5f;
    return d;
  }

  int idx(int gx, int gy) const {
    gx = ((gx % GW) + GW) % GW;
    gy = clampf(static_cast<float>(gy), 0.f, static_cast<float>(GH - 1));
    return gy * GW + gx;
  }

  bool is_solid_cell(int gx, int gy) const {
    if (gy < 0 || gy >= GH) return true;  // outside vertical = solid
    gx = ((gx % GW) + GW) % GW;
    return solid[gy * GW + gx] != 0;
  }

  bool is_solid_world(float wx, float wy) const {
    if (wy < 0.f || wy >= WORLD_H) return true;
    int gx = static_cast<int>(wrap_x(wx) / CELL);
    int gy = static_cast<int>(wy / CELL);
    return is_solid_cell(gx, gy);
  }

  // Nearest solid surface below a free point (for ALT readout)
  float floor_below(float wx, float wy) const {
    wx = wrap_x(wx);
    int gx = static_cast<int>(wx / CELL);
    int gy0 = std::max(0, static_cast<int>(wy / CELL));
    for (int gy = gy0; gy < GH; ++gy) {
      if (is_solid_cell(gx, gy))
        return static_cast<float>(gy) * CELL;
    }
    return WORLD_H;
  }

  bool on_pad(float wx, float wy) const {
    wx = wrap_x(wx);
    for (const auto& p : pads) {
      if (wx >= p.x0 && wx <= p.x1 && std::abs(wy - p.y) < CELL * 2.f)
        return true;
    }
    return false;
  }

  void generate(unsigned s) {
    seed = s ? s : 1u;
    solid.assign(GW * GH, 1);
    pads.clear();
    stars.clear();

    unsigned state = seed;
    auto rnd = [&]() -> float {
      state = state * 1664525u + 1013904223u;
      return (state >> 8) / static_cast<float>(1u << 24);
    };
    auto rnd_i = [&](int lo, int hi) -> int {
      return lo + static_cast<int>(rnd() * (hi - lo + 1));
    };

    // --- Main meandering tunnel (floor + ceiling) ---
    float cy = WORLD_H * 0.55f;
    float half = 140.f;
    for (int gx = 0; gx < GW; ++gx) {
      cy += (rnd() - 0.5f) * 18.f;
      cy = clampf(cy, WORLD_H * 0.25f, WORLD_H * 0.85f);
      half += (rnd() - 0.5f) * 12.f;
      half = clampf(half, 70.f, 220.f);
      // occasional chambers
      if (rnd() < 0.02f) half = clampf(half + 80.f, 70.f, 280.f);
      if (rnd() < 0.015f) half = clampf(half - 50.f, 50.f, 280.f);

      int y0 = static_cast<int>((cy - half) / CELL);
      int y1 = static_cast<int>((cy + half) / CELL);
      y0 = std::max(2, y0);
      y1 = std::min(GH - 3, y1);
      for (int gy = y0; gy <= y1; ++gy)
        solid[gy * GW + gx] = 0;
    }

    // --- Branch tunnels (random walks) ---
    for (int b = 0; b < 48; ++b) {
      int gx = rnd_i(0, GW - 1);
      int gy = rnd_i(GH / 5, GH * 4 / 5);
      int len = rnd_i(40, 180);
      int rad = rnd_i(2, 5);
      float dir = rnd() * 2.f * PI;
      for (int sstep = 0; sstep < len; ++sstep) {
        dir += (rnd() - 0.5f) * 0.5f;
        gx = static_cast<int>(gx + std::cos(dir) * 1.2f);
        gy = static_cast<int>(gy + std::sin(dir) * 1.2f);
        gx = ((gx % GW) + GW) % GW;
        gy = clampf(static_cast<float>(gy), 2.f, static_cast<float>(GH - 3));
        for (int dy = -rad; dy <= rad; ++dy)
          for (int dx = -rad; dx <= rad; ++dx) {
            if (dx * dx + dy * dy <= rad * rad) {
              int xx = ((gx + dx) % GW + GW) % GW;
              int yy = gy + dy;
              if (yy >= 1 && yy < GH - 1)
                solid[yy * GW + xx] = 0;
            }
          }
      }
    }

    // --- Stalactites / stalagmites ---
    for (int n = 0; n < 400; ++n) {
      int gx = rnd_i(0, GW - 1);
      bool down = rnd() < 0.5f;
      int len = rnd_i(3, 14);
      // find surface
      if (down) {
        for (int gy = 1; gy < GH - 2; ++gy) {
          if (solid[gy * GW + gx] && !solid[(gy + 1) * GW + gx]) {
            for (int k = 1; k <= len && gy + k < GH - 1; ++k)
              solid[(gy + k) * GW + gx] = 1;
            break;
          }
        }
      } else {
        for (int gy = GH - 2; gy > 1; --gy) {
          if (solid[gy * GW + gx] && !solid[(gy - 1) * GW + gx]) {
            for (int k = 1; k <= len && gy - k > 0; ++k)
              solid[(gy - k) * GW + gx] = 1;
            break;
          }
        }
      }
    }

    // --- Pillars (vertical solid bridges) ---
    for (int n = 0; n < 60; ++n) {
      int gx = rnd_i(0, GW - 1);
      int width = rnd_i(1, 3);
      for (int gy = 2; gy < GH - 2; ++gy) {
        for (int dx = 0; dx < width; ++dx) {
          int xx = (gx + dx) % GW;
          // only fill if nearby open space (keep as obstruction inside cave)
          bool near_open = false;
          for (int o = -3; o <= 3; ++o) {
            int yy = gy + o;
            if (yy >= 0 && yy < GH && !solid[yy * GW + xx]) near_open = true;
          }
          if (near_open && rnd() < 0.7f)
            solid[gy * GW + xx] = 1;
        }
      }
    }

    // Keep a solid crust at top and bottom
    for (int gx = 0; gx < GW; ++gx) {
      solid[0 * GW + gx] = 1;
      solid[1 * GW + gx] = 1;
      solid[(GH - 2) * GW + gx] = 1;
      solid[(GH - 1) * GW + gx] = 1;
    }

    // --- Landing pads: flatten floor segments in open areas ---
    for (int attempt = 0; attempt < 40 && pads.size() < 16; ++attempt) {
      int gx0 = rnd_i(10, GW - 40);
      int width = rnd_i(8, 16);  // cells
      // find floor: first solid below open
      int floor_gy = -1;
      int mid = gx0 + width / 2;
      for (int gy = 2; gy < GH - 2; ++gy) {
        if (!solid[gy * GW + mid] && solid[(gy + 1) * GW + mid]) {
          floor_gy = gy + 1;
          break;
        }
      }
      if (floor_gy < 0) continue;
      // need open air above
      bool ok = true;
      for (int dx = 0; dx < width; ++dx) {
        int xx = (gx0 + dx) % GW;
        if (solid[(floor_gy - 1) * GW + xx] || solid[(floor_gy - 2) * GW + xx])
          ok = false;
      }
      if (!ok) continue;
      for (int dx = 0; dx < width; ++dx) {
        int xx = (gx0 + dx) % GW;
        solid[floor_gy * GW + xx] = 1;
        // clear a few cells above
        for (int uy = 1; uy <= 6; ++uy) {
          int yy = floor_gy - uy;
          if (yy > 1) solid[yy * GW + xx] = 0;
        }
      }
      float x0 = static_cast<float>(gx0) * CELL;
      float x1 = static_cast<float>(gx0 + width) * CELL;
      float y = static_cast<float>(floor_gy) * CELL;
      pads.push_back({x0, x1, y});
    }
    if (pads.empty()) {
      // emergency pad
      int gx0 = GW / 2;
      int floor_gy = GH * 2 / 3;
      for (int dx = 0; dx < 12; ++dx) {
        int xx = (gx0 + dx) % GW;
        solid[floor_gy * GW + xx] = 1;
        for (int uy = 1; uy <= 8; ++uy)
          solid[(floor_gy - uy) * GW + xx] = 0;
      }
      pads.push_back({gx0 * CELL, (gx0 + 12) * CELL, floor_gy * CELL});
    }

    // Background stars (dots) scattered in open space + some in rock (dim depth)
    for (int i = 0; i < 2200; ++i) {
      float sx = rnd() * WORLD_W;
      float sy = rnd() * WORLD_H;
      stars.push_back({sx, sy});
    }
  }
};

// ---------------------------------------------------------------------------
// Ship
// ---------------------------------------------------------------------------
enum class FlightState { Flying, Landed, Crashed };

struct Ship {
  Vec2 pos, vel;
  float angle = 0.f, ang_vel = 0.f;
  float left_thrust = 0.f, right_thrust = 0.f;
  bool swap_engines = false;
  int config_index = 1;
  const ShipConfig* cfg = &SHIP_CONFIGS[1];
  FlightState state = FlightState::Flying;
  float state_timer = 0.f;

  void set_config(int index) {
    if (index < 0) index = SHIP_CONFIG_COUNT - 1;
    if (index >= SHIP_CONFIG_COUNT) index = 0;
    config_index = index;
    cfg = &SHIP_CONFIGS[config_index];
    ang_vel *= 0.5f;
    vel = vel * 0.7f;
  }
  void cycle_config(int d) { set_config(config_index + d); }

  void spawn(const Cave& cave, float wx) {
    wx = Cave::wrap_x(wx);
    float ground = cave.floor_below(wx, 0.f);
    // Prefer pad center if near one
    for (const auto& p : cave.pads) {
      if (std::abs(Cave::wrap_delta(wx, 0.5f * (p.x0 + p.x1))) < 200.f) {
        wx = 0.5f * (p.x0 + p.x1);
        ground = p.y;
        break;
      }
    }
    pos = {wx, ground - 200.f};
    // Ensure spawn is in open air
    for (int tries = 0; tries < 50 && cave.is_solid_world(pos.x, pos.y); ++tries)
      pos.y -= Cave::CELL;
    vel = {};
    angle = 0.f;
    ang_vel = 0.f;
    left_thrust = right_thrust = 0.f;
    state = FlightState::Flying;
    state_timer = 0.f;
  }

  Vec2 to_world(Vec2 local) const {
    float c = std::cos(angle), s = std::sin(angle);
    return {pos.x + c * local.x - s * local.y, pos.y + s * local.x + c * local.y};
  }

  void update_physics(float dt) {
    if (state != FlightState::Flying) return;
    const float ox = cfg->engine_offset_x, oy = cfg->engine_offset_y;
    auto rotate = [this](Vec2 v) {
      float c = std::cos(angle), s = std::sin(angle);
      return Vec2{c * v.x - s * v.y, s * v.x + c * v.y};
    };
    Vec2 force{};
    float torque = 0.f;
    Vec2 thrust_dir{0.f, -1.f};
    if (left_thrust > 0.f) {
      Vec2 f = rotate(thrust_dir) * (cfg->max_thrust * left_thrust);
      force += f;
      Vec2 r = rotate({-ox, oy});
      torque += r.x * f.y - r.y * f.x;
    }
    if (right_thrust > 0.f) {
      Vec2 f = rotate(thrust_dir) * (cfg->max_thrust * right_thrust);
      force += f;
      Vec2 r = rotate({ox, oy});
      torque += r.x * f.y - r.y * f.x;
    }
    force.y += GRAVITY * cfg->mass;
    vel += force * (1.f / cfg->mass) * dt;
    vel = vel * std::max(0.f, 1.f - LINEAR_DRAG * dt);
    pos += vel * dt;
    pos.x = Cave::wrap_x(pos.x);
    ang_vel += (torque / cfg->inertia) * dt;
    ang_vel *= std::max(0.f, 1.f - ANGULAR_DRAG * dt);
    ang_vel = clampf(ang_vel, -MAX_ANGULAR_VEL, MAX_ANGULAR_VEL);
    angle += ang_vel * dt;
    while (angle > PI) angle -= 2.f * PI;
    while (angle < -PI) angle += 2.f * PI;
  }

  void collide(const Cave& cave) {
    if (state != FlightState::Flying) return;
    const float ox = cfg->engine_offset_x, oy = cfg->engine_offset_y;
    Vec2 probes[5] = {
        {-ox, oy + 4.f}, {0.f, oy + 6.f}, {ox, oy + 4.f},
        {-ox * 0.5f, oy + 2.f}, {ox * 0.5f, oy + 2.f},
    };
    float max_pen = 0.f;
    float contact_x = pos.x, contact_y = pos.y;
    bool any = false;
    for (const auto& lp : probes) {
      Vec2 wp = to_world(lp);
      if (cave.is_solid_world(wp.x, wp.y)) {
        any = true;
        // push up out of solid
        float y = wp.y;
        while (cave.is_solid_world(wp.x, y) && y > 0.f) y -= 2.f;
        float pen = wp.y - y;
        if (pen > max_pen) {
          max_pen = pen;
          contact_x = Cave::wrap_x(wp.x);
          contact_y = y;
        }
      }
    }
    // also nose / sides for crashes into walls
    Vec2 body[3] = {{0.f, -cfg->half_h * 0.8f}, {-cfg->half_w * 0.5f, 0.f}, {cfg->half_w * 0.5f, 0.f}};
    bool wall_hit = false;
    for (const auto& lp : body) {
      Vec2 wp = to_world(lp);
      if (cave.is_solid_world(wp.x, wp.y)) wall_hit = true;
    }

    if (!any && !wall_hit) return;

    if (any) pos.y -= max_pen;

    const bool pad = cave.on_pad(contact_x, contact_y + Cave::CELL);
    const bool gentle = vel.y < LAND_MAX_VY && std::abs(vel.x) < LAND_MAX_VX &&
                        std::abs(angle) < LAND_MAX_ANGLE && std::abs(ang_vel) < LAND_MAX_ANGVEL;

    if (pad && gentle && !wall_hit) {
      state = FlightState::Landed;
      state_timer = 0.f;
      vel = {};
      ang_vel = 0.f;
      angle = 0.f;
      std::printf("LANDED\n");
    } else {
      state = FlightState::Crashed;
      state_timer = 0.f;
      vel = {};
      ang_vel = 0.f;
      std::printf("CRASH\n");
    }
  }
};

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------
struct Camera {
  float x = 0.f, y = 0.f;
  float prev_ship_x = 0.f;
  bool have_prev = false;

  void follow(const Ship& ship, float dt) {
    if (have_prev) {
      float d = ship.pos.x - prev_ship_x;
      if (d > Cave::WORLD_W * 0.5f) x -= Cave::WORLD_W;
      else if (d < -Cave::WORLD_W * 0.5f) x += Cave::WORLD_W;
    }
    prev_ship_x = ship.pos.x;
    have_prev = true;
    float target_x = ship.pos.x - WINDOW_W * 0.5f;
    float target_y = ship.pos.y - WINDOW_H * 0.55f;
    float k = 1.f - std::exp(-6.f * dt);
    x += (target_x - x) * k;
    y += (target_y - y) * k;
    y = clampf(y, -100.f, Cave::WORLD_H - WINDOW_H * 0.3f);
  }

  float continuous_x(float wx) const {
    float cam_center = x + WINDOW_W * 0.5f;
    float d = Cave::wrap_delta(cam_center, Cave::wrap_x(wx));
    return cam_center + d;
  }

  SDL_Point to_screen(float wx, float wy) const {
    float sx = continuous_x(wx) - x;
    return {static_cast<int>(sx + 0.5f), static_cast<int>(wy - y + 0.5f)};
  }
};

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
enum class AppMode { Playing, Menu };

struct Menu {
  int cursor = 0;
  // 0 Resume, 1 Fullscreen, 2 New cave, 3 Ship, 4 Swap engines, 5 Quit
  static constexpr int N = 6;
  const char* labels[N] = {
      "RESUME", "FULLSCREEN", "NEW CAVE", "SHIP PRESET", "SWAP ENGINES", "QUIT",
  };
};

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void set_color(SDL_Renderer* ren, SDL_Color c, Uint8 a = 255) {
  SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, a);
}

void draw_char(SDL_Renderer* ren, int x, int y, char ch, SDL_Color col) {
  const char* map = "111101101101111";
  switch (ch) {
    case '0': map = "111101101101111"; break;
    case '1': map = "010010010010010"; break;
    case '2': map = "111001111100111"; break;
    case '3': map = "111001111001111"; break;
    case '4': map = "101101111001001"; break;
    case '5': map = "111100111001111"; break;
    case '6': map = "111100111101111"; break;
    case '7': map = "111001001001001"; break;
    case '8': map = "111101111101111"; break;
    case '9': map = "111101111001111"; break;
    case 'A': map = "010101111101101"; break;
    case 'B': map = "110101110101110"; break;
    case 'C': map = "011100100100011"; break;
    case 'D': map = "110101101101110"; break;
    case 'E': map = "111100110100111"; break;
    case 'F': map = "111100110100100"; break;
    case 'G': map = "011100101101011"; break;
    case 'H': map = "101101111101101"; break;
    case 'I': map = "111010010010111"; break;
    case 'K': map = "101110110101101"; break;
    case 'L': map = "100100100100111"; break;
    case 'M': map = "101111111101101"; break;
    case 'N': map = "101111111111101"; break;
    case 'O': map = "010101101101010"; break;
    case 'P': map = "110101110100100"; break;
    case 'Q': map = "010101101111011"; break;
    case 'R': map = "110101110101101"; break;
    case 'S': map = "011100010001110"; break;
    case 'T': map = "111010010010010"; break;
    case 'U': map = "101101101101111"; break;
    case 'V': map = "101101101101010"; break;
    case 'W': map = "101101111111101"; break;
    case 'X': map = "101101010101101"; break;
    case 'Y': map = "101101010010010"; break;
    case '-': map = "000000111000000"; break;
    case '.': map = "000000000010010"; break;
    case ':': map = "000010000010000"; break;
    case '>': map = "100010001010100"; break;
    case ' ': map = "000000000000000"; break;
    default: break;
  }
  set_color(ren, col);
  for (int row = 0; row < 5; ++row)
    for (int colx = 0; colx < 3; ++colx)
      if (map[row * 3 + colx] == '1')
        SDL_RenderDrawPoint(ren, x + colx, y + row);
}

void draw_text(SDL_Renderer* ren, int x, int y, const char* s, SDL_Color col) {
  for (int i = 0; s[i]; ++i)
    draw_char(ren, x + i * 4, y, s[i], col);
}

void draw_stars(SDL_Renderer* ren, const Cave& cave, const Camera& cam) {
  for (const auto& st : cave.stars) {
    // parallax: slight offset by depth layer from y
    float parallax = 0.85f + 0.1f * std::fmod(st.y * 0.01f, 1.f);
    float wx = Cave::wrap_x(st.x);
    // only if roughly in view (open space preferred)
    float sx = cam.continuous_x(wx) - cam.x;
    float sy = st.y * parallax - cam.y * parallax;
    // remap sy roughly
    sy = st.y - cam.y;
    if (sx < -2 || sx > WINDOW_W + 2 || sy < -2 || sy > WINDOW_H + 2) continue;
    // dimmer if inside rock
    bool rock = cave.is_solid_world(wx, st.y);
    set_color(ren, CRT_STAR, rock ? 40 : 200);
    SDL_RenderDrawPoint(ren, static_cast<int>(sx), static_cast<int>(sy));
  }
}

void draw_cave(SDL_Renderer* ren, const Cave& cave, const Camera& cam) {
  int gx0 = static_cast<int>(std::floor(cam.x / Cave::CELL)) - 1;
  int gx1 = static_cast<int>(std::ceil((cam.x + WINDOW_W) / Cave::CELL)) + 1;
  int gy0 = std::max(0, static_cast<int>(std::floor(cam.y / Cave::CELL)) - 1);
  int gy1 = std::min(Cave::GH - 1, static_cast<int>(std::ceil((cam.y + WINDOW_H) / Cave::CELL)) + 1);

  // Fill solid cells
  for (int gy = gy0; gy <= gy1; ++gy) {
    for (int gx = gx0; gx <= gx1; ++gx) {
      if (!cave.is_solid_cell(gx, gy)) continue;
      float wx = static_cast<float>(gx) * Cave::CELL;
      float wy = static_cast<float>(gy) * Cave::CELL;
      float sx = cam.continuous_x(Cave::wrap_x(wx)) - cam.x;
      // keep consecutive cells aligned
      float sx0 = (static_cast<float>(gx) * Cave::CELL) - cam.x;
      // Use continuous from first column of view
      sx0 = cam.continuous_x(Cave::wrap_x(static_cast<float>(gx0) * Cave::CELL)) - cam.x
            + static_cast<float>(gx - gx0) * Cave::CELL;
      int ix = static_cast<int>(sx0);
      int iy = static_cast<int>(wy - cam.y);
      // edge highlight vs fill
      bool edge = false;
      if (!cave.is_solid_cell(gx - 1, gy) || !cave.is_solid_cell(gx + 1, gy) ||
          !cave.is_solid_cell(gx, gy - 1) || !cave.is_solid_cell(gx, gy + 1))
        edge = true;
      if (edge) {
        set_color(ren, CRT_BRIGHT, 220);
        SDL_Rect r{ix, iy, static_cast<int>(Cave::CELL), static_cast<int>(Cave::CELL)};
        SDL_RenderDrawRect(ren, &r);
      } else {
        set_color(ren, CRT_DIM, 120);
        SDL_Rect r{ix, iy, static_cast<int>(Cave::CELL), static_cast<int>(Cave::CELL)};
        SDL_RenderFillRect(ren, &r);
      }
      (void)sx;
    }
  }

  // Pad markers
  for (const auto& p : cave.pads) {
    float sx0 = cam.continuous_x(p.x0) - cam.x;
    float sx1 = cam.continuous_x(p.x1) - cam.x;
    // if pad wraps awkwardly, draw via continuous from x0
    sx1 = sx0 + (p.x1 - p.x0);
    int y = static_cast<int>(p.y - cam.y);
    set_color(ren, CRT_PAD);
    SDL_RenderDrawLine(ren, static_cast<int>(sx0), y, static_cast<int>(sx1), y);
    SDL_RenderDrawLine(ren, static_cast<int>(sx0), y + 1, static_cast<int>(sx1), y + 1);
    SDL_RenderDrawLine(ren, static_cast<int>(sx0), y, static_cast<int>(sx0), y + 10);
    SDL_RenderDrawLine(ren, static_cast<int>(sx1), y, static_cast<int>(sx1), y + 10);
  }
}

void draw_ship_vector(SDL_Renderer* ren, const Ship& ship, const Camera& cam) {
  const float hw = ship.cfg->half_w * 0.85f;
  const float hh = ship.cfg->half_h * 0.9f;
  const float ox = ship.cfg->engine_offset_x, oy = ship.cfg->engine_offset_y;
  auto W = [&](Vec2 l) {
    Vec2 w = ship.to_world(l);
    return cam.to_screen(w.x, w.y);
  };
  SDL_Color body = ship.state == FlightState::Crashed ? CRT_HOT
                   : ship.state == FlightState::Landed  ? CRT_PAD
                                                       : CRT_BRIGHT;
  auto line = [&](Vec2 a, Vec2 b) {
    SDL_Point pa = W(a), pb = W(b);
    set_color(ren, body);
    SDL_RenderDrawLine(ren, pa.x, pa.y, pb.x, pb.y);
  };
  line({0.f, -hh}, {-hw * 0.7f, hh * 0.35f});
  line({0.f, -hh}, {hw * 0.7f, hh * 0.35f});
  line({-hw * 0.7f, hh * 0.35f}, {hw * 0.7f, hh * 0.35f});
  line({-ox - 6.f, oy - 4.f}, {-ox + 6.f, oy - 4.f});
  line({-ox - 6.f, oy - 4.f}, {-ox - 4.f, oy + 8.f});
  line({-ox + 6.f, oy - 4.f}, {-ox + 4.f, oy + 8.f});
  line({ox - 6.f, oy - 4.f}, {ox + 6.f, oy - 4.f});
  line({ox - 6.f, oy - 4.f}, {ox - 4.f, oy + 8.f});
  line({ox + 6.f, oy - 4.f}, {ox + 4.f, oy + 8.f});
  if (ship.state == FlightState::Flying) {
    Vec2 tip = ship.to_world({0.f, -hh - 12.f});
    SDL_Point p0 = cam.to_screen(ship.pos.x, ship.pos.y);
    SDL_Point p1 = cam.to_screen(tip.x, tip.y);
    set_color(ren, CRT_WARN);
    SDL_RenderDrawLine(ren, p0.x, p0.y, p1.x, p1.y);
  }
}

void draw_exhaust(SDL_Renderer* ren, const Ship& ship, const Camera& cam, bool left) {
  if (ship.state != FlightState::Flying) return;
  float t = left ? ship.left_thrust : ship.right_thrust;
  if (t < 0.05f) return;
  float lx = left ? -ship.cfg->engine_offset_x : ship.cfg->engine_offset_x;
  Vec2 base = ship.to_world({lx, ship.cfg->engine_offset_y + 10.f});
  float c = std::cos(ship.angle), s = std::sin(ship.angle);
  float ex = -s, ey = c;
  int len = static_cast<int>(10 + t * 36);
  for (int i = 0; i < len; i += 2) {
    SDL_Point p = cam.to_screen(base.x + ex * i, base.y + ey * i);
    set_color(ren, i < len / 2 ? CRT_BRIGHT : CRT_HOT);
    SDL_RenderDrawPoint(ren, p.x, p.y);
  }
}

void draw_hud(SDL_Renderer* ren, const Ship& ship, const Cave& cave) {
  auto bar = [&](int x, int y, float v) {
    set_color(ren, CRT_DIM);
    SDL_Rect bg{x, y, 100, 8};
    SDL_RenderFillRect(ren, &bg);
    set_color(ren, CRT_MID);
    SDL_Rect fg{x, y, static_cast<int>(100 * clampf(v, 0.f, 1.f)), 8};
    SDL_RenderFillRect(ren, &fg);
  };
  bar(20, 20, ship.left_thrust);
  bar(20, 32, ship.right_thrust);
  draw_text(ren, 20, 46, ship.cfg->name, CRT_BRIGHT);
  draw_text(ren, 20, 54, ship.swap_engines ? "ENGINES SWAPPED" : "START MENU",
            ship.swap_engines ? CRT_WARN : CRT_DIM);

  char buf[64];
  float alt = cave.floor_below(ship.pos.x, ship.pos.y) - ship.pos.y;
  std::snprintf(buf, sizeof(buf), "ALT %.0f", alt);
  draw_text(ren, 20, 72, buf, CRT_BRIGHT);
  std::snprintf(buf, sizeof(buf), "VX %.0f VY %.0f", ship.vel.x, ship.vel.y);
  draw_text(ren, 20, 80, buf, CRT_MID);

  bool pad = false;
  for (const auto& p : cave.pads) {
    if (std::abs(Cave::wrap_delta(ship.pos.x, 0.5f * (p.x0 + p.x1))) < (p.x1 - p.x0) &&
        std::abs(alt) < 120.f)
      pad = true;
  }
  bool ok_v = ship.vel.y < LAND_MAX_VY && std::abs(ship.vel.x) < LAND_MAX_VX;
  bool ok_a = std::abs(ship.angle) < LAND_MAX_ANGLE;
  draw_text(ren, WINDOW_W - 120, 20, pad ? "PAD OK" : "NO PAD", pad ? CRT_PAD : CRT_HOT);
  draw_text(ren, WINDOW_W - 120, 28, ok_v ? "SPEED OK" : "SPEED HI", ok_v ? CRT_PAD : CRT_HOT);
  draw_text(ren, WINDOW_W - 120, 36, ok_a ? "ATT OK" : "ATT BAD", ok_a ? CRT_PAD : CRT_HOT);

  if (ship.state == FlightState::Landed) {
    draw_text(ren, WINDOW_W / 2 - 40, 40, "LANDED", CRT_PAD);
    draw_text(ren, WINDOW_W / 2 - 70, 50, "A TO RELIGHT", CRT_MID);
  } else if (ship.state == FlightState::Crashed) {
    draw_text(ren, WINDOW_W / 2 - 40, 40, "CRASH", CRT_HOT);
    draw_text(ren, WINDOW_W / 2 - 70, 50, "A TO RESET", CRT_MID);
  }
}

void draw_menu(SDL_Renderer* ren, const Menu& menu, const Ship& ship, bool fullscreen) {
  set_color(ren, CRT_MENU);
  SDL_Rect panel{WINDOW_W / 2 - 160, WINDOW_H / 2 - 120, 320, 240};
  SDL_RenderFillRect(ren, &panel);
  set_color(ren, CRT_BRIGHT);
  SDL_RenderDrawRect(ren, &panel);

  draw_text(ren, WINDOW_W / 2 - 50, WINDOW_H / 2 - 108, "DUALTHRUST", CRT_BRIGHT);
  draw_text(ren, WINDOW_W / 2 - 70, WINDOW_H / 2 - 96, "CRT CAVE LANDER", CRT_DIM);

  for (int i = 0; i < Menu::N; ++i) {
    char line[48];
    if (i == 1)
      std::snprintf(line, sizeof(line), "%s %s", menu.labels[i], fullscreen ? "ON" : "OFF");
    else if (i == 3)
      std::snprintf(line, sizeof(line), "%s %s", menu.labels[i], ship.cfg->name);
    else if (i == 4)
      std::snprintf(line, sizeof(line), "%s %s", menu.labels[i], ship.swap_engines ? "ON" : "OFF");
    else
      std::snprintf(line, sizeof(line), "%s", menu.labels[i]);
    SDL_Color col = (i == menu.cursor) ? CRT_WARN : CRT_MID;
    if (i == menu.cursor)
      draw_text(ren, WINDOW_W / 2 - 90, WINDOW_H / 2 - 70 + i * 16, ">", CRT_WARN);
    draw_text(ren, WINDOW_W / 2 - 80, WINDOW_H / 2 - 70 + i * 16, line, col);
  }
  draw_text(ren, WINDOW_W / 2 - 100, WINDOW_H / 2 + 100, "UP DOWN MOVE  A SELECT", CRT_DIM);
}

void draw_scanlines(SDL_Renderer* ren) {
  for (int y = 0; y < WINDOW_H; y += 3) {
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 45);
    SDL_RenderDrawLine(ren, 0, y, WINDOW_W, y);
  }
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  SDL_Window* window = SDL_CreateWindow(
      "dualthrust", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_W, WINDOW_H,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (!window) {
    SDL_Quit();
    return 1;
  }
  SDL_Renderer* ren =
      SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren) {
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

  Cave cave;
  std::printf("Generating cave...\n");
  cave.generate(0xC0FFEE);
  std::printf("Cave ready (%d pads)\n", (int)cave.pads.size());

  Ship ship;
  ship.set_config(1);
  float start_x = 0.5f * (cave.pads[0].x0 + cave.pads[0].x1);
  ship.spawn(cave, start_x);

  Camera cam;
  auto snap_camera = [&]() {
    cam.x = ship.pos.x - WINDOW_W * 0.5f;
    cam.y = ship.pos.y - WINDOW_H * 0.55f;
    cam.prev_ship_x = ship.pos.x;
    cam.have_prev = true;
  };
  snap_camera();

  AppMode mode = AppMode::Menu;  // start in menu
  Menu menu;
  bool fullscreen = false;
  bool running = true;

  SDL_GameController* pad = nullptr;
  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    if (SDL_IsGameController(i)) {
      pad = SDL_GameControllerOpen(i);
      if (pad) break;
    }
  }

  bool key_left = false, key_right = false;
  Uint64 prev = SDL_GetPerformanceCounter();
  const Uint64 freq = SDL_GetPerformanceFrequency();

  auto toggle_fullscreen = [&]() {
    fullscreen = !fullscreen;
    SDL_SetWindowFullscreen(window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  };

  auto reset_or_relight = [&]() {
    if (ship.state == FlightState::Landed) {
      ship.state = FlightState::Flying;
      ship.vel.y = -30.f;
    } else {
      int pi = static_cast<int>(SDL_GetTicks() % cave.pads.size());
      float cx = 0.5f * (cave.pads[pi].x0 + cave.pads[pi].x1);
      ship.spawn(cave, cx);
      snap_camera();
    }
  };

  auto activate_menu = [&]() {
    switch (menu.cursor) {
      case 0: mode = AppMode::Playing; break;
      case 1: toggle_fullscreen(); break;
      case 2:
        cave.generate(SDL_GetTicks());
        ship.spawn(cave, 0.5f * (cave.pads[0].x0 + cave.pads[0].x1));
        snap_camera();
        mode = AppMode::Playing;
        break;
      case 3: ship.cycle_config(+1); break;
      case 4: ship.swap_engines = !ship.swap_engines; break;
      case 5: running = false; break;
    }
  };

  while (running) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT) running = false;
      if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) {
        if (mode == AppMode::Playing) mode = AppMode::Menu;
        else running = false;
      }

      if (ev.type == SDL_KEYDOWN) {
        if (mode == AppMode::Menu) {
          if (ev.key.keysym.sym == SDLK_UP || ev.key.keysym.sym == SDLK_w)
            menu.cursor = (menu.cursor + Menu::N - 1) % Menu::N;
          if (ev.key.keysym.sym == SDLK_DOWN || ev.key.keysym.sym == SDLK_s)
            menu.cursor = (menu.cursor + 1) % Menu::N;
          if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_SPACE)
            activate_menu();
        } else {
          if (ev.key.keysym.sym == SDLK_a || ev.key.keysym.sym == SDLK_LEFT) key_left = true;
          if (ev.key.keysym.sym == SDLK_d || ev.key.keysym.sym == SDLK_RIGHT) key_right = true;
          if (ev.key.keysym.sym == SDLK_TAB) ship.cycle_config(+1);
          if (ev.key.keysym.sym == SDLK_x) ship.swap_engines = !ship.swap_engines;
          if (ev.key.keysym.sym == SDLK_r) reset_or_relight();
          if (ev.key.keysym.sym == SDLK_f) toggle_fullscreen();
          if (ev.key.keysym.sym == SDLK_g) {
            cave.generate(SDL_GetTicks());
            ship.spawn(cave, 0.5f * (cave.pads[0].x0 + cave.pads[0].x1));
            snap_camera();
          }
        }
      }
      if (ev.type == SDL_KEYUP) {
        if (ev.key.keysym.sym == SDLK_a || ev.key.keysym.sym == SDLK_LEFT) key_left = false;
        if (ev.key.keysym.sym == SDLK_d || ev.key.keysym.sym == SDLK_RIGHT) key_right = false;
      }

      if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
        if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
          if (mode == AppMode::Playing) mode = AppMode::Menu;
          else mode = AppMode::Playing;
        }
        if (mode == AppMode::Menu) {
          if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
            menu.cursor = (menu.cursor + Menu::N - 1) % Menu::N;
          if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
            menu.cursor = (menu.cursor + 1) % Menu::N;
          if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_A)
            activate_menu();
          if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_B)
            mode = AppMode::Playing;
        } else {
          if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) ship.cycle_config(+1);
          if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_A ||
              ev.cbutton.button == SDL_CONTROLLER_BUTTON_B)
            reset_or_relight();
          if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_Y) {
            cave.generate(SDL_GetTicks());
            ship.spawn(cave, 0.5f * (cave.pads[0].x0 + cave.pads[0].x1));
            snap_camera();
          }
        }
      }

      if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad)
        pad = SDL_GameControllerOpen(ev.cdevice.which);
      if (ev.type == SDL_CONTROLLERDEVICEREMOVED && pad) {
        if (ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
          SDL_GameControllerClose(pad);
          pad = nullptr;
        }
      }
    }

    float lt = 0.f, rt = 0.f;
    if (mode == AppMode::Playing) {
      if (pad) {
        lt = clampf(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.f, 0.f, 1.f);
        rt = clampf(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.f, 0.f, 1.f);
      }
      if (key_left) lt = 1.f;
      if (key_right) rt = 1.f;
    }
    if (ship.swap_engines) {
      ship.left_thrust = rt;
      ship.right_thrust = lt;
    } else {
      ship.left_thrust = lt;
      ship.right_thrust = rt;
    }

    Uint64 now = SDL_GetPerformanceCounter();
    float dt = static_cast<float>(now - prev) / static_cast<float>(freq);
    prev = now;
    dt = std::min(dt, 0.05f);

    if (mode == AppMode::Playing) {
      float sdt = dt * TIME_SCALE;
      ship.update_physics(sdt);
      ship.collide(cave);
      if (ship.state != FlightState::Flying) ship.state_timer += sdt;
      cam.follow(ship, dt);
    }

    set_color(ren, CRT_BG);
    SDL_RenderClear(ren);
    draw_stars(ren, cave, cam);
    draw_cave(ren, cave, cam);
    draw_exhaust(ren, ship, cam, true);
    draw_exhaust(ren, ship, cam, false);
    draw_ship_vector(ren, ship, cam);
    if (ship.state == FlightState::Crashed) {
      float flash = 0.5f + 0.5f * std::sin(ship.state_timer * 20.f);
      set_color(ren, CRT_HOT, static_cast<Uint8>(30 + 60 * flash));
      SDL_Rect full{0, 0, WINDOW_W, WINDOW_H};
      SDL_RenderFillRect(ren, &full);
    }
    draw_hud(ren, ship, cave);
    if (mode == AppMode::Menu)
      draw_menu(ren, menu, ship, fullscreen);
    draw_scanlines(ren);
    SDL_RenderPresent(ren);
  }

  if (pad) SDL_GameControllerClose(pad);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
