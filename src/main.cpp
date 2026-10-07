// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
// Co-authored-by: Grok <grok@x.ai>

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int WINDOW_W = 1280;
constexpr int WINDOW_H = 720;
constexpr float PI = 3.14159265358979323846f;

// Wall-clock → sim-time scale (< 1 slows the whole simulation)
constexpr float TIME_SCALE = 0.62f;

constexpr float GRAVITY = 120.0f;
constexpr float LINEAR_DRAG = 0.08f;
constexpr float ANGULAR_DRAG = 1.2f;
constexpr float MAX_ANGULAR_VEL = 8.0f;

// Landing thresholds (sim units)
constexpr float LAND_MAX_VY = 55.0f;   // downward speed
constexpr float LAND_MAX_VX = 40.0f;
constexpr float LAND_MAX_ANGLE = 0.22f;  // ~12.5°
constexpr float LAND_MAX_ANGVEL = 1.2f;

// CRT phosphor palette
constexpr SDL_Color CRT_BG{0, 12, 4, 255};
constexpr SDL_Color CRT_DIM{20, 80, 40, 255};
constexpr SDL_Color CRT_MID{40, 180, 80, 255};
constexpr SDL_Color CRT_BRIGHT{140, 255, 160, 255};
constexpr SDL_Color CRT_WARN{220, 200, 60, 255};
constexpr SDL_Color CRT_HOT{255, 120, 40, 255};
constexpr SDL_Color CRT_PAD{80, 220, 140, 255};

struct Vec2 {
  float x = 0.f;
  float y = 0.f;
  Vec2() = default;
  Vec2(float x_, float y_) : x(x_), y(y_) {}
  Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
  Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
  Vec2 operator*(float s) const { return {x * s, y * s}; }
  Vec2& operator+=(Vec2 o) {
    x += o.x;
    y += o.y;
    return *this;
  }
};

inline float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// ---------------------------------------------------------------------------
// Ship presets
// ---------------------------------------------------------------------------
struct ShipConfig {
  const char* name;
  float half_w;
  float half_h;
  float engine_offset_x;
  float engine_offset_y;
  float mass;
  float inertia;
  float max_thrust;
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
// Fractal terrain (midpoint displacement) + landing pads
// ---------------------------------------------------------------------------
struct LandingPad {
  float x0, x1;  // world x range
};

struct Terrain {
  // Large toroidal world: X wraps. Height samples form a seamless ring.
  static constexpr float WORLD_W = 24000.f;
  static constexpr float STEP = 8.f;
  // One sample per STEP; last sample coincides with x=0 for seamless wrap.
  static constexpr int COUNT = static_cast<int>(WORLD_W / STEP);

  std::vector<float> h;  // height at sample i → world y of surface
  std::vector<LandingPad> pads;
  unsigned seed = 1;

  static float wrap_x(float wx) {
    wx = std::fmod(wx, WORLD_W);
    if (wx < 0.f)
      wx += WORLD_W;
    return wx;
  }

  // Shortest signed delta on the ring (result in (-WORLD_W/2, WORLD_W/2])
  static float wrap_delta(float from, float to) {
    float d = to - from;
    d = std::fmod(d + WORLD_W * 1.5f, WORLD_W) - WORLD_W * 0.5f;
    return d;
  }

  float x_at(int i) const { return static_cast<float>(i) * STEP; }

  int sample_index(float wx) const {
    wx = wrap_x(wx);
    int i = static_cast<int>(wx / STEP) % COUNT;
    if (i < 0)
      i += COUNT;
    return i;
  }

  float height_at(float wx) const {
    wx = wrap_x(wx);
    float t = wx / STEP;
    int i0 = static_cast<int>(t) % COUNT;
    if (i0 < 0)
      i0 += COUNT;
    int i1 = (i0 + 1) % COUNT;
    float f = t - std::floor(t);
    return lerpf(h[i0], h[i1], f);
  }

  float slope_at(float wx) const {
    const float d = STEP;
    return (height_at(wx + d) - height_at(wx - d)) / (2.f * d);
  }

  bool on_pad(float wx) const {
    wx = wrap_x(wx);
    for (const auto& p : pads) {
      if (wx >= p.x0 && wx <= p.x1)
        return true;
    }
    return false;
  }

  void generate(unsigned s) {
    seed = s;
    h.assign(COUNT, 0.f);
    pads.clear();

    unsigned state = seed ? seed : 1u;
    auto rnd = [&]() -> float {
      state = state * 1664525u + 1013904223u;
      return (state >> 8) / static_cast<float>(1u << 24);
    };

    // Seed all samples with low-frequency base, then diamond-style midpoints on a ring
    for (int i = 0; i < COUNT; ++i)
      h[i] = 500.f + (rnd() * 2.f - 1.f) * 40.f;

    // Power-of-two style passes wrapping around the ring
    int step = COUNT;
    // Find largest power-of-two <= COUNT
    int po2 = 1;
    while (po2 * 2 <= COUNT)
      po2 *= 2;
    step = po2;
    float amp = 260.f;
    while (step > 1) {
      int half = step / 2;
      for (int i = 0; i < COUNT; i += step) {
        int i0 = i % COUNT;
        int i1 = (i + step) % COUNT;
        int mid = (i + half) % COUNT;
        float avg = 0.5f * (h[i0] + h[i1]);
        h[mid] = clampf(avg + (rnd() * 2.f - 1.f) * amp, 260.f, 720.f);
      }
      step = half;
      amp *= 0.55f;
    }

    // More pads across the larger ring (stay clear of the x=0 seam)
    struct PadSpec {
      float center_frac;
      float width;
    };
    const PadSpec specs[] = {
        {0.06f, 160.f}, {0.14f, 130.f}, {0.22f, 150.f}, {0.30f, 120.f},
        {0.38f, 180.f}, {0.46f, 140.f}, {0.54f, 160.f}, {0.62f, 130.f},
        {0.70f, 170.f}, {0.78f, 140.f}, {0.86f, 150.f}, {0.94f, 120.f},
    };
    for (const auto& sp : specs) {
      float cx = sp.center_frac * WORLD_W;
      float x0 = cx - sp.width * 0.5f;
      float x1 = cx + sp.width * 0.5f;
      float sum = 0.f;
      int n = 0;
      int i0 = static_cast<int>(x0 / STEP);
      int i1 = static_cast<int>(x1 / STEP);
      for (int i = i0; i <= i1; ++i) {
        int idx = ((i % COUNT) + COUNT) % COUNT;
        sum += h[idx];
        ++n;
      }
      float flat = (n > 0) ? sum / n : 500.f;
      flat -= 8.f;
      for (int i = i0; i <= i1; ++i) {
        int idx = ((i % COUNT) + COUNT) % COUNT;
        h[idx] = flat;
      }
      pads.push_back({x0, x1});
    }

    // Circular smooth (skip pads)
    std::vector<float> tmp = h;
    for (int i = 0; i < COUNT; ++i) {
      float wx = x_at(i);
      if (on_pad(wx))
        continue;
      int im = (i - 1 + COUNT) % COUNT;
      int ip = (i + 1) % COUNT;
      tmp[i] = 0.25f * h[im] + 0.5f * h[i] + 0.25f * h[ip];
    }
    h.swap(tmp);
  }
};

// ---------------------------------------------------------------------------
// Ship
// ---------------------------------------------------------------------------
enum class FlightState { Flying, Landed, Crashed };

struct Ship {
  Vec2 pos;
  Vec2 vel;
  float angle = 0.f;
  float ang_vel = 0.f;
  float left_thrust = 0.f;
  float right_thrust = 0.f;
  bool swap_engines = false;

  int config_index = 1;
  const ShipConfig* cfg = &SHIP_CONFIGS[1];
  FlightState state = FlightState::Flying;
  float state_timer = 0.f;  // for crash flash / land hold

  void set_config(int index) {
    if (index < 0)
      index = SHIP_CONFIG_COUNT - 1;
    if (index >= SHIP_CONFIG_COUNT)
      index = 0;
    config_index = index;
    cfg = &SHIP_CONFIGS[config_index];
    ang_vel *= 0.5f;
    vel = vel * 0.7f;
    std::printf("Ship: %s\n", cfg->name);
  }

  void cycle_config(int delta) { set_config(config_index + delta); }

  void spawn(const Terrain& terrain, float wx) {
    float ground = terrain.height_at(wx);
    pos = {wx, ground - 220.f};
    vel = {};
    angle = 0.f;
    ang_vel = 0.f;
    left_thrust = right_thrust = 0.f;
    state = FlightState::Flying;
    state_timer = 0.f;
  }

  // Local → world (clockwise angle, y-down)
  Vec2 to_world(Vec2 local) const {
    float c = std::cos(angle);
    float s = std::sin(angle);
    return {pos.x + c * local.x - s * local.y, pos.y + s * local.x + c * local.y};
  }

  void update_physics(float dt) {
    if (state != FlightState::Flying)
      return;

    const float ox = cfg->engine_offset_x;
    const float oy = cfg->engine_offset_y;
    Vec2 left_local{-ox, oy};
    Vec2 right_local{ox, oy};

    auto rotate = [this](Vec2 v) -> Vec2 {
      float c = std::cos(angle);
      float s = std::sin(angle);
      return {c * v.x - s * v.y, s * v.x + c * v.y};
    };

    Vec2 force{};
    float torque = 0.f;
    Vec2 thrust_dir_local{0.f, -1.f};

    if (left_thrust > 0.f) {
      Vec2 f = rotate(thrust_dir_local) * (cfg->max_thrust * left_thrust);
      force += f;
      Vec2 r = rotate(left_local);
      torque += r.x * f.y - r.y * f.x;
    }
    if (right_thrust > 0.f) {
      Vec2 f = rotate(thrust_dir_local) * (cfg->max_thrust * right_thrust);
      force += f;
      Vec2 r = rotate(right_local);
      torque += r.x * f.y - r.y * f.x;
    }

    force.y += GRAVITY * cfg->mass;
    Vec2 acc = force * (1.f / cfg->mass);
    vel += acc * dt;
    vel = vel * std::max(0.f, 1.f - LINEAR_DRAG * dt);
    pos += vel * dt;

    float ang_acc = torque / cfg->inertia;
    ang_vel += ang_acc * dt;
    ang_vel *= std::max(0.f, 1.f - ANGULAR_DRAG * dt);
    ang_vel = clampf(ang_vel, -MAX_ANGULAR_VEL, MAX_ANGULAR_VEL);
    angle += ang_vel * dt;
    while (angle > PI)
      angle -= 2.f * PI;
    while (angle < -PI)
      angle += 2.f * PI;

    // Toroidal X: wrap around the world
    pos.x = Terrain::wrap_x(pos.x);
  }

  // Probe points along the belly / engine line for collision
  void collide(const Terrain& terrain) {
    if (state != FlightState::Flying)
      return;

    const float ox = cfg->engine_offset_x;
    const float oy = cfg->engine_offset_y;
    // Three belly probes: left engine, center aft, right engine
    Vec2 probes[3] = {
        {-ox, oy + 4.f},
        {0.f, oy + 6.f},
        {ox, oy + 4.f},
    };

    float max_pen = 0.f;
    float contact_x = pos.x;
    for (const auto& lp : probes) {
      Vec2 wp = to_world(lp);
      float ground = terrain.height_at(wp.x);
      float pen = wp.y - ground;
      if (pen > max_pen) {
        max_pen = pen;
        contact_x = wp.x;
      }
    }

    if (max_pen <= 0.f)
      return;  // still airborne

    // Contact: resolve position out of ground
    pos.y -= max_pen;

    const bool pad = terrain.on_pad(contact_x);
    const float abs_angle = std::abs(angle);
    const float speed_y = vel.y;  // positive = downward
    const float speed_x = std::abs(vel.x);
    const float abs_w = std::abs(ang_vel);

    const bool gentle = speed_y < LAND_MAX_VY && speed_x < LAND_MAX_VX && abs_angle < LAND_MAX_ANGLE &&
                        abs_w < LAND_MAX_ANGVEL;

    if (pad && gentle) {
      state = FlightState::Landed;
      state_timer = 0.f;
      vel = {};
      ang_vel = 0.f;
      // Snap upright-ish on pad
      angle = 0.f;
      std::printf("LANDED on pad at x=%.0f\n", contact_x);
    } else {
      state = FlightState::Crashed;
      state_timer = 0.f;
      vel = {};
      ang_vel = 0.f;
      std::printf("CRASH  vy=%.1f vx=%.1f angle=%.2f pad=%d\n", speed_y, speed_x, angle, (int)pad);
    }
  }
};

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------
struct Camera {
  float x = 0.f;
  float y = 0.f;
  float prev_ship_x = 0.f;
  bool have_prev = false;

  void follow(const Ship& ship, float dt) {
    // When the ship wraps across the seam, shift the camera by the same amount
    // so the view does not jump.
    if (have_prev) {
      float d = ship.pos.x - prev_ship_x;
      if (d > Terrain::WORLD_W * 0.5f)
        x -= Terrain::WORLD_W;
      else if (d < -Terrain::WORLD_W * 0.5f)
        x += Terrain::WORLD_W;
    }
    prev_ship_x = ship.pos.x;
    have_prev = true;

    float target_x = ship.pos.x - WINDOW_W * 0.5f;
    float target_y = ship.pos.y - WINDOW_H * 0.55f;
    float k = 1.f - std::exp(-6.f * dt);
    // Seam already corrected above; smooth toward the continuous target.
    x += (target_x - x) * k;
    y += (target_y - y) * k;
    y = clampf(y, -200.f, 900.f);
  }

  // Map a world X (already on the ring) into the continuous camera frame
  // so objects near the seam still appear beside the ship.
  float continuous_x(float wx) const {
    float cam_center = x + WINDOW_W * 0.5f;
    float d = Terrain::wrap_delta(cam_center, Terrain::wrap_x(wx));
    return cam_center + d;
  }

  SDL_Point to_screen(float wx, float wy) const {
    float sx = continuous_x(wx) - x;
    return {static_cast<int>(sx + 0.5f), static_cast<int>(wy - y + 0.5f)};
  }
};

// ---------------------------------------------------------------------------
// CRT drawing helpers
// ---------------------------------------------------------------------------
void set_color(SDL_Renderer* ren, SDL_Color c, Uint8 a = 255) {
  SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, a);
}

void draw_line_w(SDL_Renderer* ren, const Camera& cam, float x0, float y0, float x1, float y1,
                 SDL_Color c) {
  SDL_Point a = cam.to_screen(x0, y0);
  SDL_Point b = cam.to_screen(x1, y1);
  set_color(ren, c);
  SDL_RenderDrawLine(ren, a.x, a.y, b.x, b.y);
  // soft glow: second dimmer pass offset
  set_color(ren, c, 60);
  SDL_RenderDrawLine(ren, a.x + 1, a.y, b.x + 1, b.y);
}

void draw_scanlines(SDL_Renderer* ren) {
  set_color(ren, SDL_Color{0, 0, 0, 255}, 255);
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
  for (int y = 0; y < WINDOW_H; y += 3) {
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 50);
    SDL_RenderDrawLine(ren, 0, y, WINDOW_W, y);
  }
}

void draw_terrain(SDL_Renderer* ren, const Terrain& t, const Camera& cam) {
  // Walk a continuous X range covering the view; sample heights on the ring.
  float x_lo = cam.x - 40.f;
  float x_hi = cam.x + WINDOW_W + 40.f;
  int i0 = static_cast<int>(std::floor(x_lo / Terrain::STEP));
  int i1 = static_cast<int>(std::ceil(x_hi / Terrain::STEP));

  set_color(ren, CRT_DIM, 180);
  for (int i = i0; i < i1; ++i) {
    float wx0 = static_cast<float>(i) * Terrain::STEP;
    float wx1 = static_cast<float>(i + 1) * Terrain::STEP;
    float h0 = t.height_at(wx0);
    float h1 = t.height_at(wx1);
    // Screen positions use continuous mapping via camera
    SDL_Point a = cam.to_screen(Terrain::wrap_x(wx0), h0);
    // Prefer continuous x for segment neighbour to avoid seam flip mid-segment
    float sx0 = cam.continuous_x(Terrain::wrap_x(wx0));
    float sx1 = sx0 + Terrain::STEP;  // consecutive sample in camera space
    int ax = static_cast<int>(sx0 - cam.x + 0.5f);
    int bx = static_cast<int>(sx1 - cam.x + 0.5f);
    int ay = static_cast<int>(h0 - cam.y + 0.5f);
    int by = static_cast<int>(h1 - cam.y + 0.5f);
    int y_bot = WINDOW_H + 2;
    int steps = std::max(1, std::abs(bx - ax));
    for (int s = 0; s <= steps; ++s) {
      float u = static_cast<float>(s) / steps;
      int x = static_cast<int>(lerpf(static_cast<float>(ax), static_cast<float>(bx), u));
      int y = static_cast<int>(lerpf(static_cast<float>(ay), static_cast<float>(by), u));
      SDL_RenderDrawLine(ren, x, y, x, y_bot);
    }
    (void)a;
  }

  for (int i = i0; i < i1; ++i) {
    float wx0 = static_cast<float>(i) * Terrain::STEP;
    float wx1 = static_cast<float>(i + 1) * Terrain::STEP;
    float h0 = t.height_at(wx0);
    float h1 = t.height_at(wx1);
    bool pad = t.on_pad(wx0);
    float sx0 = cam.continuous_x(Terrain::wrap_x(wx0));
    float sx1 = sx0 + Terrain::STEP;
    SDL_Color col = pad ? CRT_PAD : CRT_BRIGHT;
    set_color(ren, col);
    int ax = static_cast<int>(sx0 - cam.x + 0.5f);
    int bx = static_cast<int>(sx1 - cam.x + 0.5f);
    int ay = static_cast<int>(h0 - cam.y + 0.5f);
    int by = static_cast<int>(h1 - cam.y + 0.5f);
    SDL_RenderDrawLine(ren, ax, ay, bx, by);
    set_color(ren, col, 60);
    SDL_RenderDrawLine(ren, ax + 1, ay, bx + 1, by);
  }

  for (const auto& p : t.pads) {
    float y = t.height_at(0.5f * (p.x0 + p.x1));
    draw_line_w(ren, cam, p.x0, y + 1.f, p.x0, y + 14.f, CRT_PAD);
    draw_line_w(ren, cam, p.x1, y + 1.f, p.x1, y + 14.f, CRT_PAD);
    float cx = 0.5f * (p.x0 + p.x1);
    draw_line_w(ren, cam, cx - 8.f, y + 6.f, cx + 8.f, y + 6.f, CRT_MID);
  }
}

void draw_ship_vector(SDL_Renderer* ren, const Ship& ship, const Camera& cam) {
  const float hw = ship.cfg->half_w * 0.85f;
  const float hh = ship.cfg->half_h * 0.9f;
  const float ox = ship.cfg->engine_offset_x;
  const float oy = ship.cfg->engine_offset_y;

  // Hull diamond / lander outline in local space
  Vec2 nose{0.f, -hh};
  Vec2 bl{-hw * 0.7f, hh * 0.35f};
  Vec2 br{hw * 0.7f, hh * 0.35f};
  Vec2 tl{-hw * 0.35f, -hh * 0.2f};
  Vec2 tr{hw * 0.35f, -hh * 0.2f};

  auto W = [&](Vec2 l) {
    Vec2 w = ship.to_world(l);
    return cam.to_screen(w.x, w.y);
  };

  SDL_Color body = (ship.state == FlightState::Crashed) ? CRT_HOT
                    : (ship.state == FlightState::Landed)  ? CRT_PAD
                                                         : CRT_BRIGHT;

  auto line = [&](Vec2 a, Vec2 b) {
    SDL_Point pa = W(a), pb = W(b);
    set_color(ren, body);
    SDL_RenderDrawLine(ren, pa.x, pa.y, pb.x, pb.y);
    set_color(ren, body, 70);
    SDL_RenderDrawLine(ren, pa.x + 1, pa.y, pb.x + 1, pb.y);
  };

  line(nose, bl);
  line(nose, br);
  line(bl, br);
  line(tl, tr);
  // cockpit
  line(Vec2{-6.f, -hh * 0.45f}, Vec2{6.f, -hh * 0.45f});
  line(Vec2{-6.f, -hh * 0.45f}, Vec2{-4.f, -hh * 0.15f});
  line(Vec2{6.f, -hh * 0.45f}, Vec2{4.f, -hh * 0.15f});

  // Engine pods
  line(Vec2{-ox - 6.f, oy - 4.f}, Vec2{-ox + 6.f, oy - 4.f});
  line(Vec2{-ox - 6.f, oy - 4.f}, Vec2{-ox - 4.f, oy + 8.f});
  line(Vec2{-ox + 6.f, oy - 4.f}, Vec2{-ox + 4.f, oy + 8.f});
  line(Vec2{-ox - 4.f, oy + 8.f}, Vec2{-ox + 4.f, oy + 8.f});

  line(Vec2{ox - 6.f, oy - 4.f}, Vec2{ox + 6.f, oy - 4.f});
  line(Vec2{ox - 6.f, oy - 4.f}, Vec2{ox - 4.f, oy + 8.f});
  line(Vec2{ox + 6.f, oy - 4.f}, Vec2{ox + 4.f, oy + 8.f});
  line(Vec2{ox - 4.f, oy + 8.f}, Vec2{ox + 4.f, oy + 8.f});

  // Heading tick
  if (ship.state == FlightState::Flying) {
    Vec2 tip = ship.to_world({0.f, -hh - 12.f});
    SDL_Point p0 = cam.to_screen(ship.pos.x, ship.pos.y);
    SDL_Point p1 = cam.to_screen(tip.x, tip.y);
    set_color(ren, CRT_WARN);
    SDL_RenderDrawLine(ren, p0.x, p0.y, p1.x, p1.y);
  }
}

void draw_exhaust(SDL_Renderer* ren, const Ship& ship, const Camera& cam, bool left) {
  if (ship.state != FlightState::Flying)
    return;
  float t = left ? ship.left_thrust : ship.right_thrust;
  if (t < 0.05f)
    return;

  float local_x = left ? -ship.cfg->engine_offset_x : ship.cfg->engine_offset_x;
  float local_y = ship.cfg->engine_offset_y + 10.f;
  Vec2 base = ship.to_world({local_x, local_y});
  float c = std::cos(ship.angle);
  float s = std::sin(ship.angle);
  float ex = -s;  // aft
  float ey = c;
  int len = static_cast<int>(10 + t * 36);
  for (int i = 0; i < len; i += 2) {
    float wx = base.x + ex * i;
    float wy = base.y + ey * i;
    SDL_Point p = cam.to_screen(wx, wy);
    set_color(ren, (i < len / 2) ? CRT_BRIGHT : CRT_HOT);
    SDL_RenderDrawPoint(ren, p.x, p.y);
    SDL_RenderDrawPoint(ren, p.x + 1, p.y);
  }
}

// Minimal 3x5 HUD font
void draw_char(SDL_Renderer* ren, int x, int y, char ch, SDL_Color col) {
  const char* map = nullptr;
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
    case ' ': map = "000000000000000"; break;
    default: map = "111101101101111"; break;
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

void draw_hud(SDL_Renderer* ren, const Ship& ship, const Terrain& terrain) {
  // Thrust bars
  auto bar = [&](int x, int y, float v, SDL_Color c) {
    set_color(ren, CRT_DIM);
    SDL_Rect bg{x, y, 100, 8};
    SDL_RenderFillRect(ren, &bg);
    set_color(ren, c);
    SDL_Rect fg{x, y, static_cast<int>(100 * clampf(v, 0.f, 1.f)), 8};
    SDL_RenderFillRect(ren, &fg);
  };
  bar(20, 20, ship.left_thrust, CRT_MID);
  bar(20, 32, ship.right_thrust, CRT_MID);

  draw_text(ren, 20, 46, ship.cfg->name, CRT_BRIGHT);
  draw_text(ren, 20, 54, "SELECT PRESET", CRT_DIM);
  draw_text(ren, 20, 62, ship.swap_engines ? "ENGINES SWAPPED" : "START SWAP L R",
            ship.swap_engines ? CRT_WARN : CRT_DIM);

  // Telemetry
  char buf[64];
  std::snprintf(buf, sizeof(buf), "ALT %.0f",
                terrain.height_at(ship.pos.x) - ship.pos.y - ship.cfg->engine_offset_y);
  draw_text(ren, 20, 80, buf, CRT_BRIGHT);
  std::snprintf(buf, sizeof(buf), "VX %.0f  VY %.0f", ship.vel.x, ship.vel.y);
  draw_text(ren, 20, 88, buf, CRT_MID);
  std::snprintf(buf, sizeof(buf), "ANG %.0f", ship.angle * 180.f / PI);
  draw_text(ren, 20, 96, buf, CRT_MID);

  // Landing safety lights
  bool pad = terrain.on_pad(ship.pos.x);
  bool ok_v = ship.vel.y < LAND_MAX_VY && std::abs(ship.vel.x) < LAND_MAX_VX;
  bool ok_a = std::abs(ship.angle) < LAND_MAX_ANGLE;
  draw_text(ren, WINDOW_W - 120, 20, pad ? "PAD OK" : "NO PAD", pad ? CRT_PAD : CRT_HOT);
  draw_text(ren, WINDOW_W - 120, 28, ok_v ? "SPEED OK" : "SPEED HI", ok_v ? CRT_PAD : CRT_HOT);
  draw_text(ren, WINDOW_W - 120, 36, ok_a ? "ATT OK" : "ATT BAD", ok_a ? CRT_PAD : CRT_HOT);

  if (ship.state == FlightState::Landed) {
    draw_text(ren, WINDOW_W / 2 - 40, 40, "LANDED", CRT_PAD);
    draw_text(ren, WINDOW_W / 2 - 70, 50, "R OR A TO RELIGHT", CRT_MID);
  } else if (ship.state == FlightState::Crashed) {
    draw_text(ren, WINDOW_W / 2 - 40, 40, "CRASH", CRT_HOT);
    draw_text(ren, WINDOW_W / 2 - 70, 50, "R OR A TO RESET", CRT_MID);
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
      "dualthrust — CRT lander  triggers=engines  Select=ship  Start=swap",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_W, WINDOW_H,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (!window) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_Renderer* ren =
      SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren) {
    std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

  Terrain terrain;
  terrain.generate(0xC0FFEE);

  Ship ship;
  ship.set_config(1);
  // Start above the middle pad
  ship.spawn(terrain, Terrain::WORLD_W * 0.45f);

  Camera cam;
  auto snap_camera = [&]() {
    cam.x = ship.pos.x - WINDOW_W * 0.5f;
    cam.y = ship.pos.y - WINDOW_H * 0.55f;
    cam.prev_ship_x = ship.pos.x;
    cam.have_prev = true;
  };
  snap_camera();

  SDL_GameController* pad = nullptr;
  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    if (SDL_IsGameController(i)) {
      pad = SDL_GameControllerOpen(i);
      if (pad) {
        std::printf("Opened controller: %s\n", SDL_GameControllerName(pad));
        break;
      }
    }
  }
  if (!pad) {
    std::printf("No game controller found.\n");
    std::printf("  A/D or arrows = engines\n");
    std::printf("  Tab / [ / ] = ship preset\n");
    std::printf("  X = swap engines   R = reset/relight\n");
  }

  bool running = true;
  Uint64 prev = SDL_GetPerformanceCounter();
  const Uint64 freq = SDL_GetPerformanceFrequency();
  bool key_left = false, key_right = false;

  auto reset_or_relight = [&]() {
    if (ship.state == FlightState::Landed) {
      ship.state = FlightState::Flying;
      ship.vel.y = -30.f;  // gentle hop
      std::printf("Relight\n");
    } else {
      int pi = static_cast<int>(SDL_GetTicks() % terrain.pads.size());
      float cx = 0.5f * (terrain.pads[pi].x0 + terrain.pads[pi].x1);
      ship.spawn(terrain, cx);
      snap_camera();
      std::printf("Reset above pad %d\n", pi);
    }
  };

  while (running) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT)
        running = false;
      if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
        running = false;

      if (ev.type == SDL_KEYDOWN) {
        if (ev.key.keysym.sym == SDLK_a || ev.key.keysym.sym == SDLK_LEFT)
          key_left = true;
        if (ev.key.keysym.sym == SDLK_d || ev.key.keysym.sym == SDLK_RIGHT)
          key_right = true;
        if (ev.key.keysym.sym == SDLK_TAB || ev.key.keysym.sym == SDLK_RIGHTBRACKET)
          ship.cycle_config(+1);
        if (ev.key.keysym.sym == SDLK_LEFTBRACKET)
          ship.cycle_config(-1);
        if (ev.key.keysym.sym == SDLK_x) {
          ship.swap_engines = !ship.swap_engines;
          std::printf("Engine mapping: %s\n", ship.swap_engines ? "SWAPPED" : "normal");
        }
        if (ev.key.keysym.sym == SDLK_r)
          reset_or_relight();
        // Regenerate terrain
        if (ev.key.keysym.sym == SDLK_g) {
          terrain.generate(SDL_GetTicks());
          ship.spawn(terrain, Terrain::WORLD_W * 0.45f);
          snap_camera();
          std::printf("New terrain seed\n");
        }
      }
      if (ev.type == SDL_KEYUP) {
        if (ev.key.keysym.sym == SDLK_a || ev.key.keysym.sym == SDLK_LEFT)
          key_left = false;
        if (ev.key.keysym.sym == SDLK_d || ev.key.keysym.sym == SDLK_RIGHT)
          key_right = false;
      }

      if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
        if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
          ship.cycle_config(+1);
        if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
          ship.swap_engines = !ship.swap_engines;
          std::printf("Engine mapping: %s\n", ship.swap_engines ? "SWAPPED" : "normal");
        }
        if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_A ||
            ev.cbutton.button == SDL_CONTROLLER_BUTTON_B)
          reset_or_relight();
        if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_Y) {
          terrain.generate(SDL_GetTicks());
          ship.spawn(terrain, Terrain::WORLD_W * 0.45f);
          snap_camera();
        }
      }

      if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad) {
        pad = SDL_GameControllerOpen(ev.cdevice.which);
        if (pad)
          std::printf("Controller connected: %s\n", SDL_GameControllerName(pad));
      }
      if (ev.type == SDL_CONTROLLERDEVICEREMOVED && pad) {
        if (ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
          SDL_GameControllerClose(pad);
          pad = nullptr;
        }
      }
    }

    float lt = 0.f, rt = 0.f;
    if (pad) {
      Sint16 raw_l = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
      Sint16 raw_r = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
      lt = clampf(raw_l / 32767.f, 0.f, 1.f);
      rt = clampf(raw_r / 32767.f, 0.f, 1.f);
    }
    if (key_left)
      lt = std::max(lt, 1.f);
    if (key_right)
      rt = std::max(rt, 1.f);

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
    dt = std::min(dt, 0.05f) * TIME_SCALE;

    ship.update_physics(dt);
    ship.collide(terrain);
    if (ship.state != FlightState::Flying)
      ship.state_timer += dt;

    cam.follow(ship, dt / TIME_SCALE);  // camera in wall-clock feel

    // --- Render CRT frame ---
    set_color(ren, CRT_BG);
    SDL_RenderClear(ren);

    draw_terrain(ren, terrain, cam);
    draw_exhaust(ren, ship, cam, true);
    draw_exhaust(ren, ship, cam, false);
    draw_ship_vector(ren, ship, cam);

    // Crash flash
    if (ship.state == FlightState::Crashed) {
      float flash = 0.5f + 0.5f * std::sin(ship.state_timer * 20.f);
      set_color(ren, CRT_HOT, static_cast<Uint8>(40 + 80 * flash));
      SDL_Rect full{0, 0, WINDOW_W, WINDOW_H};
      SDL_RenderFillRect(ren, &full);
    }

    draw_hud(ren, ship, terrain);
    draw_scanlines(ren);

    // Vignette-ish side darkening via vertical edges
    set_color(ren, SDL_Color{0, 0, 0, 255});
    for (int i = 0; i < 24; ++i) {
      SDL_SetRenderDrawColor(ren, 0, 0, 0, static_cast<Uint8>(90 - i * 3));
      SDL_RenderDrawLine(ren, i, 0, i, WINDOW_H);
      SDL_RenderDrawLine(ren, WINDOW_W - 1 - i, 0, WINDOW_W - 1 - i, WINDOW_H);
    }

    SDL_RenderPresent(ren);
  }

  if (pad)
    SDL_GameControllerClose(pad);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
