// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "config.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace {

std::string g_dir_override;

// mkdir -p
bool make_dirs(const std::string& dir) {
  for (size_t i = 1; i <= dir.size(); ++i) {
    if (i != dir.size() && dir[i] != '/') continue;
    std::string part = dir.substr(0, i);
    if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
  }
  return true;
}

}  // namespace

void set_config_dir_override(const std::string& dir) { g_dir_override = dir; }

namespace {

// $<var>/dualthrust when the XDG variable is set, else $HOME/<fallback>/dualthrust.
// No guessing: a set-but-relative variable, or no usable base at all, gives "".
std::string xdg_dir(const char* var, const char* fallback) {
  const char* v = std::getenv(var);
  if (v && v[0]) return v[0] == '/' ? std::string(v) + "/dualthrust" : std::string();
  const char* home = std::getenv("HOME");
  if (home && home[0] == '/') return std::string(home) + "/" + fallback + "/dualthrust";
  return {};
}

}  // namespace

std::string config_dir_path() {
  return g_dir_override.empty() ? xdg_dir("XDG_CONFIG_HOME", ".config") : g_dir_override;
}

std::string state_dir_path() { return xdg_dir("XDG_STATE_HOME", ".local/state"); }

std::string config_file_path() { return config_dir_path() + "/config"; }

bool check_user_dirs() {
  bool ok = true;
  if (config_dir_path().empty()) {
    std::fprintf(stderr, "dualthrust: no config directory: XDG_CONFIG_HOME must be an absolute path (or unset with an absolute HOME)\n");
    ok = false;
  }
  if (state_dir_path().empty()) {
    std::fprintf(stderr, "dualthrust: no state directory: XDG_STATE_HOME must be an absolute path (or unset with an absolute HOME)\n");
    ok = false;
  }
  return ok;
}

UserConfig load_config() {
  UserConfig c;
  std::FILE* f = std::fopen(config_file_path().c_str(), "r");
  if (!f) return c;
  char line[256];
  while (std::fgets(line, sizeof(line), f)) {
    char key[64];
    int val = 0;
    if (line[0] == '#' || std::sscanf(line, "%63[^=]=%d", key, &val) != 2) continue;
    if (!std::strcmp(key, "fullscreen")) c.fullscreen = val != 0;
    else if (!std::strcmp(key, "swap_engines")) c.swap_engines = val != 0;
    else if (!std::strcmp(key, "sound")) c.sound = val != 0;
    else if (!std::strcmp(key, "ship")) c.ship = val;
    else if (!std::strcmp(key, "zoom")) c.zoom = val;
    else if (!std::strcmp(key, "crt")) c.crt = val != 0;
    else if (!std::strcmp(key, "music")) c.music = val;
    else if (!std::strcmp(key, "sfx")) c.sfx = val;
  }
  std::fclose(f);
  return c;
}

void save_config(const UserConfig& c) {
  if (!make_dirs(config_dir_path())) {
    std::fprintf(stderr, "dualthrust: cannot create %s: %s\n", config_dir_path().c_str(), std::strerror(errno));
    return;
  }
  std::FILE* f = std::fopen(config_file_path().c_str(), "w");
  if (!f) {
    std::fprintf(stderr, "dualthrust: cannot write %s: %s\n", config_file_path().c_str(), std::strerror(errno));
    return;
  }
  std::fprintf(f, "# dualthrust config (XDG)\nfullscreen=%d\nswap_engines=%d\nsound=%d\nship=%d\nzoom=%d\ncrt=%d\nmusic=%d\nsfx=%d\n",
               c.fullscreen ? 1 : 0, c.swap_engines ? 1 : 0, c.sound ? 1 : 0, c.ship, c.zoom, c.crt ? 1 : 0, c.music, c.sfx);
  std::fclose(f);
#ifdef __EMSCRIPTEN__
  // The config directory is an IndexedDB mount (see mk/wasm/shell.html): write it back to the browser.
  EM_ASM(FS.syncfs(false, function(err) { if (err) console.warn('dualthrust: saving settings failed', err); }));
#endif
}
