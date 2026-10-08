// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <SDL.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "math.hpp"

// The 2D drawing primitives `Gfx` needs, behind two implementations:
//  - GLES2: our own batcher (one vertex buffer, one shader; it only flushes when the texture or blend
//    mode changes), so a frame is a handful of GL draws even on weak GPUs with old SDL.
//  - SDL_Renderer: the fallback (software rendering, headless testing, no GLES2 available).
// All coordinates are screen pixels, origin top-left; rects cover [x, x+w) x [y, y+h).
enum class Blend { Alpha, Add };

struct Texture;  // opaque, owned by the backend that made it

class Backend {
 public:
  virtual ~Backend() = default;
  virtual const char* name() const = 0;
  virtual void output_size(int& w, int& h) const = 0;

  virtual Texture* create_texture(int w, int h, const uint8_t* rgba, bool linear) = 0;
  virtual void destroy_texture(Texture* t) = 0;

  virtual void begin_frame(Rgba clear) = 0;
  virtual void end_frame() = 0;  // flush and present
  virtual void set_blend(Blend b) = 0;
  virtual void fill_rects(const SDL_Rect* rects, int n, Rgba c) = 0;
  virtual void line(int x0, int y0, int x1, int y1, Rgba c) = 0;  // 1 px, both end pixels included
  virtual void polygon(const SDL_Point* pts, int n, Rgba c) = 0;  // filled; at most one span per row
  virtual void gradient_triangle(SDL_Point a, SDL_Point b, SDL_Point apex, Rgba base, Rgba tip) = 0;
  virtual void copy(Texture* t, const SDL_Rect& dst, Rgba tint) = 0;  // whole texture, stretched to dst
  virtual int draw_calls() const { return -1; }  // GL draws submitted by the last finished frame (-1 unknown)
  virtual bool read_pixels(std::vector<uint8_t>& rgba, int& w, int& h) = 0;  // top-down RGBA, before end_frame
};

// nullptr when unavailable. The GLES2 backend needs a window created with SDL_WINDOW_OPENGL and the
// ES 2.0 context attributes (see prepare_gles2_attributes()).
void prepare_gles2_attributes();
std::unique_ptr<Backend> create_gles2_backend(SDL_Window* window);
std::unique_ptr<Backend> create_sdl_backend(SDL_Window* window);
