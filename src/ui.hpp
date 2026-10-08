// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Screens, menu pages (data) and the UI state shared by input handling and rendering.

#include <cstddef>

// Last device the player used; on-screen hints name its buttons.
enum class InputDevice { Keyboard, Gamepad };

enum class Screen { Title, Play, Pause };
enum class MenuPage { Title, Pause, Options, Stats };

enum class MenuAction {
  Start, Resume, NewCave, Options, Stats, MainMenu, Quit, Back,  // actions
  Ship, Zoom, SwapEngines, Crt, Fullscreen,               // choices (on/off or a list), changed with left/right
  Music, Effects,                                         // sliders 0..SLIDER_MAX
};
enum class ItemKind { Action, Choice, Slider };

struct MenuItem {
  MenuAction action;
  const char* label;
  ItemKind kind;
};
struct MenuPageDef {
  const char* title;
  const MenuItem* items;
  int count;
};

inline constexpr int SLIDER_MAX = 10;

inline constexpr MenuItem TITLE_ITEMS[] = {
    {MenuAction::Start, "START", ItemKind::Action},
    {MenuAction::Options, "OPTIONS", ItemKind::Action},
    {MenuAction::Stats, "STATISTICS", ItemKind::Action},
#ifndef __EMSCRIPTEN__  // a web page cannot quit
    {MenuAction::Quit, "QUIT", ItemKind::Action},
#endif
};
inline constexpr MenuItem PAUSE_ITEMS[] = {
    {MenuAction::Resume, "RESUME", ItemKind::Action},
    {MenuAction::NewCave, "NEW CAVE", ItemKind::Action},
    {MenuAction::Options, "OPTIONS", ItemKind::Action},
    {MenuAction::Stats, "STATISTICS", ItemKind::Action},
    {MenuAction::MainMenu, "MAIN MENU", ItemKind::Action},
#ifndef __EMSCRIPTEN__
    {MenuAction::Quit, "QUIT", ItemKind::Action},
#endif
};
inline constexpr MenuItem OPTION_ITEMS[] = {
    {MenuAction::Ship, "SHIP", ItemKind::Choice},
    {MenuAction::Zoom, "ZOOM", ItemKind::Choice},
    {MenuAction::SwapEngines, "SWAP ENGINES", ItemKind::Choice},
    {MenuAction::Music, "MUSIC", ItemKind::Slider},
    {MenuAction::Effects, "EFFECTS", ItemKind::Slider},
    {MenuAction::Crt, "CRT EFFECT", ItemKind::Choice},
    {MenuAction::Fullscreen, "FULLSCREEN", ItemKind::Choice},
    {MenuAction::Back, "BACK", ItemKind::Action},
};

inline constexpr MenuItem STATS_ITEMS[] = {
    {MenuAction::Back, "BACK", ItemKind::Action},
};

template <std::size_t N>
constexpr int item_count(const MenuItem (&)[N]) {
  return static_cast<int>(N);
}

inline const MenuPageDef& page_def(MenuPage p) {
  static const MenuPageDef defs[] = {
      {"DUALTHRUST", TITLE_ITEMS, item_count(TITLE_ITEMS)},
      {"PAUSED", PAUSE_ITEMS, item_count(PAUSE_ITEMS)},
      {"OPTIONS", OPTION_ITEMS, item_count(OPTION_ITEMS)},
      {"STATISTICS", STATS_ITEMS, item_count(STATS_ITEMS)},
  };
  return defs[static_cast<int>(p)];
}

struct UiState {
  Screen screen = Screen::Title;
  MenuPage page = MenuPage::Title;
  MenuPage options_back = MenuPage::Title;  // the page Options / Statistics was opened from
  int back_cursor = 0;                      // its cursor, restored on the way back
  int cursor = 0;
  bool fullscreen = false;
  bool swap_engines = false;
  bool sound = true;  // master switch (M); the sliders set the levels
  bool crt = true;    // scanlines + vignette
  int music_vol = 7, sfx_vol = 10;
  InputDevice device = InputDevice::Keyboard;
  double time = 0.0;      // real seconds, for UI animation
  char toast[40] = "";    // short message above the minimap, fading out
  float toast_timer = 0.f;

  bool in_menu() const { return screen != Screen::Play; }
};
