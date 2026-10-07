// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <SDL.h>

#include <vector>

#include "game.hpp"
#include "ui.hpp"

// All drawing. Static art (font, rock tiles, CRT overlay, minimap) is baked into textures once
// so a frame is a few hundred batched draw calls instead of tens of thousands of lines.
class Gfx {
 public:
  bool init(SDL_Renderer* ren);
  void shutdown();
  void resize();  // call after window size / fullscreen changes
  int width() const { return w_; }
  int height() const { return h_; }
  void draw(const Game& g, const UiState& ui);
  bool save_screenshot(const char* path) const;

 private:
  struct View { float ox, oy; };

  void build_rock_tiles();
  void build_overlay();
  void build_minimap(const Cave& cave);

  void color(Rgba c) const;
  void line(int x0, int y0, int x1, int y1, Rgba c) const;
  void polygon(const SDL_Point* pts, int n, Rgba c) const;
  void gradient_triangle(SDL_Point a, SDL_Point b, SDL_Point apex, Rgba base, Rgba tip) const;
  void fill(int x, int y, int w, int h, Rgba c) const;
  void outline(int x, int y, int w, int h, Rgba c) const;
  void text(int x, int y, const char* s, Rgba c) const;
  int text_width(const char* s) const;
  void text_centered(int cx, int y, const char* s, Rgba c) const { text(cx - text_width(s) / 2, y, s, c); }
  int sx(const Camera& cam, float wx) const;
  int sy(float wy) const;

  void draw_world(const Game& g);
  void render_chunk(const Cave& cave, int cx, int cy, std::vector<uint8_t>& px) const;
  void draw_pads(const Game& g, double t) const;
  void draw_particles(const Game& g) const;
  void draw_ship(const Game& g, double t) const;
  void draw_hud(const Game& g) const;
  void draw_minimap(const Game& g, double t);
  void draw_menu(const Game& g, const UiState& ui) const;

  SDL_Renderer* ren_ = nullptr;
  int w_ = 1280, h_ = 720;
  View view_{0.f, 0.f};
  const char* glyph_bits_[95] = {};  // 3x5 bit strings per printable ASCII char
  SDL_Texture* overlay_ = nullptr;
  SDL_Texture* minimap_ = nullptr;
  unsigned minimap_generation_ = 0;

  // Static world art (stars, rock, contour) is rasterised on the CPU into CHUNK x CHUNK textures and
  // cached, so the world costs a handful of draw calls per frame instead of thousands.
  struct Chunk {
    SDL_Texture* tex = nullptr;
    unsigned last_used = 0;
  };
  std::vector<Chunk> chunks_;
  std::vector<uint8_t> rock_px_;  // MAX_DEPTH tiles of TILE x TILE RGBA
  std::vector<uint8_t> chunk_px_;  // scratch buffer
  unsigned chunk_generation_ = ~0u;
  unsigned frame_ = 0;
  int chunk_count_ = 0;
};
