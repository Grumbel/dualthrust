// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
// Co-authored-by: Grok <grok@x.ai>

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr int WINDOW_W = 1280;
constexpr int WINDOW_H = 720;
constexpr float PI = 3.14159265358979323846f;

// Physics (world units: pixels, y increases downward like SDL)
constexpr float GRAVITY = 120.0f;          // px/s² downward
constexpr float SHIP_MASS = 1.0f;
constexpr float SHIP_INERTIA = 800.0f;     // moment of inertia
constexpr float MAX_THRUST = 400.0f;       // force per engine at full trigger
constexpr float ENGINE_OFFSET_X = 18.0f;   // left/right of center
constexpr float ENGINE_OFFSET_Y = 22.0f;   // aft of center (local +y = aft)
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
  Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
};

inline float length(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
inline Vec2 normalize(Vec2 v) {
  float l = length(v);
  return l > 1e-6f ? Vec2{v.x / l, v.y / l} : Vec2{};
}

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
  float angle = 0.f;       // radians, clockwise from nose-up
  float ang_vel = 0.f;

  float left_thrust = 0.f;  // 0..1
  float right_thrust = 0.f;

  void update(float dt) {
    // Engine hardpoints in local space (aft, left/right of center)
    Vec2 left_local{-ENGINE_OFFSET_X, ENGINE_OFFSET_Y};
    Vec2 right_local{ENGINE_OFFSET_X, ENGINE_OFFSET_Y};

    // local → world (clockwise angle, y-down)
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
      Vec2 f = rotate(thrust_dir_local) * (MAX_THRUST * left_thrust);
      force += f;
      // 2D cross r×F; with y-down this sign makes left engine produce clockwise torque
      Vec2 r = rotate(left_local);
      torque += r.x * f.y - r.y * f.x;
    }
    if (right_thrust > 0.f) {
      Vec2 f = rotate(thrust_dir_local) * (MAX_THRUST * right_thrust);
      force += f;
      Vec2 r = rotate(right_local);
      torque += r.x * f.y - r.y * f.x;
    }

    // Gravity
    force.y += GRAVITY * SHIP_MASS;

    // Integrate linear
    Vec2 acc = force * (1.f / SHIP_MASS);
    vel += acc * dt;
    // Simple linear drag
    vel = vel * std::max(0.f, 1.f - LINEAR_DRAG * dt);
    pos += vel * dt;

    // Integrate angular
    float ang_acc = torque / SHIP_INERTIA;
    ang_vel += ang_acc * dt;
    ang_vel *= std::max(0.f, 1.f - ANGULAR_DRAG * dt);
    ang_vel = std::clamp(ang_vel, -MAX_ANGULAR_VEL, MAX_ANGULAR_VEL);
    angle += ang_vel * dt;

    // Keep angle in [-pi, pi] for niceness
    while (angle > PI) angle -= 2.f * PI;
    while (angle < -PI) angle += 2.f * PI;

    // Soft bounds (bounce lightly)
    const float margin = 40.f;
    if (pos.x < margin) { pos.x = margin; vel.x = std::abs(vel.x) * 0.4f; }
    if (pos.x > WINDOW_W - margin) { pos.x = WINDOW_W - margin; vel.x = -std::abs(vel.x) * 0.4f; }
    if (pos.y < margin) { pos.y = margin; vel.y = std::abs(vel.y) * 0.4f; }
    if (pos.y > WINDOW_H - margin) { pos.y = WINDOW_H - margin; vel.y = -std::abs(vel.y) * 0.4f; }
  }
};

SDL_Texture* create_ship_texture(SDL_Renderer* ren) {
  // Procedural ship. Texture space: nose toward TOP (small y), engines at BOTTOM.
  // Center at (32,32). This matches local -y = nose when angle=0.
  constexpr int W = 64;
  constexpr int H = 64;
  SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_RGBA32);
  if (!surf) return nullptr;

  auto put = [&](int x, int y, Uint8 r, Uint8 g, Uint8 b, Uint8 a = 255) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    Uint32* p = static_cast<Uint32*>(surf->pixels) + y * W + x;
    *p = (a << 24) | (b << 16) | (g << 8) | r;
  };

  SDL_FillRect(surf, nullptr, 0);

  // Sharp triangular hull — nose is an unmistakable point at the top
  for (int y = 4; y < 46; ++y) {
    float t = (y - 4) / 42.f;                 // 0 at nose → 1 at base
    int half = static_cast<int>(1 + t * 16);  // 1px tip → wide base
    for (int x = 32 - half; x <= 32 + half; ++x) {
      // Slight gradient: brighter toward nose
      Uint8 shade = static_cast<Uint8>(220 - t * 40);
      put(x, y, shade, shade, static_cast<Uint8>(shade + 15));
    }
  }

  // Bright nose tip (heading cue)
  for (int y = 2; y < 10; ++y) {
    int half = (y < 6) ? 1 : 2;
    for (int x = 32 - half; x <= 32 + half; ++x)
      put(x, y, 255, 220, 60);
  }

  // Cockpit window (upper body)
  for (int y = 16; y < 28; ++y)
    for (int x = 27; x < 37; ++x)
      put(x, y, 60, 140, 255);

  // Dark "keel" line down the center for orientation
  for (int y = 10; y < 46; ++y)
    put(32, y, 40, 40, 55);

  // Engine pods (aft corners) — warm metal, clearly the rear
  for (int y = 44; y < 56; ++y) {
    for (int x = 12; x < 24; ++x) put(x, y, 180, 90, 50);   // left
    for (int x = 40; x < 52; ++x) put(x, y, 180, 90, 50);   // right
  }
  // Nozzle openings
  for (int y = 54; y < 62; ++y) {
    for (int x = 14; x < 22; ++x) put(x, y, 25, 25, 30);
    for (int x = 42; x < 50; ++x) put(x, y, 25, 25, 30);
  }

  SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
  SDL_FreeSurface(surf);
  SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
  return tex;
}

void draw_exhaust(SDL_Renderer* ren, const Ship& ship, bool left) {
  if ((left ? ship.left_thrust : ship.right_thrust) < 0.05f) return;

  float t = left ? ship.left_thrust : ship.right_thrust;
  float local_x = left ? -ENGINE_OFFSET_X : ENGINE_OFFSET_X;
  float local_y = ENGINE_OFFSET_Y + 10.f; // just behind nozzle

  float c = std::cos(ship.angle);
  float s = std::sin(ship.angle);
  // same local→world as physics
  float wx = ship.pos.x + c * local_x - s * local_y;
  float wy = ship.pos.y + s * local_x + c * local_y;

  // Aft direction = rotate(local +y) = (-s, c)
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

// Bright heading tick from center toward the nose — removes any ambiguity
void draw_heading_marker(SDL_Renderer* ren, const Ship& ship) {
  float c = std::cos(ship.angle);
  float s = std::sin(ship.angle);
  // nose dir = (s, -c)
  float nx = s;
  float ny = -c;
  int x0 = static_cast<int>(ship.pos.x);
  int y0 = static_cast<int>(ship.pos.y);
  int x1 = static_cast<int>(ship.pos.x + nx * 36.f);
  int y1 = static_cast<int>(ship.pos.y + ny * 36.f);
  SDL_SetRenderDrawColor(ren, 255, 230, 80, 220);
  SDL_RenderDrawLine(ren, x0, y0, x1, y1);
  // small tip crossbar
  float rx = c;   // right dir
  float ry = s;
  SDL_RenderDrawLine(ren,
                     static_cast<int>(ship.pos.x + nx * 30.f - rx * 5.f),
                     static_cast<int>(ship.pos.y + ny * 30.f - ry * 5.f),
                     static_cast<int>(ship.pos.x + nx * 30.f + rx * 5.f),
                     static_cast<int>(ship.pos.y + ny * 30.f + ry * 5.f));
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
      "dualthrust — left/right triggers = engines",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      WINDOW_W, WINDOW_H,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (!window) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_Renderer* ren = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
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

  // Open first available game controller
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
    std::printf("No game controller found. Plug one in and restart, or use keyboard for testing:\n");
    std::printf("  A/D or Left/Right arrows = left/right engine (hold)\n");
  }

  Ship ship;
  bool running = true;
  Uint64 prev = SDL_GetPerformanceCounter();
  const Uint64 freq = SDL_GetPerformanceFrequency();

  // Keyboard fallback for development (not the primary control)
  bool key_left = false, key_right = false;

  while (running) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT) running = false;
      if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) running = false;
      if (ev.type == SDL_KEYDOWN) {
        if (ev.key.keysym.sym == SDLK_a || ev.key.keysym.sym == SDLK_LEFT) key_left = true;
        if (ev.key.keysym.sym == SDLK_d || ev.key.keysym.sym == SDLK_RIGHT) key_right = true;
      }
      if (ev.type == SDL_KEYUP) {
        if (ev.key.keysym.sym == SDLK_a || ev.key.keysym.sym == SDLK_LEFT) key_left = false;
        if (ev.key.keysym.sym == SDLK_d || ev.key.keysym.sym == SDLK_RIGHT) key_right = false;
      }
      if (ev.type == SDL_CONTROLLERDEVICEADDED && !pad) {
        pad = SDL_GameControllerOpen(ev.cdevice.which);
        if (pad) std::printf("Controller connected: %s\n", SDL_GameControllerName(pad));
      }
      if (ev.type == SDL_CONTROLLERDEVICEREMOVED && pad) {
        if (ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
          SDL_GameControllerClose(pad);
          pad = nullptr;
          std::printf("Controller disconnected\n");
        }
      }
    }

    // Read triggers (0..32767)
    float lt = 0.f, rt = 0.f;
    if (pad) {
      Sint16 raw_l = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
      Sint16 raw_r = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
      lt = std::clamp(raw_l / 32767.f, 0.f, 1.f);
      rt = std::clamp(raw_r / 32767.f, 0.f, 1.f);
    }
    // Keyboard overrides / adds for testing
    if (key_left) lt = std::max(lt, 1.f);
    if (key_right) rt = std::max(rt, 1.f);

    ship.left_thrust = lt;
    ship.right_thrust = rt;

    Uint64 now = SDL_GetPerformanceCounter();
    float dt = static_cast<float>(now - prev) / static_cast<float>(freq);
    prev = now;
    // Clamp dt to avoid spiral after hitch
    dt = std::min(dt, 0.05f);

    ship.update(dt);

    // Render
    SDL_SetRenderDrawColor(ren, 8, 10, 24, 255);
    SDL_RenderClear(ren);

    // Simple starfield (deterministic-ish)
    SDL_SetRenderDrawColor(ren, 180, 190, 220, 255);
    for (int i = 0; i < 120; ++i) {
      int sx = (i * 97 + 13) % WINDOW_W;
      int sy = (i * 53 + 29) % WINDOW_H;
      SDL_RenderDrawPoint(ren, sx, sy);
    }

    draw_exhaust(ren, ship, true);
    draw_exhaust(ren, ship, false);

    // Ship sprite: texture nose = top; our angle is clockwise from nose-up → matches SDL
    SDL_Rect dst{static_cast<int>(ship.pos.x - 32), static_cast<int>(ship.pos.y - 32), 64, 64};
    double deg = ship.angle * 180.0 / PI;
    SDL_RenderCopyEx(ren, ship_tex, nullptr, &dst, deg, nullptr, SDL_FLIP_NONE);

    draw_heading_marker(ren, ship);

    // HUD
    // (no TTF; just colored bars for thrust)
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

    SDL_RenderPresent(ren);
  }

  if (pad) SDL_GameControllerClose(pad);
  SDL_DestroyTexture(ship_tex);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
