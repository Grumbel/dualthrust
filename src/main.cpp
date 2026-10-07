// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
// Co-authored-by: Grok <grok@x.ai>

#include <SDL.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "audio.hpp"
#include "config.hpp"
#include "defs.hpp"
#include "game.hpp"
#include "render.hpp"
#include "systems.hpp"
#include "ui.hpp"

namespace {

constexpr int WINDOW_W_DEFAULT = 1280;
constexpr int WINDOW_H_DEFAULT = 720;

struct Options {
  bool fullscreen = false, fullscreen_set = false;
  bool swap = false, swap_set = false;
  bool mute = false;
  int ship = -1;
  unsigned seed = 0xC0FFEE;
  int win_w = WINDOW_W_DEFAULT, win_h = WINDOW_H_DEFAULT;
  // Debug / automation
  int frames = 0;              // exit after N frames (0 = run forever)
  const char* screenshot = nullptr;
  bool play = false;           // skip the menu
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
      "  -m, --mute           Start with sound off\n"
      "  --config-dir PATH    Override XDG config directory\n"
      "\n"
      "Debug:\n"
      "  --play               Skip the menu and start flying\n"
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
    else if (a == "-m" || a == "--mute") o.mute = true;
    else if (a == "--config-dir") set_config_dir_override(need("--config-dir"));
    else if (a == "--play") o.play = true;
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
  std::printf("  HOME:       %s\n  XDG_CONFIG_HOME: %s\n", home ? home : "(unset)", xdg ? xdg : "(unset)");
  std::printf("  config:     %s\n", config_file_path().c_str());
  char cwd[4096];
  if (getcwd(cwd, sizeof cwd)) std::printf("  cwd:        %s\n", cwd);
  std::printf("  window:     %dx%d\n  cave seed:  0x%08x (%u)\n", o.win_w, o.win_h, o.seed, o.seed);
  std::fflush(stdout);
}

// Engine levels 0..1 from triggers / sticks (up = thrust) and the keyboard
void read_thrust(SDL_GameController* pad, bool key_left, bool key_right, float out[2]) {
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
  if (key_left) out[0] = 1.f;
  if (key_right) out[1] = 1.f;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (int rc = parse_args(argc, argv, opt); rc >= 0) return rc;
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

  SDL_Window* window = SDL_CreateWindow("dualthrust", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, opt.win_w,
                                        opt.win_h, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (!window) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_Renderer* ren = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren) ren = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  Gfx gfx;
  if (!ren || !gfx.init(ren)) {
    std::fprintf(stderr, "renderer setup failed: %s\n", SDL_GetError());
    gfx.shutdown();
    if (ren) SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  {
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(ren, &info) == 0)
      std::printf("  renderer:   %s (%s)\n", info.name, (info.flags & SDL_RENDERER_ACCELERATED) ? "accelerated" : "software");
    std::printf("  video:      %s\n", SDL_GetCurrentVideoDriver());
  }

  // --- World ---
  Game game;
  std::printf("Generating cave...\n");
  game.cave.generate(opt.seed);
  std::printf("Cave ready (%d pads)\n", static_cast<int>(game.cave.pads.size()));
  game.rng = Rng(SDL_GetTicks() | 1u);

  UserConfig user = load_config();
  if (opt.ship >= 0) user.ship = opt.ship;
  if (user.ship < 0 || user.ship >= SHIP_DEF_COUNT) user.ship = DEFAULT_SHIP;
  if (opt.swap_set) user.swap_engines = opt.swap;
  if (opt.fullscreen_set) user.fullscreen = opt.fullscreen;

  create_ship(game, user.ship);
  game.cam.vw = static_cast<float>(gfx.width());
  game.cam.vh = static_cast<float>(gfx.height());
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
  ui.menu_open = !opt.play;
  ui.fullscreen = user.fullscreen;
  ui.swap_engines = user.swap_engines;
  ui.sound = user.sound && !opt.mute;
  Audio audio;
  audio.init();
  audio.set_enabled(ui.sound);
  bool running = true;

  if (ui.fullscreen) SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);

  auto persist_config = [&] {
    UserConfig c;
    c.fullscreen = ui.fullscreen;
    c.swap_engines = ui.swap_engines;
    c.sound = opt.mute ? user.sound : ui.sound;
    c.ship = ship_def_index(game);
    save_config(c);
  };
  auto sync_viewport = [&] {
    gfx.resize();
    game.cam.vw = static_cast<float>(gfx.width());
    game.cam.vh = static_cast<float>(gfx.height());
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
  // Respawn / relight only — never regenerates the cave
  auto reset_or_relight = [&] {
    if (game.ecs.get<Flight>(game.ship).state == FlightState::Landed) {
      relight_ship(game);
    } else {
      int pi = game.rng.range_i(0, static_cast<int>(game.cave.pads.size()) - 1);
      new_game_at(0.5f * (game.cave.pads[pi].x0 + game.cave.pads[pi].x1));
    }
  };
  auto cycle_ship = [&] {
    set_ship_def(game, ship_def_index(game) + 1);
    persist_config();
  };
  auto toggle_swap = [&] {
    ui.swap_engines = !ui.swap_engines;
    persist_config();
  };
  auto toggle_sound = [&] {
    ui.sound = !ui.sound;
    audio.set_enabled(ui.sound);
    persist_config();
  };
  auto activate_menu = [&] {
    switch (MENU_ITEMS[ui.cursor].action) {
      case MenuAction::Resume: ui.menu_open = false; break;
      case MenuAction::Fullscreen: toggle_fullscreen(); break;
      case MenuAction::NewCave: new_cave(); ui.menu_open = false; break;
      case MenuAction::Ship: cycle_ship(); break;
      case MenuAction::SwapEngines: toggle_swap(); break;
      case MenuAction::Sound: toggle_sound(); break;
      case MenuAction::Quit: persist_config(); running = false; break;
    }
  };
  auto menu_move = [&](int d) { ui.cursor = (ui.cursor + MENU_COUNT + d) % MENU_COUNT; };

  SDL_GameController* pad = nullptr;
  for (int i = 0; i < SDL_NumJoysticks() && !pad; ++i)
    if (SDL_IsGameController(i)) pad = SDL_GameControllerOpen(i);

  bool key_left = false, key_right = false;
  Uint64 prev = SDL_GetPerformanceCounter();
  const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
  float accumulator = 0.f;
  int frame = 0;
  double t_sim = 0, t_draw = 0, t_present = 0;  // seconds, for --frames timing stats
  auto stamp = [&] { return SDL_GetPerformanceCounter() / freq; };

  while (running) {
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
          const SDL_Keycode k = ev.key.keysym.sym;
          if (k == SDLK_ESCAPE) {
            if (ui.menu_open) running = false;
            else ui.menu_open = true;
          } else if ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && (ev.key.keysym.mod & KMOD_ALT)) {
            toggle_fullscreen();
          } else if (ui.menu_open) {
            if (k == SDLK_UP || k == SDLK_w) menu_move(-1);
            if (k == SDLK_DOWN || k == SDLK_s) menu_move(+1);
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) activate_menu();
          } else {
            if (k == SDLK_a || k == SDLK_LEFT) key_left = true;
            if (k == SDLK_d || k == SDLK_RIGHT) key_right = true;
            if (k == SDLK_TAB) cycle_ship();
            if (k == SDLK_x) toggle_swap();
            if (k == SDLK_m) toggle_sound();
            if (k == SDLK_r) reset_or_relight();
            if (k == SDLK_f) toggle_fullscreen();
            if (k == SDLK_g) new_cave();
          }
          break;
        }
        case SDL_KEYUP:
          if (ev.key.keysym.sym == SDLK_a || ev.key.keysym.sym == SDLK_LEFT) key_left = false;
          if (ev.key.keysym.sym == SDLK_d || ev.key.keysym.sym == SDLK_RIGHT) key_right = false;
          break;

        case SDL_CONTROLLERBUTTONDOWN: {
          const int b = ev.cbutton.button;
          if (b == SDL_CONTROLLER_BUTTON_START) ui.menu_open = !ui.menu_open;
          else if (ui.menu_open) {
            if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) menu_move(-1);
            if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) menu_move(+1);
            if (b == SDL_CONTROLLER_BUTTON_A) activate_menu();
            if (b == SDL_CONTROLLER_BUTTON_B) ui.menu_open = false;
          } else {
            if (b == SDL_CONTROLLER_BUTTON_BACK) cycle_ship();
            if (b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_B) reset_or_relight();
            if (b == SDL_CONTROLLER_BUTTON_Y) new_cave();
          }
          break;
        }
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
    if (!ui.menu_open) {
      read_thrust(pad, key_left, key_right, in);
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

    const double t0 = stamp();
    if (!ui.menu_open) {
      // Fixed-step simulation, independent of display refresh
      accumulator += dt;
      for (int n = 0; accumulator >= tune::SIM_STEP && n < tune::MAX_STEPS_PER_FRAME; ++n) {
        step_sim(game, tune::SIM_STEP * tune::TIME_SCALE);
        accumulator -= tune::SIM_STEP;
      }
      accumulator = std::min(accumulator, tune::SIM_STEP);
      update_camera(game, dt);
    } else {
      accumulator = 0.f;
    }

    for (const SimEvent& ev : game.fired) audio.trigger(ev.kind, ev.strength);
    game.fired.clear();

    const double t1 = stamp();
    gfx.draw(game, ui);
    const double t2 = stamp();
    SDL_RenderPresent(ren);
    const double t3 = stamp();
    t_sim += t1 - t0;
    t_draw += t2 - t1;
    t_present += t3 - t2;

    if (opt.frames > 0 && ++frame >= opt.frames) {
      std::printf("timing: %d frames, per frame: sim %.2f ms, draw %.2f ms, present %.2f ms\n", frame,
                  1000 * t_sim / frame, 1000 * t_draw / frame, 1000 * t_present / frame);
      if (opt.screenshot && !gfx.save_screenshot(opt.screenshot))
        std::fprintf(stderr, "screenshot failed: %s\n", SDL_GetError());
      break;
    }
  }

  persist_config();
  if (pad) SDL_GameControllerClose(pad);
  audio.shutdown();
  gfx.shutdown();
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
