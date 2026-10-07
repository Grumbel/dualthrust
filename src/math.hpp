// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

constexpr float PI = 3.14159265358979323846f;

struct Vec2 {
  float x = 0.f, y = 0.f;
  constexpr Vec2() = default;
  constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
  constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
  constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
  constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
  Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
};

inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float length(Vec2 v) { return std::sqrt(dot(v, v)); }
inline Vec2 rotate(Vec2 v, float angle) {
  float c = std::cos(angle), s = std::sin(angle);
  return {c * v.x - s * v.y, s * v.x + c * v.y};
}

inline float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float smoothstep(float t) { t = clampf(t, 0.f, 1.f); return t * t * (3.f - 2.f * t); }

struct Rgba {
  uint8_t r, g, b, a;
};
constexpr Rgba with_alpha(Rgba c, uint8_t a) { return {c.r, c.g, c.b, a}; }
inline Rgba mix(Rgba a, Rgba b, float t) {
  auto m = [t](uint8_t x, uint8_t y) { return static_cast<uint8_t>(lerpf(x, y, clampf(t, 0.f, 1.f)) + 0.5f); };
  return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}

// Small LCG; cave generation depends on its exact sequence so seeds stay stable.
struct Rng {
  uint32_t state = 1;
  explicit Rng(uint32_t seed = 1) : state(seed ? seed : 1u) {}
  float next() {  // [0,1)
    state = state * 1664525u + 1013904223u;
    return (state >> 8) / static_cast<float>(1u << 24);
  }
  float range(float lo, float hi) { return lo + (hi - lo) * next(); }
  int range_i(int lo, int hi) { return lo + static_cast<int>(next() * (hi - lo + 1)); }
};
