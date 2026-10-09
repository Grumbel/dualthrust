// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <SDL.h>

#include <vector>

#include "backend.hpp"
#include "game.hpp"
#include "input.hpp"
#include "ui.hpp"

// All drawing. Static art (font, rock tiles, CRT overlay, minimap) is baked into textures once
// so a frame is a few hundred batched draw calls instead of tens of thousands of lines.
class Gfx {
 public:
  bool init(std::unique_ptr<Backend> backend);
  const char* renderer_name() const { return be_->name(); }
  int draw_calls() const { return be_->draw_calls(); }  // of the last presented frame, -1 if unknown
  void present() { be_->end_frame(); }                 // after draw() and any save_screenshot()
  void shutdown();
  void resize();  // call after window size / fullscreen changes
  int width() const { return w_; }
  int height() const { return h_; }
  void draw(Game& g, const UiState& ui, const BindMap& binds);
  bool save_screenshot(const char* path) const;

 private:
  // World layer: world px * scale_ = screen px; the offsets are the camera's position in screen px
  struct View { int ox, oy; };

  void build_overlay();
  void build_minimap(const Cave& cave, const std::vector<uint8_t>& revealed);

  void line(int x0, int y0, int x1, int y1, Rgba c) const;
  void fill(int x, int y, int w, int h, Rgba c) const;
  void outline(int x, int y, int w, int h, Rgba c) const;
  void text(int x, int y, const char* s, Rgba c, int scale = 1) const;
  int text_width(const char* s, int scale = 1) const;
  void text_centered(int cx, int y, const char* s, Rgba c, int scale = 1) const {
    text(cx - text_width(s, scale) / 2, y, s, c, scale);
  }
  // Pixel-font cell size at the current UI scale (scale=1 text uses these).
  int cell_w() const { return 4 * font_scale_; }
  int cell_h() const { return 5 * font_scale_; }
  // Layout helper: values authored for the 2X (font=3) baseline, scaled to the active font.
  int L(int px_at_2x) const { int v = (px_at_2x * font_scale_ + 1) / 3; return v < 1 ? 1 : v; }
  int sx(const Camera& cam, float wx) const;  // world -> screen px (zoom applied)
  int sy(float wy) const;
  int Z(float world_len) const;               // a world length in screen px, at least 1

  void draw_world(const Game& g);
  void render_chunk(const Cave& cave, int cx, int cy, std::vector<uint8_t>& px, int& tex_w, int& tex_h) const;
  void drop_chunks();
  void draw_pads(const Game& g, double t) const;
  void draw_particles(const Game& g) const;
  void draw_ship(const Game& g, double t) const;
  void draw_cargo(const Game& g, double t) const;
  void draw_rope(const Game& g, double t) const;
  void draw_hud(const Game& g, const UiState& ui, const BindMap& binds) const;
  void draw_minimap(Game& g, double t);
  void draw_residues(const Game& g) const;
  void draw_sonar(const Game& g) const;
  void draw_full_map(const Game& g, const UiState& ui) const;
  void draw_menu(const Game& g, const UiState& ui, const BindMap& binds) const;
  void draw_title(const Game& g, const UiState& ui) const;  // logo + the title page
  void item_value(const MenuItem& item, const Game& g, const UiState& ui, char* buf, size_t n) const;

  std::unique_ptr<Backend> be_;
  int w_ = 1280, h_ = 720;           // screen (output) size in px
  float scale_ = 1.f;                // screen px per world px (animates while zooming)
  float bake_scale_ = 1.f;           // scale the cached chunk textures were rasterised at (the zoom level's)
  int font_scale_ = 3;              // 3x5 glyph multiplier; set from UiState::ui_scale each frame
  View view_{0, 0};
  const char* glyph_bits_[95] = {};  // 3x5 bit strings per printable ASCII char
  Texture* overlay_ = nullptr;
  Texture* minimap_ = nullptr;
  unsigned minimap_generation_ = 0;

  // Static world art (stars, rock, contour) is rasterised on the CPU into CHUNK x CHUNK textures and
  // cached, so the world costs a handful of draw calls per frame instead of thousands.
  struct Chunk {
    Texture* tex = nullptr;
    unsigned last_used = 0;
  };
  std::vector<Chunk> chunks_;
  std::vector<uint8_t> chunk_px_;  // scratch buffer
  unsigned chunk_generation_ = ~0u;
  unsigned frame_ = 0;
  int chunk_count_ = 0;
  float chunk_scale_ = 0.f;  // bake scale of the cached chunks; a change of zoom level re-rasterises them
};
