// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>

#include <algorithm>
#include <cstddef>
#include <cstdio>

#include "backend.hpp"

#ifdef __EMSCRIPTEN__
#include <GLES2/gl2.h>
#else
// The slice of OpenGL ES 2.0 we use, declared here so no GLES headers are needed to build (the
// functions are looked up at run time).
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLfloat = float;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;
constexpr GLenum GL_DEPTH_TEST = 0x0B71, GL_CULL_FACE = 0x0B44, GL_BLEND = 0x0BE2;
constexpr GLenum GL_SRC_ALPHA = 0x0302, GL_ONE_MINUS_SRC_ALPHA = 0x0303, GL_ONE = 1;
constexpr GLenum GL_TEXTURE_2D = 0x0DE1, GL_TEXTURE0 = 0x84C0, GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_WRAP_S = 0x2802,
                 GL_TEXTURE_WRAP_T = 0x2803, GL_LINEAR = 0x2601, GL_NEAREST = 0x2600, GL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31, GL_FRAGMENT_SHADER = 0x8B30, GL_COMPILE_STATUS = 0x8B81,
                 GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892, GL_STREAM_DRAW = 0x88E0, GL_FLOAT = 0x1406, GL_TRIANGLES = 0x0004;
constexpr GLenum GL_COLOR_BUFFER_BIT = 0x4000, GL_NO_ERROR = 0;
constexpr GLboolean GL_FALSE = 0, GL_TRUE = 1;
#endif

// OpenGL ES 2.0 backend with a vertex batcher. Everything — rects, lines, polygons, gradient triangles
// and textured quads — becomes triangles in one buffer drawn with one shader (colour * texture; solid
// shapes sample a 1x1 white texture). A flush happens only when the bound texture or the blend mode
// changes, or the buffer fills up. GL entry points come from SDL_GL_GetProcAddress, so nothing links
// against libGLESv2 and the same binary works on desktop (EGL), the R36S and WebGL.
struct Texture {
  GLuint id = 0;
  int w = 0, h = 0;
};

namespace {

struct Vtx {
  float x, y;
  uint8_t r, g, b, a;
  float u, v;
};

constexpr size_t MAX_VERTS = 6 * 16384;

const char* VERT_SRC =
    "attribute vec2 a_pos;\n"
    "attribute vec4 a_col;\n"
    "attribute vec2 a_uv;\n"
    "uniform vec2 u_screen;\n"
    "varying vec4 v_col;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 p = a_pos / u_screen * 2.0 - 1.0;\n"
    "  gl_Position = vec4(p.x, -p.y, 0.0, 1.0);\n"
    "  v_col = a_col;\n"
    "  v_uv = a_uv;\n"
    "}\n";

const char* FRAG_SRC =
    "precision mediump float;\n"
    "varying vec4 v_col;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(u_tex, v_uv) * v_col;\n"
    "}\n";

struct GL {
  GLuint (*CreateShader)(GLenum);
  void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
  void (*CompileShader)(GLuint);
  void (*GetShaderiv)(GLuint, GLenum, GLint*);
  void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
  void (*DeleteShader)(GLuint);
  GLuint (*CreateProgram)();
  void (*AttachShader)(GLuint, GLuint);
  void (*BindAttribLocation)(GLuint, GLuint, const GLchar*);
  void (*LinkProgram)(GLuint);
  void (*GetProgramiv)(GLuint, GLenum, GLint*);
  void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
  void (*UseProgram)(GLuint);
  void (*DeleteProgram)(GLuint);
  GLint (*GetUniformLocation)(GLuint, const GLchar*);
  void (*Uniform1i)(GLint, GLint);
  void (*Uniform2f)(GLint, GLfloat, GLfloat);
  void (*GenBuffers)(GLsizei, GLuint*);
  void (*BindBuffer)(GLenum, GLuint);
  void (*BufferData)(GLenum, GLsizeiptr, const void*, GLenum);
  void (*DeleteBuffers)(GLsizei, const GLuint*);
  void (*EnableVertexAttribArray)(GLuint);
  void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
  void (*DrawArrays)(GLenum, GLint, GLsizei);
  void (*GenTextures)(GLsizei, GLuint*);
  void (*BindTexture)(GLenum, GLuint);
  void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
  void (*TexParameteri)(GLenum, GLenum, GLint);
  void (*DeleteTextures)(GLsizei, const GLuint*);
  void (*ActiveTexture)(GLenum);
  void (*Enable)(GLenum);
  void (*Disable)(GLenum);
  void (*BlendFunc)(GLenum, GLenum);
  void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
  void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
  void (*Clear)(GLbitfield);
  void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
  void (*PixelStorei)(GLenum, GLint);
  GLenum (*GetError)();
};

bool load_gl(GL& gl) {
  bool ok = true;
#ifdef __EMSCRIPTEN__
#define LOAD(name) gl.name = &gl##name  // WebGL: link-time entry points
#else
#define LOAD(name)                                                                      \
  do {                                                                                  \
    gl.name = reinterpret_cast<decltype(gl.name)>(SDL_GL_GetProcAddress("gl" #name));   \
    if (!gl.name) { std::fprintf(stderr, "gles2: missing gl" #name "\n"); ok = false; } \
  } while (0)
#endif
  LOAD(CreateShader); LOAD(ShaderSource); LOAD(CompileShader); LOAD(GetShaderiv); LOAD(GetShaderInfoLog);
  LOAD(DeleteShader); LOAD(CreateProgram); LOAD(AttachShader); LOAD(BindAttribLocation); LOAD(LinkProgram);
  LOAD(GetProgramiv); LOAD(GetProgramInfoLog); LOAD(UseProgram); LOAD(DeleteProgram); LOAD(GetUniformLocation);
  LOAD(Uniform1i); LOAD(Uniform2f); LOAD(GenBuffers); LOAD(BindBuffer); LOAD(BufferData); LOAD(DeleteBuffers);
  LOAD(EnableVertexAttribArray); LOAD(VertexAttribPointer); LOAD(DrawArrays); LOAD(GenTextures);
  LOAD(BindTexture); LOAD(TexImage2D); LOAD(TexParameteri); LOAD(DeleteTextures); LOAD(ActiveTexture);
  LOAD(Enable); LOAD(Disable); LOAD(BlendFunc); LOAD(Viewport); LOAD(ClearColor); LOAD(Clear);
  LOAD(ReadPixels); LOAD(PixelStorei); LOAD(GetError);
#undef LOAD
  return ok;
}

class Gles2Backend : public Backend {
 public:
  Gles2Backend(SDL_Window* w, SDL_GLContext c) : win_(w), ctx_(c) {}

  ~Gles2Backend() override {
    if (white_) destroy_texture(white_);
    if (vbo_) gl_.DeleteBuffers(1, &vbo_);
    if (prog_) gl_.DeleteProgram(prog_);
    if (ctx_) SDL_GL_DeleteContext(ctx_);
  }

  bool init() {
    if (!load_gl(gl_)) return false;
    GLuint vs = compile(GL_VERTEX_SHADER, VERT_SRC), fs = compile(GL_FRAGMENT_SHADER, FRAG_SRC);
    if (!vs || !fs) return false;
    prog_ = gl_.CreateProgram();
    gl_.AttachShader(prog_, vs);
    gl_.AttachShader(prog_, fs);
    gl_.BindAttribLocation(prog_, 0, "a_pos");
    gl_.BindAttribLocation(prog_, 1, "a_col");
    gl_.BindAttribLocation(prog_, 2, "a_uv");
    gl_.LinkProgram(prog_);
    GLint ok = 0;
    gl_.GetProgramiv(prog_, GL_LINK_STATUS, &ok);
    gl_.DeleteShader(vs);
    gl_.DeleteShader(fs);
    if (!ok) {
      char log[512] = {};
      gl_.GetProgramInfoLog(prog_, sizeof log, nullptr, log);
      std::fprintf(stderr, "gles2: link failed: %s\n", log);
      return false;
    }
    u_screen_ = gl_.GetUniformLocation(prog_, "u_screen");
    gl_.UseProgram(prog_);
    gl_.Uniform1i(gl_.GetUniformLocation(prog_, "u_tex"), 0);
    gl_.GenBuffers(1, &vbo_);
    const uint8_t white[4] = {255, 255, 255, 255};
    white_ = create_texture(1, 1, white, false);
    verts_.reserve(MAX_VERTS);
    gl_.Disable(GL_DEPTH_TEST);
    gl_.Disable(GL_CULL_FACE);
    gl_.Enable(GL_BLEND);
    return white_ != nullptr && gl_.GetError() == GL_NO_ERROR;
  }

  const char* name() const override { return "gles2"; }

  void output_size(int& w, int& h) const override { SDL_GL_GetDrawableSize(win_, &w, &h); }

  Texture* create_texture(int w, int h, const uint8_t* rgba, bool linear) override {
    // Quads already queued belong to the texture bound now; draw them before the upload below rebinds
    // (a chunk baked in the middle of a frame otherwise made the previous chunk's quad use the new texture).
    flush();
    auto* t = new Texture{0, w, h};
    gl_.GenTextures(1, &t->id);
    gl_.ActiveTexture(GL_TEXTURE0);
    gl_.BindTexture(GL_TEXTURE_2D, t->id);
    const GLint filter = linear ? GL_LINEAR : GL_NEAREST;
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);  // NPOT needs clamp, no mipmaps
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl_.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    bound_ = nullptr;  // the binding above changed GL state behind the batcher's back
    return t;
  }

  void destroy_texture(Texture* t) override {
    if (!t) return;
    if (bound_ == t) { flush(); bound_ = nullptr; }
    gl_.DeleteTextures(1, &t->id);
    delete t;
  }

  void begin_frame(Rgba c) override {
    SDL_GL_GetDrawableSize(win_, &sw_, &sh_);
    gl_.Viewport(0, 0, sw_, sh_);
    gl_.ClearColor(c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f);
    gl_.Clear(GL_COLOR_BUFFER_BIT);
    gl_.UseProgram(prog_);
    gl_.Uniform2f(u_screen_, static_cast<float>(sw_), static_cast<float>(sh_));
    blend_ = Blend::Alpha;
    apply_blend();
    bound_ = nullptr;
  }

  void end_frame() override {
    flush();
    last_draws_ = draws_;
    draws_ = 0;
    SDL_GL_SwapWindow(win_);
  }

  int draw_calls() const override { return static_cast<int>(last_draws_); }

  void set_blend(Blend b) override {
    if (b == blend_) return;
    flush();
    blend_ = b;
    apply_blend();
  }

  void fill_rects(const SDL_Rect* r, int n, Rgba c) override {
    use(white_);
    for (int i = 0; i < n; ++i) quad(r[i].x, r[i].y, r[i].x + r[i].w, r[i].y + r[i].h, 0.f, 0.f, 0.f, 0.f, c);
  }

  // A 1 px wide quad along the segment through the pixel centres, extended half a pixel at both ends so
  // the end pixels are covered like SDL's lines.
  void line(int x0, int y0, int x1, int y1, Rgba c) override {
    use(white_);
    float ax = x0 + 0.5f, ay = y0 + 0.5f, bx = x1 + 0.5f, by = y1 + 0.5f;
    float dx = bx - ax, dy = by - ay;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) { dx = 1.f; dy = 0.f; } else { dx /= len; dy /= len; }
    const float hw = 0.55f;  // half width, a touch over 0.5 so diagonals do not break up
    const float ex = dx * 0.5f, ey = dy * 0.5f, nx = -dy * hw, ny = dx * hw;
    ax -= ex; ay -= ey; bx += ex; by += ey;
    push(ax + nx, ay + ny, c); push(ax - nx, ay - ny, c); push(bx + nx, by + ny, c);
    push(bx + nx, by + ny, c); push(ax - nx, ay - ny, c); push(bx - nx, by - ny, c);
  }

  void polygon(const SDL_Point* p, int n, Rgba c) override {  // triangle fan around the first point
    use(white_);
    for (int i = 1; i + 1 < n; ++i) {
      push(static_cast<float>(p[0].x), static_cast<float>(p[0].y), c);
      push(static_cast<float>(p[i].x), static_cast<float>(p[i].y), c);
      push(static_cast<float>(p[i + 1].x), static_cast<float>(p[i + 1].y), c);
    }
  }

  void gradient_triangle(SDL_Point a, SDL_Point b, SDL_Point apex, Rgba base, Rgba tip) override {
    use(white_);
    push(static_cast<float>(a.x), static_cast<float>(a.y), base);
    push(static_cast<float>(b.x), static_cast<float>(b.y), base);
    push(static_cast<float>(apex.x), static_cast<float>(apex.y), tip);
  }

  void copy(Texture* t, const SDL_Rect& d, Rgba tint) override {
    use(t);
    quad(d.x, d.y, d.x + d.w, d.y + d.h, 0.f, 0.f, 1.f, 1.f, tint);
  }

  bool read_pixels(std::vector<uint8_t>& rgba, int& w, int& h) override {
    flush();
    SDL_GL_GetDrawableSize(win_, &w, &h);
    std::vector<uint8_t> bottom_up(static_cast<size_t>(w) * h * 4);
    gl_.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, bottom_up.data());
    rgba.resize(bottom_up.size());
    for (int y = 0; y < h; ++y)
      std::copy_n(&bottom_up[static_cast<size_t>(h - 1 - y) * w * 4], static_cast<size_t>(w) * 4, &rgba[static_cast<size_t>(y) * w * 4]);
    return gl_.GetError() == GL_NO_ERROR;
  }

 private:
  GLuint compile(GLenum type, const char* src) {
    GLuint s = gl_.CreateShader(type);
    gl_.ShaderSource(s, 1, &src, nullptr);
    gl_.CompileShader(s);
    GLint ok = 0;
    gl_.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[512] = {};
      gl_.GetShaderInfoLog(s, sizeof log, nullptr, log);
      std::fprintf(stderr, "gles2: shader compile failed: %s\n", log);
      gl_.DeleteShader(s);
      return 0;
    }
    return s;
  }

  void apply_blend() { gl_.BlendFunc(GL_SRC_ALPHA, blend_ == Blend::Add ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA); }

  void use(Texture* t) {
    if (t == bound_) return;
    flush();
    bound_ = t;
    gl_.ActiveTexture(GL_TEXTURE0);
    gl_.BindTexture(GL_TEXTURE_2D, t->id);
  }

  void push(float x, float y, Rgba c, float u = 0.f, float v = 0.f) {
    if (verts_.size() >= MAX_VERTS) flush();
    verts_.push_back({x, y, c.r, c.g, c.b, c.a, u, v});
  }

  void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Rgba c) {
    if (verts_.size() + 6 > MAX_VERTS) flush();
    verts_.push_back({x0, y0, c.r, c.g, c.b, c.a, u0, v0});
    verts_.push_back({x1, y0, c.r, c.g, c.b, c.a, u1, v0});
    verts_.push_back({x0, y1, c.r, c.g, c.b, c.a, u0, v1});
    verts_.push_back({x1, y0, c.r, c.g, c.b, c.a, u1, v0});
    verts_.push_back({x1, y1, c.r, c.g, c.b, c.a, u1, v1});
    verts_.push_back({x0, y1, c.r, c.g, c.b, c.a, u0, v1});
  }

  void flush() {
    if (verts_.empty()) return;
    gl_.BindBuffer(GL_ARRAY_BUFFER, vbo_);
    gl_.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts_.size() * sizeof(Vtx)), verts_.data(), GL_STREAM_DRAW);
    gl_.EnableVertexAttribArray(0);
    gl_.EnableVertexAttribArray(1);
    gl_.EnableVertexAttribArray(2);
    gl_.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), reinterpret_cast<const void*>(offsetof(Vtx, x)));
    gl_.VertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), reinterpret_cast<const void*>(offsetof(Vtx, r)));
    gl_.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), reinterpret_cast<const void*>(offsetof(Vtx, u)));
    gl_.DrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts_.size()));
    ++draws_;
    verts_.clear();
  }

  SDL_Window* win_;
  SDL_GLContext ctx_;
  GL gl_{};
  GLuint prog_ = 0, vbo_ = 0;
  GLint u_screen_ = -1;
  Texture* white_ = nullptr;
  Texture* bound_ = nullptr;
  Blend blend_ = Blend::Alpha;
  int sw_ = 0, sh_ = 0;
  unsigned draws_ = 0, last_draws_ = 0;
  std::vector<Vtx> verts_;
};

}  // namespace

void prepare_gles2_attributes() {
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
}

std::unique_ptr<Backend> create_gles2_backend(SDL_Window* window) {
  SDL_GLContext ctx = SDL_GL_CreateContext(window);
  if (!ctx) {
    std::fprintf(stderr, "gles2: no context: %s\n", SDL_GetError());
    return nullptr;
  }
  SDL_GL_SetSwapInterval(1);
  auto be = std::make_unique<Gles2Backend>(window, ctx);
  if (!be->init()) return nullptr;
  return be;
}
