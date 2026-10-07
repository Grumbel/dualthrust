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
};

void set_config_dir_override(const std::string& dir);
std::string config_dir_path();
std::string config_file_path();
UserConfig load_config();
void save_config(const UserConfig& c);
