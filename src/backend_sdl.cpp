// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>

#include <algorithm>
#include <cstdio>

#include "backend.hpp"

// SDL_Renderer backend. Keep to the SDL 2.0.10 API (the R36S ships that): no SDL_RenderGeometry.
struct Texture {
  SDL_Texture* tex;
};

namespace {

class SdlBackend : public Backend {
 public:
  explicit SdlBackend(SDL_Renderer* r) : ren_(r) { SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND); }
  ~SdlBackend() override { if (ren_) SDL_DestroyRenderer(ren_); }

  const char* name() const override { return "sdl-renderer"; }
  void output_size(int& w, int& h) const override { SDL_GetRendererOutputSize(ren_, &w, &h); }

  Texture* create_texture(int w, int h, const uint8_t* rgba, bool linear) override {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, linear ? "linear" : "nearest");
    SDL_Texture* t = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return nullptr;
    SDL_UpdateTexture(t, nullptr, rgba, w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return new Texture{t};
  }
  void destroy_texture(Texture* t) override {
    if (!t) return;
    SDL_DestroyTexture(t->tex);
    delete t;
  }

  void begin_frame(Rgba c) override {
    color(c);
    SDL_RenderClear(ren_);
  }
  void end_frame() override { SDL_RenderPresent(ren_); }
  void set_blend(Blend b) override {
    SDL_SetRenderDrawBlendMode(ren_, b == Blend::Add ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
  }
  void fill_rects(const SDL_Rect* r, int n, Rgba c) override {
    if (n <= 0) return;
    color(c);
    SDL_RenderFillRects(ren_, r, n);
  }
  void line(int x0, int y0, int x1, int y1, Rgba c) override {
    color(c);
    SDL_RenderDrawLine(ren_, x0, y0, x1, y1);
  }

  // Scanline fill, one FillRects call
  void polygon(const SDL_Point* pts, int n, Rgba c) override {
    rects_.clear();
    int ymin = pts[0].y, ymax = pts[0].y;
    for (int i = 1; i < n; ++i) { ymin = std::min(ymin, pts[i].y); ymax = std::max(ymax, pts[i].y); }
    for (int y = ymin; y <= ymax; ++y) {
      const float fy = y + 0.5f;
      float lo = 1e9f, hi = -1e9f;
      for (int i = 0; i < n; ++i) {
        const SDL_Point p = pts[i], q = pts[(i + 1) % n];
        if (fy < std::min(p.y, q.y) || fy >= std::max(p.y, q.y) || p.y == q.y) continue;
        const float x = p.x + (q.x - p.x) * (fy - p.y) / static_cast<float>(q.y - p.y);
        lo = std::min(lo, x);
        hi = std::max(hi, x);
      }
      if (lo <= hi) {
        const int x0 = static_cast<int>(std::floor(lo + 0.5f)), x1 = static_cast<int>(std::floor(hi + 0.5f));
        rects_.push_back({x0, y, std::max(1, x1 - x0), 1});
      }
    }
    fill_rects(rects_.data(), static_cast<int>(rects_.size()), c);
  }

  // A few flat slices stand in for the gradient
  void gradient_triangle(SDL_Point a, SDL_Point b, SDL_Point c, Rgba base, Rgba tip) override {
    constexpr int SLICES = 5;
    auto at = [](SDL_Point p, SDL_Point q, float t) {
      return SDL_Point{static_cast<int>(std::lround(lerpf(p.x, q.x, t))), static_cast<int>(std::lround(lerpf(p.y, q.y, t)))};
    };
    for (int i = 0; i < SLICES; ++i) {
      const float t0 = static_cast<float>(i) / SLICES, t1 = static_cast<float>(i + 1) / SLICES;
      const SDL_Point quad[4] = {at(a, c, t0), at(b, c, t0), at(b, c, t1), at(a, c, t1)};
      polygon(quad, 4, mix(base, tip, (t0 + t1) * 0.5f));
    }
  }

  void copy_part(Texture* t, const SDL_Rect& src, const SDL_Rect& dst, Rgba tint) override {
    SDL_SetTextureColorMod(t->tex, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaMod(t->tex, tint.a);
    SDL_RenderCopy(ren_, t->tex, &src, &dst);
  }
  void copy(Texture* t, const SDL_Rect& dst, Rgba tint) override {
    SDL_SetTextureColorMod(t->tex, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaMod(t->tex, tint.a);
    SDL_RenderCopy(ren_, t->tex, nullptr, &dst);
  }

  bool read_pixels(std::vector<uint8_t>& rgba, int& w, int& h) override {
    SDL_GetRendererOutputSize(ren_, &w, &h);
    rgba.resize(static_cast<size_t>(w) * h * 4);
    return SDL_RenderReadPixels(ren_, nullptr, SDL_PIXELFORMAT_RGBA32, rgba.data(), w * 4) == 0;
  }

 private:
  void color(Rgba c) { SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, c.a); }
  SDL_Renderer* ren_;
  std::vector<SDL_Rect> rects_;
};

}  // namespace

std::unique_ptr<Backend> create_sdl_backend(SDL_Window* window) {
  SDL_Renderer* ren = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren) ren = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!ren) return nullptr;
  return std::make_unique<SdlBackend>(ren);
}
