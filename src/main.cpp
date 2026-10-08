// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
// Co-authored-by: Grok <grok@x.ai>

#include <SDL.h>

#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <functional>
#include <string>

#include "audio.hpp"
#include "config.hpp"
#include "defs.hpp"
#include "game.hpp"
#include "render.hpp"
#include "systems.hpp"
#include "ui.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __EMSCRIPTEN__
namespace { UiState* web_ui = nullptr; }  // for the page's hooks

// Called by the web page when the tab is hidden or its Pause button pressed.
extern "C" EMSCRIPTEN_KEEPALIVE void dualthrust_pause() {
  if (web_ui && web_ui->screen == Screen::Play) {
    web_ui->screen = Screen::Pause;
    web_ui->page = MenuPage::Pause;
    web_ui->cursor = 0;
  }
}
#endif

namespace {

constexpr int WINDOW_W_DEFAULT = 1280;
constexpr int WINDOW_H_DEFAULT = 720;

struct Options {
  bool fullscreen = false, fullscreen_set = false;
  bool swap = false, swap_set = false;
  bool mute = false;
  int zoom = -1;  // ZOOM_LEVELS index, -1 = saved / automatic
  std::string renderer = "auto";  // auto, gles2 or sdl
  int ship = -1;
  unsigned seed = 0xC0FFEE;
  int win_w = WINDOW_W_DEFAULT, win_h = WINDOW_H_DEFAULT;
  // Debug / automation
  int frames = 0;              // exit after N frames (0 = run forever)
  const char* screenshot = nullptr;
  bool play = false;           // skip the title screen
  std::string screen;          // start on: title, pause or options (screenshots, testing)
  float hold[2] = {0.f, 0.f};  // constant engine input
};

void print_help(const char* argv0) {
  std::printf(
      "Usage: %s [options]\n"
      "\n"
      "CRT dual-engine cave lander. Triggers or stick-up control engines.\n"
      "\n"
      "Options:\n"
      "  -h, --help           Show this help\n"
      "  -V, --version        Show version\n"
      "  -f, --fullscreen     Start fullscreen\n"
      "  -w, --window WxH     Window size (e.g. 1280x720)\n"
      "  -s, --ship N         Ship preset index 0..%d (5=Topdog, 6=Canopy)\n"
      "  -S, --seed N         Cave generation seed (unsigned)\n"
      "  -x, --swap-engines   Swap left/right engine mapping\n"
      "  -z, --zoom LEVEL     View zoom: near, medium or far (0..2); Tab / D-pad change it in game\n"
      "  -m, --mute           Start with sound off\n"
      "  --renderer MODE      auto (default: GLES2, else SDL), gles2 or sdl\n"
      "  --config-dir PATH    Override XDG config directory\n"
      "\n"
      "Debug:\n"
      "  --play               Skip the title screen and start flying\n"
      "  --screen NAME        Start on title, pause or options\n"
      "  --thrust L,R         Hold both engines at fixed levels (0..1)\n"
      "  --frames N           Exit after N frames\n"
      "  --screenshot FILE    Save a BMP of the last frame (with --frames)\n"
      "\n"
      "Config: $XDG_CONFIG_HOME/dualthrust/config  (default ~/.config/dualthrust/)\n",
      argv0, SHIP_DEF_COUNT - 1);
}

// Returns -1 to continue, otherwise the process exit code.
int parse_args(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto need = [&](const char* opt) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "dualthrust: %s requires an argument\n", opt);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") { print_help(argv[0]); return 0; }
    else if (a == "-V" || a == "--version") { std::printf("dualthrust %s\n", APP_VERSION); return 0; }
    else if (a == "-f" || a == "--fullscreen") o.fullscreen = o.fullscreen_set = true;
    else if (a == "-x" || a == "--swap-engines") o.swap = o.swap_set = true;
    else if (a == "-s" || a == "--ship") o.ship = std::atoi(need("--ship"));
    else if (a == "-S" || a == "--seed") o.seed = static_cast<unsigned>(std::strtoul(need("--seed"), nullptr, 0));
    else if (a == "-z" || a == "--zoom") {
      const std::string v = need("--zoom");
      o.zoom = -1;
      for (int i = 0; i < ZOOM_COUNT; ++i) {
        std::string name = ZOOM_LEVELS[i].name;
        for (char& ch : name) ch = static_cast<char>(std::tolower(ch));
        if (v == name || v == std::to_string(i)) o.zoom = i;
      }
      if (o.zoom < 0) {
        std::fprintf(stderr, "dualthrust: bad zoom '%s' (near, medium, far)\n", v.c_str());
        return 2;
      }
    }
    else if (a == "--renderer") {
      o.renderer = need("--renderer");
      if (o.renderer != "auto" && o.renderer != "gles2" && o.renderer != "sdl") {
        std::fprintf(stderr, "dualthrust: bad renderer '%s' (auto, gles2, sdl)\n", o.renderer.c_str());
        return 2;
      }
    }
    else if (a == "-m" || a == "--mute") o.mute = true;
    else if (a == "--config-dir") set_config_dir_override(need("--config-dir"));
    else if (a == "--play") o.play = true;
    else if (a == "--screen") o.screen = need("--screen");
    else if (a == "--frames") o.frames = std::atoi(need("--frames"));
    else if (a == "--screenshot") o.screenshot = need("--screenshot");
    else if (a == "--thrust") {
      const char* v = need("--thrust");
      if (std::sscanf(v, "%f,%f", &o.hold[0], &o.hold[1]) != 2) {
        std::fprintf(stderr, "dualthrust: bad --thrust '%s' (use L,R)\n", v);
        return 2;
      }
    } else if (a == "-w" || a == "--window") {
      const char* v = need("--window");
      if (std::sscanf(v, "%dx%d", &o.win_w, &o.win_h) != 2 || o.win_w < 320 || o.win_h < 240) {
        std::fprintf(stderr, "dualthrust: bad window size '%s' (use WxH)\n", v);
        return 2;
      }
    } else {
      std::fprintf(stderr, "dualthrust: unknown option '%s' (try --help)\n", a.c_str());
      return 2;
    }
  }
  return -1;
}

void print_diagnostics(const char* argv0, const Options& o) {
  std::printf("dualthrust %s starting\n  executable: %s\n", APP_VERSION, argv0);
#if defined(__linux__)
  std::printf("  system:     Linux\n");
#elif defined(__APPLE__)
  std::printf("  system:     Apple\n");
#else
  std::printf("  system:     other\n");
#endif
#ifdef __VERSION__
  std::printf("  compiler:   %s\n", __VERSION__);
#endif
  const char* home = std::getenv("HOME");
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  const char* xst = std::getenv("XDG_STATE_HOME");
  std::printf("  HOME:       %s\n  XDG_CONFIG_HOME: %s\n  XDG_STATE_HOME:  %s\n", home ? home : "(unset)",
              xdg ? xdg : "(unset)", xst ? xst : "(unset)");
  std::printf("  config:     %s\n  state:      %s\n", config_file_path().c_str(), state_dir_path().c_str());
  char cwd[4096];
  if (getcwd(cwd, sizeof cwd)) std::printf("  cwd:        %s\n", cwd);
  std::printf("  window:     %dx%d\n  cave seed:  0x%08x (%u)\n", o.win_w, o.win_h, o.seed, o.seed);
  std::fflush(stdout);
}

// Engine levels 0..1 from triggers / shoulders / sticks (up = thrust) and the keyboard
void read_thrust(SDL_GameController* pad, float out[2]) {
  out[0] = out[1] = 0.f;
  if (pad) {
    auto trigger = [&](SDL_GameControllerAxis ax) {
      return clampf(SDL_GameControllerGetAxis(pad, ax) / 32767.f, 0.f, 1.f);
    };
    constexpr int DEAD = 8000;
    auto stick_up = [&](SDL_GameControllerAxis ax) {
      int raw = SDL_GameControllerGetAxis(pad, ax);
      return raw >= -DEAD ? 0.f : clampf((-raw - DEAD) / static_cast<float>(32768 - DEAD), 0.f, 1.f);
    };
    out[0] = std::max(trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT), stick_up(SDL_CONTROLLER_AXIS_LEFTY));
    out[1] = std::max(trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT), stick_up(SDL_CONTROLLER_AXIS_RIGHTY));
  }
  if (pad) {  // shoulder buttons: full thrust (handhelds without analog triggers)
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) out[0] = 1.f;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) out[1] = 1.f;
  }
  // Keyboard, one hand per engine: Ctrl (or A / D, arrows) = full thrust, Shift = half thrust
  const Uint8* keys = SDL_GetKeyboardState(nullptr);
  const bool full[2] = {keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT],
                        keys[SDL_SCANCODE_RCTRL] || keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]};
  const bool half[2] = {keys[SDL_SCANCODE_LSHIFT] != 0, keys[SDL_SCANCODE_RSHIFT] != 0};
  for (int i = 0; i < 2; ++i) {
    if (full[i]) out[i] = 1.f;
    else if (half[i]) out[i] = std::max(out[i], 0.5f);
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (int rc = parse_args(argc, argv, opt); rc >= 0) return rc;
  if (!check_user_dirs()) return 1;
  print_diagnostics(argv[0], opt);

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  {
    SDL_version v;
    SDL_GetVersion(&v);
    std::printf("  SDL:        %d.%d.%d\n  joysticks:  %d\n", v.major, v.minor, v.patch, SDL_NumJoysticks());
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
      std::printf("    [%d] %s\n", i, SDL_IsGameController(i) ? SDL_GameControllerNameForIndex(i) : "joystick");
    std::fflush(stdout);
  }

  // Window + renderer: our GLES2 batcher when a context can be made, else SDL's renderer (software,
  // headless drivers). The GL window needs its flag and attributes before it is created.
  SDL_Window* window = nullptr;
  std::unique_ptr<Backend> backend;
  const char* flavour = "";
  auto make_window = [&](bool gl) {
    if (window) SDL_DestroyWindow(window);
    if (gl) prepare_gles2_attributes();
    window = SDL_CreateWindow("dualthrust", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, opt.win_w, opt.win_h,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | (gl ? SDL_WINDOW_OPENGL : 0));
    if (!window) std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return window != nullptr;
  };
  if (opt.renderer != "sdl" && make_window(true)) {
    backend = create_gles2_backend(window);
    if (!backend) std::fprintf(stderr, "GLES2 unavailable, using the SDL renderer\n");
  }
  if (!backend) {
    if (opt.renderer == "gles2") {
      std::fprintf(stderr, "dualthrust: --renderer gles2 requested but unavailable\n");
      if (window) SDL_DestroyWindow(window);
      SDL_Quit();
      return 1;
    }
    if (make_window(false)) backend = create_sdl_backend(window);
  }
  Gfx gfx;
  if (!backend || !gfx.init(std::move(backend))) {
    std::fprintf(stderr, "renderer setup failed: %s\n", SDL_GetError());
    gfx.shutdown();
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  (void)flavour;
  std::printf("  renderer:   %s\n  video:      %s\n", gfx.renderer_name(), SDL_GetCurrentVideoDriver());

  // --- World ---
  Game game;
  std::printf("Generating cave...\n");
  game.cave.generate(opt.seed);
  std::printf("Cave ready (%d pads)\n", static_cast<int>(game.cave.pads.size()));
  game.rng = Rng(SDL_GetTicks() | 1u);

  UserConfig user = load_config();
  if (opt.ship >= 0) user.ship = opt.ship;
  if (user.ship < 0 || user.ship >= SHIP_DEF_COUNT) user.ship = DEFAULT_SHIP;
  if (opt.zoom >= 0) user.zoom = opt.zoom;
  if (opt.swap_set) user.swap_engines = opt.swap;
  if (opt.fullscreen_set) user.fullscreen = opt.fullscreen;

  create_ship(game, user.ship);
  auto aspect = [&] { return static_cast<float>(gfx.width()) / static_cast<float>(gfx.height()); };
  // Zoom level is resolution independent (world px visible vertically); the first start picks by screen size
  int display_h = gfx.height();
  SDL_DisplayMode desktop;
  if (SDL_GetDesktopDisplayMode(0, &desktop) == 0) display_h = desktop.h;  // the panel, not the window
  set_zoom(game, user.zoom >= 0 ? user.zoom : auto_zoom_for_height(display_h));
  update_view(game, 0.f, aspect(), true);
  auto new_game_at = [&](float wx) {
    respawn_ship(game, wx);
    snap_camera(game);
  };
  auto first_pad_x = [&] { return 0.5f * (game.cave.pads[0].x0 + game.cave.pads[0].x1); };
  new_game_at(first_pad_x());
  std::printf("  ship:       %s (%d)\n  swap L/R:   %s\n  fullscreen: %s\nReady.\n", SHIP_DEFS[user.ship].name,
              user.ship, user.swap_engines ? "yes" : "no", user.fullscreen ? "yes" : "no");
  std::fflush(stdout);

  UiState ui;
  ui.device = SDL_NumJoysticks() > 0 ? InputDevice::Gamepad : InputDevice::Keyboard;
  ui.fullscreen = user.fullscreen;
  ui.swap_engines = user.swap_engines;
  ui.sound = user.sound && !opt.mute;
  ui.crt = user.crt;
  ui.music_vol = std::clamp(user.music, 0, SLIDER_MAX);
  ui.sfx_vol = std::clamp(user.sfx, 0, SLIDER_MAX);
  if (opt.play) ui.screen = Screen::Play;
  else if (opt.screen == "pause") { ui.screen = Screen::Pause; ui.page = MenuPage::Pause; }
  else if (opt.screen == "options") { ui.screen = Screen::Pause; ui.page = MenuPage::Options; ui.options_back = MenuPage::Pause; }
  Audio audio;
  audio.init();
  audio.set_enabled(ui.sound);
  auto apply_volumes = [&] { audio.set_volumes(ui.music_vol / static_cast<float>(SLIDER_MAX), ui.sfx_vol / static_cast<float>(SLIDER_MAX)); };
  apply_volumes();
  bool running = true;

#ifdef __EMSCRIPTEN__
  ui.fullscreen = false;  // browsers only allow fullscreen from a user gesture
#else
  if (ui.fullscreen) SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
#endif

  auto persist_config = [&] {
    UserConfig c;
    c.fullscreen = ui.fullscreen;
    c.swap_engines = ui.swap_engines;
    c.sound = opt.mute ? user.sound : ui.sound;
    c.ship = ship_def_index(game);
    c.zoom = game.cam.zoom;
    c.crt = ui.crt;
    c.music = ui.music_vol;
    c.sfx = ui.sfx_vol;
    save_config(c);
  };
  auto sync_viewport = [&] {
    gfx.resize();
    update_view(game, 0.f, aspect(), true);
  };
  auto toggle_fullscreen = [&] {
    ui.fullscreen = !ui.fullscreen;
    SDL_SetWindowFullscreen(window, ui.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    SDL_PumpEvents();
    sync_viewport();
    persist_config();
  };
  auto new_cave = [&] {
    game.cave.generate(SDL_GetTicks());
    new_game_at(first_pad_x());
  };
  // Respawn on a random pad after a crash — never regenerates the cave
  auto respawn_after_crash = [&] {
    if (game.ecs.get<Flight>(game.ship).state != FlightState::Crashed) return;
    int pi = game.rng.range_i(0, static_cast<int>(game.cave.pads.size()) - 1);
    new_game_at(0.5f * (game.cave.pads[pi].x0 + game.cave.pads[pi].x1));
  };
  auto show_toast = [&](const char* text) {
    std::snprintf(ui.toast, sizeof ui.toast, "%s", text);
    ui.toast_timer = 1.6f;
  };
  auto zoom_to = [&](int index, bool wrap) {
    const int n = wrap ? (index % ZOOM_COUNT + ZOOM_COUNT) % ZOOM_COUNT : index;
    const int before = game.cam.zoom;
    set_zoom(game, n);
    char buf[32];
    std::snprintf(buf, sizeof buf, "ZOOM %s", ZOOM_LEVELS[game.cam.zoom].name);
    if (!ui.in_menu()) show_toast(buf);
    if (game.cam.zoom != before) persist_config();
  };
  auto cycle_ship = [&](int delta) {
    set_ship_def(game, ship_def_index(game) + delta);
    persist_config();
  };
  auto toggle_swap = [&] {
    ui.swap_engines = !ui.swap_engines;
    persist_config();
  };
  auto toggle_sound = [&] {
    ui.sound = !ui.sound;
    audio.set_enabled(ui.sound);
    show_toast(ui.sound ? "SOUND ON" : "SOUND OFF");
    persist_config();
  };

  // --- Menus: pages are data (ui.hpp); this is their behaviour ---
  auto open_page = [&](MenuPage p, int cursor = 0) {
    ui.page = p;
    ui.cursor = cursor;
  };
  auto menu_move = [&](int d) {
    const int n = page_def(ui.page).count;
    ui.cursor = (ui.cursor + n + d) % n;
  };
  auto start_game = [&] {
    new_game_at(first_pad_x());
    ui.screen = Screen::Play;
  };
  // delta = +1/-1 from left/right; `confirm` = Enter/A (cycles forward, wrapping sliders too)
  auto activate = [&](int delta, bool confirm) {
    const MenuItem& item = page_def(ui.page).items[ui.cursor];
    if (!confirm && item.kind == ItemKind::Action) return;
    auto slider = [&](int& v) {
      v = confirm ? (v + 1) % (SLIDER_MAX + 1) : std::clamp(v + delta, 0, SLIDER_MAX);
      apply_volumes();
      persist_config();
    };
    switch (item.action) {
      case MenuAction::Start: start_game(); break;
      case MenuAction::Resume: ui.screen = Screen::Play; break;
      case MenuAction::NewCave: new_cave(); ui.screen = Screen::Play; break;
      case MenuAction::Options: ui.options_back = ui.page; open_page(MenuPage::Options); break;
      case MenuAction::MainMenu: ui.screen = Screen::Title; open_page(MenuPage::Title); break;
      case MenuAction::Quit: persist_config(); running = false; break;
      case MenuAction::Back: open_page(ui.options_back, 1); break;  // back on its Options entry
      case MenuAction::Ship: cycle_ship(delta); break;
      case MenuAction::Zoom: zoom_to(game.cam.zoom + delta, true); break;
      case MenuAction::SwapEngines: toggle_swap(); break;
      case MenuAction::Crt: ui.crt = !ui.crt; persist_config(); break;
      case MenuAction::Fullscreen: toggle_fullscreen(); break;
      case MenuAction::Music: slider(ui.music_vol); break;
      case MenuAction::Effects: slider(ui.sfx_vol); break;
    }
  };
  auto menu_back = [&] {  // Escape / B
    switch (ui.page) {
      case MenuPage::Title:
#ifndef __EMSCRIPTEN__
        persist_config();
        running = false;
#endif
        break;
      case MenuPage::Pause: ui.screen = Screen::Play; break;
      case MenuPage::Options: open_page(ui.options_back, 1); break;
    }
  };
  auto open_pause = [&] {
    ui.screen = Screen::Pause;
    open_page(MenuPage::Pause);
  };

  SDL_GameController* pad = nullptr;
  for (int i = 0; i < SDL_NumJoysticks() && !pad; ++i)
    if (SDL_IsGameController(i)) pad = SDL_GameControllerOpen(i);

  Uint64 prev = SDL_GetPerformanceCounter();
  const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
  float accumulator = 0.f;
  int frame = 0;
  double t_sim = 0, t_draw = 0, t_present = 0;  // seconds, for --frames timing stats
  auto stamp = [&] { return SDL_GetPerformanceCounter() / freq; };

  // One frame; the browser drives it with requestAnimationFrame, desktop loops until quit.
  auto frame_fn = [&] {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      switch (ev.type) {
        case SDL_QUIT: running = false; break;

        case SDL_WINDOWEVENT:
          if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || ev.window.event == SDL_WINDOWEVENT_RESIZED ||
              ev.window.event == SDL_WINDOWEVENT_EXPOSED)
            sync_viewport();
          break;

        case SDL_KEYDOWN: {
          ui.device = InputDevice::Keyboard;
          const SDL_Keycode k = ev.key.keysym.sym;
          const bool enter = k == SDLK_RETURN || k == SDLK_KP_ENTER;
          if (enter && (ev.key.keysym.mod & KMOD_ALT)) { toggle_fullscreen(); break; }
          if (k == SDLK_f) { toggle_fullscreen(); break; }
          if (k == SDLK_m) { toggle_sound(); break; }
          if (ui.screen == Screen::Play) {
            if (k == SDLK_ESCAPE) open_pause();
            if (k == SDLK_TAB) zoom_to(game.cam.zoom + 1, true);
            if (k == SDLK_s) cycle_ship(+1);
            if (k == SDLK_x) toggle_swap();
            if (enter) respawn_after_crash();
            if (k == SDLK_g) new_cave();
          } else {
            if (k == SDLK_UP || k == SDLK_w) menu_move(-1);
            if (k == SDLK_DOWN || k == SDLK_s) menu_move(+1);
            if (k == SDLK_LEFT || k == SDLK_a) activate(-1, false);
            if (k == SDLK_RIGHT || k == SDLK_d) activate(+1, false);
            if (enter || k == SDLK_SPACE) activate(+1, true);
            if (k == SDLK_ESCAPE) menu_back();
          }
          break;
        }
        case SDL_CONTROLLERBUTTONDOWN: {
          ui.device = InputDevice::Gamepad;
          const int b = ev.cbutton.button;
          if (ui.screen == Screen::Play) {
            if (b == SDL_CONTROLLER_BUTTON_START) open_pause();
            if (b == SDL_CONTROLLER_BUTTON_BACK) cycle_ship(+1);
            if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) zoom_to(game.cam.zoom - 1, false);    // closer
            if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) zoom_to(game.cam.zoom + 1, false);  // farther
            if (b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_B) respawn_after_crash();
            if (b == SDL_CONTROLLER_BUTTON_Y) new_cave();
          } else {
            if (b == SDL_CONTROLLER_BUTTON_START) {
              if (ui.screen == Screen::Title) start_game();
              else ui.screen = Screen::Play;
            }
            if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) menu_move(-1);
            if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) menu_move(+1);
            if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT) activate(-1, false);
            if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) activate(+1, false);
            if (b == SDL_CONTROLLER_BUTTON_A) activate(+1, true);
            if (b == SDL_CONTROLLER_BUTTON_B) menu_back();
          }
          break;
        }
        case SDL_CONTROLLERAXISMOTION:  // sticks and triggers count as using the pad (ignore dead-zone noise)
          if (std::abs(ev.caxis.value) > 12000) ui.device = InputDevice::Gamepad;
          break;
        case SDL_MOUSEWHEEL:
          ui.device = InputDevice::Keyboard;
          if (ui.screen == Screen::Play && ev.wheel.y != 0) zoom_to(game.cam.zoom + (ev.wheel.y > 0 ? -1 : 1), false);
          break;
        case SDL_CONTROLLERDEVICEADDED:
          if (!pad) pad = SDL_GameControllerOpen(ev.cdevice.which);
          break;
        case SDL_CONTROLLERDEVICEREMOVED:
          if (pad && ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
            SDL_GameControllerClose(pad);
            pad = nullptr;
          }
          break;
      }
    }

    // Input → thruster levels (L/R swapped on request)
    float in[2] = {0.f, 0.f};
    if (ui.screen == Screen::Play) {
      read_thrust(pad, in);
      in[0] = std::max(in[0], opt.hold[0]);
      in[1] = std::max(in[1], opt.hold[1]);
    }
    if (ui.swap_engines) std::swap(in[0], in[1]);
    set_thrust(game, in[0], in[1]);
    const bool flying = game.ecs.get<Flight>(game.ship).state == FlightState::Flying;
    audio.set_engines(flying ? in[0] : 0.f, flying ? in[1] : 0.f);

    const Uint64 now = SDL_GetPerformanceCounter();
    const float dt = std::min(static_cast<float>((now - prev) / freq), 0.05f);
    prev = now;
    ui.time += dt;
    ui.toast_timer = std::max(0.f, ui.toast_timer - dt);
    update_view(game, dt, aspect());

    const double t0 = stamp();
    if (ui.screen != Screen::Pause) {  // the title screen runs the world too: the ship settles onto its pad
      // Fixed-step simulation, independent of display refresh
      accumulator += dt;
      for (int n = 0; accumulator >= tune::SIM_STEP && n < tune::MAX_STEPS_PER_FRAME; ++n) {
        step_sim(game, tune::SIM_STEP * tune::TIME_SCALE);
        accumulator -= tune::SIM_STEP;
      }
      accumulator = std::min(accumulator, tune::SIM_STEP);
      if (ui.screen == Screen::Title) {
        // Slow drift over the cave behind the logo, the ship staying in view
        const Vec2 p = ship_transform(game).pos;
        // (parked low and to the left, out from behind the menu text)
        game.cam.x = p.x - game.cam.vw * 0.36f + 80.f * std::sin(static_cast<float>(ui.time) * 0.13f);
        game.cam.y = p.y - game.cam.vh * 0.74f + 20.f * std::sin(static_cast<float>(ui.time) * 0.09f);
        game.cam.prev_ship_x = p.x;
      } else {
        update_camera(game, dt);
      }
    } else {
      accumulator = 0.f;
    }

    for (const SimEvent& ev : game.fired) audio.trigger(ev.kind, ev.strength);
    game.fired.clear();

    const double t1 = stamp();
    gfx.draw(game, ui);
    const double t2 = stamp();
    const bool last_frame = opt.frames > 0 && frame + 1 >= opt.frames;
    if (last_frame && opt.screenshot && !gfx.save_screenshot(opt.screenshot))  // before the buffer swap
      std::fprintf(stderr, "screenshot failed: %s\n", SDL_GetError());
    gfx.present();
    const double t3 = stamp();
    t_sim += t1 - t0;
    t_draw += t2 - t1;
    t_present += t3 - t2;

    if (opt.frames > 0 && ++frame >= opt.frames) {
      std::printf("timing: %d frames, per frame: sim %.2f ms, draw %.2f ms, present %.2f ms", frame,
                  1000 * t_sim / frame, 1000 * t_draw / frame, 1000 * t_present / frame);
      if (gfx.draw_calls() >= 0) std::printf(", %d GL draws in the last frame", gfx.draw_calls());
      std::printf("\n");
      running = false;
    }
  };

#ifdef __EMSCRIPTEN__
  // Never returns: main()'s locals stay alive for the callbacks, like a heap-allocated game would.
  static std::function<void()> web_frame;
  web_frame = [&] {
    if (running) frame_fn();
    else emscripten_cancel_main_loop();
  };
  web_ui = &ui;
  emscripten_set_main_loop([] { web_frame(); }, 0, 1);
#else
  while (running) frame_fn();
#endif

  persist_config();
  if (pad) SDL_GameControllerClose(pad);
  audio.shutdown();
  gfx.shutdown();
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
