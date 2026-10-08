// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <string>

// XDG config: $XDG_CONFIG_HOME/dualthrust/config (default ~/.config/dualthrust/config)
struct UserConfig {
  bool fullscreen = false;
  bool swap_engines = false;
  bool sound = true;
  int ship = 1;
  bool crt = true;
  int music = 7, sfx = 10;  // volumes 0..10
  int zoom = -1;  // index into ZOOM_LEVELS; -1 = pick by screen size
};

void set_config_dir_override(const std::string& dir);
std::string config_dir_path();
std::string config_file_path();
UserConfig load_config();
void save_config(const UserConfig& c);
