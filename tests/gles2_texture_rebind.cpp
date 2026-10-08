// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>

// Regression test for the GLES2 backend: a quad queued with texture A must still be drawn with A when
// texture B is created before the next draw. This happens whenever a world chunk is baked in the middle
// of a frame; the bug drew the previous chunk's quad with the new chunk's texture for one frame.
//
// Needs a GL ES capable display; headless: xvfb-run -a env SDL_VIDEODRIVER=x11 ./gles2_texture_rebind
// Exit code 0 = pass, 1 = fail, 2 = no GLES2 available (skipped).

#include <cstdio>
#include <vector>

#include "../src/backend.hpp"

int main() {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) return 2;
  prepare_gles2_attributes();
  SDL_Window* w = SDL_CreateWindow("test", 0, 0, 200, 100, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
  auto be = w ? create_gles2_backend(w) : nullptr;
  if (!be) {
    std::puts("SKIP: no GLES2");
    return 2;
  }
  const uint8_t red[4] = {255, 0, 0, 255}, blue[4] = {0, 0, 255, 255};
  Texture* a = be->create_texture(1, 1, red, false);
  be->begin_frame({0, 0, 0, 255});
  be->copy(a, SDL_Rect{10, 10, 40, 40}, {255, 255, 255, 255});  // queued, not yet flushed
  Texture* b = be->create_texture(1, 1, blue, false);           // "bake a chunk" mid-frame
  be->copy(b, SDL_Rect{100, 10, 40, 40}, {255, 255, 255, 255});
  std::vector<uint8_t> px;
  int pw = 0, ph = 0;
  if (!be->read_pixels(px, pw, ph)) return 1;
  const uint8_t* first = &px[(30 * pw + 30) * 4];
  const uint8_t* second = &px[(30 * pw + 120) * 4];
  const bool ok = first[0] == 255 && first[2] == 0 && second[0] == 0 && second[2] == 255;
  std::puts(ok ? "PASS" : "FAIL: a queued quad was drawn with the wrong texture");
  return ok ? 0 : 1;
}
