// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Menu definition (data) and the UI state shared by input handling and rendering.

enum class MenuAction { Resume, Fullscreen, NewCave, Ship, Zoom, SwapEngines, Sound, Quit };

struct MenuItem {
  MenuAction action;
  const char* label;
};

inline constexpr MenuItem MENU_ITEMS[] = {
    {MenuAction::Resume, "RESUME"},        {MenuAction::Fullscreen, "FULLSCREEN"},
    {MenuAction::NewCave, "NEW CAVE"},     {MenuAction::Ship, "SHIP PRESET"},
    {MenuAction::Zoom, "ZOOM"},
    {MenuAction::SwapEngines, "SWAP ENGINES"}, {MenuAction::Sound, "SOUND"},
#ifndef __EMSCRIPTEN__  // a web page cannot quit
    {MenuAction::Quit, "QUIT"},
#endif
};
inline constexpr int MENU_COUNT = static_cast<int>(sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]));

// Last device the player used; on-screen hints name its buttons.
enum class InputDevice { Keyboard, Gamepad };

struct UiState {
  bool menu_open = true;
  int cursor = 0;
  bool fullscreen = false;
  bool swap_engines = false;
  bool sound = true;
  double time = 0.0;  // real seconds, for UI animation
  InputDevice device = InputDevice::Keyboard;
  float zoom_toast = 0.f;  // seconds left to show the zoom level name
};
