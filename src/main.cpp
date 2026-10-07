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

constexpr float GRAVITY = 120.0f;  // px/s² downward (sim seconds)
constexpr float LINEAR_DRAG = 0.15f;
constexpr float ANGULAR_DRAG = 1.5f;
constexpr float MAX_ANGULAR_VEL = 8.0f;

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

inline float length(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// ---------------------------------------------------------------------------
// Ship presets: size, engine hardpoints, mass / inertia / thrust
// ---------------------------------------------------------------------------
struct ShipConfig {
  const char* name;
  float half_w;          // half sprite width in px (full draw size = 2*half)
  float half_h;          // half sprite height in px
  float engine_offset_x; // local |x| of each engine
  float engine_offset_y; // local +y (aft) of engines
  float mass;
  float inertia;
  float max_thrust;      // force per engine at full trigger
};

// Ordered list; Select/Back cycles through these.
constexpr ShipConfig SHIP_CONFIGS[] = {
    // narrow, engines close → sluggish yaw, nimble translation
    {"Narrow", 22.f, 30.f, 12.f, 20.f, 0.85f, 450.f, 380.f},
    // default-ish medium
    {"Medium", 32.f, 32.f, 20.f, 22.f, 1.0f, 900.f, 400.f},
    // wide hull, engines far apart → strong differential torque
    {"Wide", 48.f, 28.f, 36.f, 18.f, 1.25f, 1600.f, 420.f},
    // barge: very wide, heavy, engines near the tips
    {"Barge", 64.f, 26.f, 52.f, 16.f, 1.6f, 2800.f, 440.f},
    // tall / long, engines mid-aft
    {"Long", 26.f, 42.f, 14.f, 30.f, 1.1f, 1100.f, 390.f},
};
constexpr int SHIP_CONFIG_COUNT = static_cast<int>(sizeof(SHIP_CONFIGS) / sizeof(SHIP_CONFIGS[0]));

// Orientation (y increases downward, like SDL):
//   angle = 0      → nose points up (toward -world y)
//   angle > 0      → clockwise on screen (matches SDL_RenderCopyEx)
// Local frame at angle 0: +x = right, +y = aft (down).
// local→world:  x =  c*lx - s*ly
//               y =  s*lx + c*ly
// nose direction: (sin θ, -cos θ)
struct Ship {
  Vec2 pos{WINDOW_W * 0.5f, WINDOW_H * 0.4f};
  Vec2 vel{};
  float angle = 0.f;  // radians, clockwise from nose-up
  float ang_vel = 0.f;

  float left_thrust = 0.f;  // 0..1
  float right_thrust = 0.f;

  int config_index = 1;  // start on Medium
  const ShipConfig* cfg = &SHIP_CONFIGS[1];

  void set_config(int index) {
    if (index < 0)
      index = SHIP_CONFIG_COUNT - 1;
    if (index >= SHIP_CONFIG_COUNT)
      index = 0;
    config_index = index;
    cfg = &SHIP_CONFIGS[config_index];
    // Soft reset of rates so a sudden inertia change does not explode
    ang_vel *= 0.5f;
    vel = vel * 0.7f;
    std::printf("Ship: %s  (engines ±%.0f,%.0f  mass %.2f  inertia %.0f)\n",
                cfg->name, cfg->engine_offset_x, cfg->engine_offset_y, cfg->mass,
                cfg->inertia);
  }

  void cycle_config(int delta) { set_config(config_index + delta); }

  void update(float dt) {
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

    // Reaction force on the ship is nose-ward: local (0, -1)
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
    ang_vel = std::clamp(ang_vel, -MAX_ANGULAR_VEL, MAX_ANGULAR_VEL);
    angle += ang_vel * dt;

    while (angle > PI)
      angle -= 2.f * PI;
    while (angle < -PI)
      angle += 2.f * PI;

    const float margin = 40.f + std::max(cfg->half_w, cfg->half_h);
    if (pos.x < margin) {
      pos.x = margin;
      vel.x = std::abs(vel.x) * 0.4f;
    }
    if (pos.x > WINDOW_W - margin) {
      pos.x = WINDOW_W - margin;
      vel.x = -std::abs(vel.x) * 0.4f;
    }
    if (pos.y < margin) {
      pos.y = margin;
      vel.y = std::abs(vel.y) * 0.4f;
    }
    if (pos.y > WINDOW_H - margin) {
      pos.y = WINDOW_H - margin;
      vel.y = -std::abs(vel.y) * 0.4f;
    }
  }
};

SDL_Texture* create_ship_texture(SDL_Renderer* ren) {
  // Procedural ship in a unit square texture. Nose = TOP, engines = BOTTOM.
  // Drawn at cfg half_w/half_h so one texture serves every preset.
  constexpr int W = 64;
  constexpr int H = 64;
  SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_RGBA32);
  if (!surf)
    return nullptr;

  auto put = [&](int x, int y, Uint8 r, Uint8 g, Uint8 b, Uint8 a = 255) {
    if (x < 0 || y < 0 || x >= W || y >= H)
      return;
    Uint32* p = static_cast<Uint32*>(surf->pixels) + y * W + x;
    *p = (a << 24) | (b << 16) | (g << 8) | r;
  };

  SDL_FillRect(surf, nullptr, 0);

  // Hull — full width of texture so wide presets look broad when scaled
  for (int y = 4; y < 46; ++y) {
    float t = (y - 4) / 42.f;
    int half = static_cast<int>(1 + t * 28);  // tip → nearly full width
    half = std::min(half, 30);
    for (int x = 32 - half; x <= 32 + half; ++x) {
      Uint8 shade = static_cast<Uint8>(220 - t * 40);
      put(x, y, shade, shade, static_cast<Uint8>(shade + 15));
    }
  }

  // Gold nose tip
  for (int y = 2; y < 10; ++y) {
    int half = (y < 6) ? 1 : 2;
    for (int x = 32 - half; x <= 32 + half; ++x)
      put(x, y, 255, 220, 60);
  }

  // Cockpit
  for (int y = 16; y < 28; ++y)
    for (int x = 27; x < 37; ++x)
      put(x, y, 60, 140, 255);

  // Center keel
  for (int y = 10; y < 46; ++y)
    put(32, y, 40, 40, 55);

  // Engine pods at the outer aft corners of the texture
  for (int y = 44; y < 56; ++y) {
    for (int x = 4; x < 18; ++x)
      put(x, y, 180, 90, 50);
    for (int x = 46; x < 60; ++x)
      put(x, y, 180, 90, 50);
  }
  for (int y = 54; y < 62; ++y) {
    for (int x = 6; x < 16; ++x)
      put(x, y, 25, 25, 30);
    for (int x = 48; x < 58; ++x)
      put(x, y, 25, 25, 30);
  }

  SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
  SDL_FreeSurface(surf);
  SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
  return tex;
}

void draw_exhaust(SDL_Renderer* ren, const Ship& ship, bool left) {
  if ((left ? ship.left_thrust : ship.right_thrust) < 0.05f)
    return;

  float t = left ? ship.left_thrust : ship.right_thrust;
  float local_x = left ? -ship.cfg->engine_offset_x : ship.cfg->engine_offset_x;
  float local_y = ship.cfg->engine_offset_y + 10.f;

  float c = std::cos(ship.angle);
  float s = std::sin(ship.angle);
  float wx = ship.pos.x + c * local_x - s * local_y;
  float wy = ship.pos.y + s * local_x + c * local_y;

  // Aft = rotate(local +y) = (-s, c)
  float ex = -s;
  float ey = c;

  int len = static_cast<int>(14 + t * 32);
  SDL_SetRenderDrawColor(ren, 255, 190, 50, 230);
  for (int i = 0; i < len; i += 2) {
    int px = static_cast<int>(wx + ex * i);
    int py = static_cast<int>(wy + ey * i);
    SDL_RenderDrawPoint(ren, px, py);
    SDL_RenderDrawPoint(ren, px + 1, py);
  }
  SDL_SetRenderDrawColor(ren, 255, 70, 20, 140);
  for (int i = 0; i < len / 2; i += 3) {
    int px = static_cast<int>(wx + ex * i + (left ? -1.f : 1.f));
    int py = static_cast<int>(wy + ey * i);
    SDL_RenderDrawPoint(ren, px, py);
  }
}

void draw_heading_marker(SDL_Renderer* ren, const Ship& ship) {
  float c = std::cos(ship.angle);
  float s = std::sin(ship.angle);
  float nx = s;
  float ny = -c;
  float len = ship.cfg->half_h + 10.f;
  int x0 = static_cast<int>(ship.pos.x);
  int y0 = static_cast<int>(ship.pos.y);
  int x1 = static_cast<int>(ship.pos.x + nx * len);
  int y1 = static_cast<int>(ship.pos.y + ny * len);
  SDL_SetRenderDrawColor(ren, 255, 230, 80, 220);
  SDL_RenderDrawLine(ren, x0, y0, x1, y1);
  float rx = c;
  float ry = s;
  float tip = len - 6.f;
  SDL_RenderDrawLine(ren,
                     static_cast<int>(ship.pos.x + nx * tip - rx * 5.f),
                     static_cast<int>(ship.pos.y + ny * tip - ry * 5.f),
                     static_cast<int>(ship.pos.x + nx * tip + rx * 5.f),
                     static_cast<int>(ship.pos.y + ny * tip + ry * 5.f));
}

// Tiny 3x5 digit font for the config name index / HUD labels (no TTF dependency)
void draw_char(SDL_Renderer* ren, int x, int y, char ch, Uint8 r, Uint8 g, Uint8 b) {
  // 3x5 bitmaps for A-Z, 0-9, space
  static const char* glyphs[] = {
      // 0-9
      "111101101101111",  // 0
      "010010010010010",  // 1
      "111001111100111",  // 2
      "111001111001111",  // 3
      "101101111001001",  // 4
      "111100111001111",  // 5
      "111100111101111",  // 6
      "111001001001001",  // 7
      "111101111101111",  // 8
      "111101111001111",  // 9
  };
  auto plot = [&](const char* bits) {
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    for (int row = 0; row < 5; ++row)
      for (int col = 0; col < 3; ++col)
        if (bits[row * 3 + col] == '1')
          SDL_RenderDrawPoint(ren, x + col, y + row);
  };
  if (ch >= '0' && ch <= '9') {
    plot(glyphs[ch - '0']);
    return;
  }
  // crude letters used in preset names
  const char* map = nullptr;
  switch (ch) {
    case 'A': map = "010101111101101"; break;
    case 'B': map = "110101110101110"; break;
    case 'E': map = "111100110100111"; break;
    case 'G': map = "011100101101011"; break;
    case 'I': map = "111010010010111"; break;
    case 'L': map = "100100100100111"; break;
    case 'M': map = "101111111101101"; break;
    case 'N': map = "101111111111101"; break;
    case 'O': map = "010101101101010"; break;
    case 'R': map = "110101110101101"; break;
    case 'W': map = "101101111111101"; break;
    case 'D': map = "110101101101110"; break;
    case ' ': map = "000000000000000"; break;
    default: map = "111101101101111"; break;
  }
  plot(map);
}

void draw_text(SDL_Renderer* ren, int x, int y, const char* s, Uint8 r, Uint8 g, Uint8 b) {
  for (int i = 0; s[i]; ++i)
    draw_char(ren, x + i * 4, y, s[i], r, g, b);
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
      "dualthrust — triggers = engines, Select/Back = ship preset",
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

  SDL_Texture* ship_tex = create_ship_texture(ren);
  if (!ship_tex) {
    std::fprintf(stderr, "Failed to create ship texture\n");
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

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
    std::printf("  A/D or Left/Right = engines\n");
    std::printf("  Tab / [ / ] = cycle ship preset\n");
  }

  Ship ship;
  ship.set_config(1);  // Medium

  bool running = true;
  Uint64 prev = SDL_GetPerformanceCounter();
  const Uint64 freq = SDL_GetPerformanceFrequency();

  bool key_left = false, key_right = false;

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
      }
      if (ev.type == SDL_KEYUP) {
        if (ev.key.keysym.sym == SDLK_a || ev.key.keysym.sym == SDLK_LEFT)
          key_left = false;
        if (ev.key.keysym.sym == SDLK_d || ev.key.keysym.sym == SDLK_RIGHT)
          key_right = false;
      }

      // Select / Back on the pad cycles presets (also View on Xbox, Share on DS)
      if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
        if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
          ship.cycle_config(+1);
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
          std::printf("Controller disconnected\n");
        }
      }
    }

    float lt = 0.f, rt = 0.f;
    if (pad) {
      Sint16 raw_l = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
      Sint16 raw_r = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
      lt = std::clamp(raw_l / 32767.f, 0.f, 1.f);
      rt = std::clamp(raw_r / 32767.f, 0.f, 1.f);
    }
    if (key_left)
      lt = std::max(lt, 1.f);
    if (key_right)
      rt = std::max(rt, 1.f);

    ship.left_thrust = lt;
    ship.right_thrust = rt;

    Uint64 now = SDL_GetPerformanceCounter();
    float dt = static_cast<float>(now - prev) / static_cast<float>(freq);
    prev = now;
    dt = std::min(dt, 0.05f) * TIME_SCALE;

    ship.update(dt);

    SDL_SetRenderDrawColor(ren, 8, 10, 24, 255);
    SDL_RenderClear(ren);

    SDL_SetRenderDrawColor(ren, 180, 190, 220, 255);
    for (int i = 0; i < 120; ++i) {
      int sx = (i * 97 + 13) % WINDOW_W;
      int sy = (i * 53 + 29) % WINDOW_H;
      SDL_RenderDrawPoint(ren, sx, sy);
    }

    draw_exhaust(ren, ship, true);
    draw_exhaust(ren, ship, false);

    {
      int hw = static_cast<int>(ship.cfg->half_w);
      int hh = static_cast<int>(ship.cfg->half_h);
      SDL_Rect dst{static_cast<int>(ship.pos.x) - hw, static_cast<int>(ship.pos.y) - hh, hw * 2,
                   hh * 2};
      double deg = ship.angle * 180.0 / PI;
      SDL_RenderCopyEx(ren, ship_tex, nullptr, &dst, deg, nullptr, SDL_FLIP_NONE);
    }

    draw_heading_marker(ren, ship);

    // Thrust bars
    auto bar = [&](int x, int y, float v, Uint8 r, Uint8 g, Uint8 b) {
      SDL_Rect bg{x, y, 100, 12};
      SDL_SetRenderDrawColor(ren, 40, 40, 50, 255);
      SDL_RenderFillRect(ren, &bg);
      SDL_Rect fg{x, y, static_cast<int>(100 * v), 12};
      SDL_SetRenderDrawColor(ren, r, g, b, 255);
      SDL_RenderFillRect(ren, &fg);
    };
    bar(20, 20, ship.left_thrust, 80, 200, 120);
    bar(20, 40, ship.right_thrust, 200, 120, 80);

    // Preset name (pixel font)
    draw_text(ren, 20, 60, ship.cfg->name, 200, 200, 220);
    draw_text(ren, 20, 68, "SELECT BACK TO CYCLE", 120, 120, 140);

    SDL_RenderPresent(ren);
  }

  if (pad)
    SDL_GameControllerClose(pad);
  SDL_DestroyTexture(ship_tex);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
