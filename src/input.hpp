// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Rebindable play actions. Menu navigation stays fixed so the Controls page is always reachable.

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "math.hpp"

enum class Action : int {
  ThrustL = 0,
  ThrustR,
  ThrustLS,
  ThrustRS,
  Pause,
  Legs,
  WinchOut,
  WinchIn,
  Grip,
  ZoomCloser,
  ZoomFarther,
  NextShip,
  SwapEngines,
  Sonar,
  MapView,
  NextPad,
  PrevPad,
  Respawn,
  Count
};

inline constexpr int ACTION_COUNT = static_cast<int>(Action::Count);
inline constexpr int BIND_SLOTS = 3;  // primary + alternates per device family

enum class SrcKind : uint8_t { None = 0, Key = 1, Button = 2, Axis = 3 };

// One physical input. Axis uses `sign`: +1 = positive half (right/down/trigger), -1 = negative (left/up).
struct InputSrc {
  SrcKind kind = SrcKind::None;
  int16_t code = 0;  // SDL_Scancode, SDL_GameControllerButton, or SDL_GameControllerAxis
  int8_t sign = 1;

  bool empty() const { return kind == SrcKind::None; }
  bool operator==(const InputSrc& o) const {
    return kind == o.kind && code == o.code && (kind != SrcKind::Axis || sign == o.sign);
  }
};

struct BindMap {
  InputSrc kbd[ACTION_COUNT][BIND_SLOTS] = {};
  InputSrc pad[ACTION_COUNT][BIND_SLOTS] = {};
};

struct ActionInfo {
  Action action;
  const char* label;       // Controls list
  const char* short_name;  // HUD hints
  bool analog;             // thrust channels accept axis values 0..1
};

inline constexpr ActionInfo ACTION_INFO[] = {
    {Action::ThrustL, "THRUST L", "L", true},
    {Action::ThrustR, "THRUST R", "R", true},
    {Action::ThrustLS, "THRUST LS", "LS", true},
    {Action::ThrustRS, "THRUST RS", "RS", true},
    {Action::Pause, "PAUSE", "PAUSE", false},
    {Action::Legs, "LEGS", "LEGS", false},
    {Action::WinchOut, "WINCH OUT", "WINCH+", false},
    {Action::WinchIn, "WINCH IN", "WINCH-", false},
    {Action::Grip, "HOOK", "HOOK", false},
    {Action::ZoomCloser, "ZOOM IN", "ZOOM+", false},
    {Action::ZoomFarther, "ZOOM OUT", "ZOOM-", false},
    {Action::NextShip, "NEXT SHIP", "SHIP", false},
    {Action::SwapEngines, "SWAP ENGINES", "SWAP", false},
    {Action::Sonar, "SONAR", "SONAR", false},
    {Action::MapView, "MAP", "MAP", false},
    {Action::NextPad, "NEXT PAD", "PAD+", false},
    {Action::PrevPad, "PREV PAD", "PAD-", false},
    {Action::Respawn, "RESPAWN", "RESPAWN", false},
};
static_assert(sizeof(ACTION_INFO) / sizeof(ACTION_INFO[0]) == ACTION_COUNT, "ACTION_INFO size");

inline InputSrc key_src(SDL_Scancode sc) { return {SrcKind::Key, static_cast<int16_t>(sc), 1}; }
inline InputSrc btn_src(SDL_GameControllerButton b) {
  return {SrcKind::Button, static_cast<int16_t>(b), 1};
}
inline InputSrc axis_src(SDL_GameControllerAxis a, int sign) {
  return {SrcKind::Axis, static_cast<int16_t>(a), static_cast<int8_t>(sign < 0 ? -1 : 1)};
}

inline void clear_action(BindMap& m, Action a) {
  const int i = static_cast<int>(a);
  for (int s = 0; s < BIND_SLOTS; ++s) {
    m.kbd[i][s] = {};
    m.pad[i][s] = {};
  }
}

inline void set_default_binds(BindMap& m) {
  for (int a = 0; a < ACTION_COUNT; ++a) clear_action(m, static_cast<Action>(a));

  auto K = [&](Action a, int slot, SDL_Scancode sc) { m.kbd[static_cast<int>(a)][slot] = key_src(sc); };
  auto B = [&](Action a, int slot, SDL_GameControllerButton b) {
    m.pad[static_cast<int>(a)][slot] = btn_src(b);
  };
  auto A = [&](Action a, int slot, SDL_GameControllerAxis ax, int sign) {
    m.pad[static_cast<int>(a)][slot] = axis_src(ax, sign);
  };

  // Keyboard — matches the historical hard-coded layout
  K(Action::ThrustL, 0, SDL_SCANCODE_LCTRL);
  K(Action::ThrustL, 1, SDL_SCANCODE_A);
  K(Action::ThrustL, 2, SDL_SCANCODE_LEFT);
  K(Action::ThrustR, 0, SDL_SCANCODE_RCTRL);
  K(Action::ThrustR, 1, SDL_SCANCODE_D);
  K(Action::ThrustR, 2, SDL_SCANCODE_RIGHT);
  K(Action::ThrustLS, 0, SDL_SCANCODE_W);
  K(Action::ThrustLS, 1, SDL_SCANCODE_UP);
  K(Action::ThrustRS, 0, SDL_SCANCODE_DOWN);
  K(Action::ThrustRS, 1, SDL_SCANCODE_RALT);
  K(Action::Pause, 0, SDL_SCANCODE_ESCAPE);
  K(Action::Legs, 0, SDL_SCANCODE_SPACE);
  K(Action::WinchOut, 0, SDL_SCANCODE_Q);
  K(Action::WinchIn, 0, SDL_SCANCODE_E);
  K(Action::Grip, 0, SDL_SCANCODE_R);
  K(Action::ZoomCloser, 0, SDL_SCANCODE_EQUALS);  // optional; Tab still cycles via ZoomFarther path in UI
  K(Action::ZoomFarther, 0, SDL_SCANCODE_TAB);
  K(Action::NextShip, 0, SDL_SCANCODE_S);
  K(Action::SwapEngines, 0, SDL_SCANCODE_X);
  K(Action::Sonar, 0, SDL_SCANCODE_C);
  K(Action::MapView, 0, SDL_SCANCODE_Z);
  K(Action::NextPad, 0, SDL_SCANCODE_RIGHTBRACKET);  // ]
  K(Action::PrevPad, 0, SDL_SCANCODE_LEFTBRACKET);   // [
  K(Action::Respawn, 0, SDL_SCANCODE_RETURN);
  // Half-thrust on Shift is special-cased in read_thrust (not a separate action)

  // Gamepad
  A(Action::ThrustL, 0, SDL_CONTROLLER_AXIS_TRIGGERLEFT, +1);
  A(Action::ThrustR, 0, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, +1);
  // Shoulders reserved for pad teleport (see NextPad / PrevPad)
  A(Action::ThrustLS, 0, SDL_CONTROLLER_AXIS_LEFTY, -1);   // stick up
  A(Action::ThrustRS, 0, SDL_CONTROLLER_AXIS_RIGHTY, -1);
  B(Action::Pause, 0, SDL_CONTROLLER_BUTTON_START);
  B(Action::Legs, 0, SDL_CONTROLLER_BUTTON_X);
  B(Action::WinchOut, 0, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
  B(Action::WinchIn, 0, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
  B(Action::Grip, 0, SDL_CONTROLLER_BUTTON_A);
  B(Action::ZoomCloser, 0, SDL_CONTROLLER_BUTTON_DPAD_UP);
  B(Action::ZoomFarther, 0, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
  B(Action::NextShip, 0, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
  B(Action::Sonar, 0, SDL_CONTROLLER_BUTTON_LEFTSTICK);
  B(Action::MapView, 0, SDL_CONTROLLER_BUTTON_BACK);  // Select: hold for the full revealed map
  B(Action::NextPad, 0, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);  // triggers still drive thrust
  B(Action::PrevPad, 0, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
  B(Action::Respawn, 0, SDL_CONTROLLER_BUTTON_B);
  B(Action::Respawn, 1, SDL_CONTROLLER_BUTTON_A);  // A also respawns when crashed (handled in play logic)
}

// Clear every slot on either device that matches `src`, then write `src` into the first free
// slot of `action` for that device. Returns false if the source was empty.
inline bool assign_bind(BindMap& m, Action action, InputSrc src, bool keyboard) {
  if (src.empty()) return false;
  const int ai = static_cast<int>(action);
  auto& table = keyboard ? m.kbd : m.pad;
  for (int a = 0; a < ACTION_COUNT; ++a)
    for (int s = 0; s < BIND_SLOTS; ++s)
      if (table[a][s] == src) table[a][s] = {};
  // Prefer empty slot; else overwrite slot 0
  int dest = 0;
  for (int s = 0; s < BIND_SLOTS; ++s)
    if (table[ai][s].empty()) {
      dest = s;
      break;
    }
  table[ai][dest] = src;
  return true;
}

inline float src_value(const InputSrc& src, SDL_GameController* pad, const Uint8* keys) {
  if (src.empty()) return 0.f;
  switch (src.kind) {
    case SrcKind::Key:
      return keys && keys[src.code] ? 1.f : 0.f;
    case SrcKind::Button:
      return pad && SDL_GameControllerGetButton(pad, static_cast<SDL_GameControllerButton>(src.code)) ? 1.f
                                                                                                       : 0.f;
    case SrcKind::Axis: {
      if (!pad) return 0.f;
      const int raw = SDL_GameControllerGetAxis(pad, static_cast<SDL_GameControllerAxis>(src.code));
      // Triggers are 0..+32767; sticks are -32768..+32767
      const bool trigger = src.code == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                           src.code == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
      if (trigger) {
        if (src.sign < 0) return 0.f;
        return clampf(raw / 32767.f, 0.f, 1.f);
      }
      constexpr int DEAD = 8000;
      if (src.sign < 0) {
        // Negative half (stick up / left)
        if (raw >= -DEAD) return 0.f;
        return clampf((-raw - DEAD) / static_cast<float>(32768 - DEAD), 0.f, 1.f);
      }
      if (raw <= DEAD) return 0.f;
      return clampf((raw - DEAD) / static_cast<float>(32767 - DEAD), 0.f, 1.f);
    }
    default:
      return 0.f;
  }
}

inline float action_value(const BindMap& m, Action a, SDL_GameController* pad, const Uint8* keys) {
  const int i = static_cast<int>(a);
  float v = 0.f;
  for (int s = 0; s < BIND_SLOTS; ++s) {
    v = std::max(v, src_value(m.kbd[i][s], pad, keys));
    v = std::max(v, src_value(m.pad[i][s], pad, keys));
  }
  return v;
}

// Half thrust on Shift (historical): applied on top of key binds for L/R only.
inline void read_thrust_bound(const BindMap& m, SDL_GameController* pad, int channels, float out[4]) {
  const Uint8* keys = SDL_GetKeyboardState(nullptr);
  out[0] = action_value(m, Action::ThrustL, pad, keys);
  out[1] = action_value(m, Action::ThrustR, pad, keys);
  out[2] = action_value(m, Action::ThrustLS, pad, keys);
  out[3] = action_value(m, Action::ThrustRS, pad, keys);
  // Shift = half thrust on the classic L/R keys when no full bind is active from Shift itself
  if (keys) {
    if (keys[SDL_SCANCODE_LSHIFT]) out[0] = std::max(out[0], 0.5f);
    if (keys[SDL_SCANCODE_RSHIFT]) out[1] = std::max(out[1], 0.5f);
  }
  if (channels < 4) {
    out[0] = std::max(out[0], out[2]);
    out[1] = std::max(out[1], out[3]);
    out[2] = out[3] = 0.f;
  }
}

inline bool src_matches_key(const InputSrc& src, SDL_Scancode sc) {
  return src.kind == SrcKind::Key && src.code == static_cast<int16_t>(sc);
}
inline bool src_matches_button(const InputSrc& src, Uint8 button) {
  return src.kind == SrcKind::Button && src.code == static_cast<int16_t>(button);
}

inline bool action_pressed_key(const BindMap& m, Action a, SDL_Scancode sc) {
  const int i = static_cast<int>(a);
  for (int s = 0; s < BIND_SLOTS; ++s)
    if (src_matches_key(m.kbd[i][s], sc)) return true;
  return false;
}
inline bool action_pressed_button(const BindMap& m, Action a, Uint8 button) {
  const int i = static_cast<int>(a);
  for (int s = 0; s < BIND_SLOTS; ++s)
    if (src_matches_button(m.pad[i][s], button)) return true;
  return false;
}

// Human-readable name for a single source (for the Controls list and HUD).
inline const char* src_name(const InputSrc& src, char* buf, size_t n) {
  if (src.empty()) {
    std::snprintf(buf, n, "-");
    return buf;
  }
  switch (src.kind) {
    case SrcKind::Key: {
      const char* nm = SDL_GetScancodeName(static_cast<SDL_Scancode>(src.code));
      if (!nm || !nm[0]) nm = "?";
      std::snprintf(buf, n, "%s", nm);
      // Uppercase for the pixel font
      for (char* p = buf; *p; ++p)
        if (*p >= 'a' && *p <= 'z') *p = static_cast<char>(*p - 32);
      return buf;
    }
    case SrcKind::Button: {
      const char* nm = SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(src.code));
      if (!nm) nm = "btn";
      std::snprintf(buf, n, "%s", nm);
      for (char* p = buf; *p; ++p)
        if (*p >= 'a' && *p <= 'z') *p = static_cast<char>(*p - 32);
      return buf;
    }
    case SrcKind::Axis: {
      const char* nm = SDL_GameControllerGetStringForAxis(static_cast<SDL_GameControllerAxis>(src.code));
      if (!nm) nm = "axis";
      std::snprintf(buf, n, "%s%s", nm, src.sign < 0 ? "-" : "+");
      for (char* p = buf; *p; ++p)
        if (*p >= 'a' && *p <= 'z') *p = static_cast<char>(*p - 32);
      return buf;
    }
    default:
      std::snprintf(buf, n, "-");
      return buf;
  }
}

// Compact summary of the primary bind for a device family.
inline const char* action_bind_label(const BindMap& m, Action a, bool keyboard, char* buf, size_t n) {
  const int i = static_cast<int>(a);
  const auto& slots = keyboard ? m.kbd[i] : m.pad[i];
  char part[3][24];
  int n_parts = 0;
  for (int s = 0; s < BIND_SLOTS && n_parts < 3; ++s) {
    if (slots[s].empty()) continue;
    src_name(slots[s], part[n_parts], sizeof part[n_parts]);
    ++n_parts;
  }
  if (n_parts == 0) {
    std::snprintf(buf, n, "-");
    return buf;
  }
  if (n_parts == 1) {
    std::snprintf(buf, n, "%s", part[0]);
    return buf;
  }
  if (n_parts == 2) {
    std::snprintf(buf, n, "%s/%s", part[0], part[1]);
    return buf;
  }
  std::snprintf(buf, n, "%s/%s/%s", part[0], part[1], part[2]);
  return buf;
}

// Encode/decode for the config file: "key:40", "btn:0", "axis+:2", "axis-:1", "none"
inline void src_encode(const InputSrc& src, char* buf, size_t n) {
  switch (src.kind) {
    case SrcKind::Key:
      std::snprintf(buf, n, "key:%d", static_cast<int>(src.code));
      break;
    case SrcKind::Button:
      std::snprintf(buf, n, "btn:%d", static_cast<int>(src.code));
      break;
    case SrcKind::Axis:
      std::snprintf(buf, n, "axis%c:%d", src.sign < 0 ? '-' : '+', static_cast<int>(src.code));
      break;
    default:
      std::snprintf(buf, n, "none");
      break;
  }
}

inline InputSrc src_decode(const char* s) {
  InputSrc out;
  if (!s || !s[0] || std::strcmp(s, "none") == 0) return out;
  int code = 0;
  if (std::sscanf(s, "key:%d", &code) == 1) {
    out.kind = SrcKind::Key;
    out.code = static_cast<int16_t>(code);
    return out;
  }
  if (std::sscanf(s, "btn:%d", &code) == 1) {
    out.kind = SrcKind::Button;
    out.code = static_cast<int16_t>(code);
    return out;
  }
  char sign = '+';
  if (std::sscanf(s, "axis%c:%d", &sign, &code) == 2) {
    out.kind = SrcKind::Axis;
    out.code = static_cast<int16_t>(code);
    out.sign = (sign == '-') ? -1 : 1;
    return out;
  }
  return out;
}

