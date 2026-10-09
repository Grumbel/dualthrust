// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <SDL.h>

#include <vector>

#include "game.hpp"

// Fully synthesised audio (no asset files): engine rumble, event effects and generative music,
// mixed in the SDL audio callback. The game thread only sets a few parameters under the device
// lock. If no audio device is available everything silently becomes a no-op.
class Audio {
 public:
  bool init();
  void shutdown();
  bool available() const { return dev_ != 0; }

  void set_enabled(bool on);
  void set_volumes(float music, float sfx);  // 0..1 each: music loop vs engines + effects
  // left/right thrust 0..1; dmg 0..1 (engine health loss on that side — crackle / sputter timbre)
  void set_engines(float left, float right, float left_dmg = 0.f, float right_dmg = 0.f);
  void trigger(SimEventKind kind, float strength);

 private:
  struct Voice {
    SimEventKind kind;
    float t = 0.f, amp = 0.f;
    bool active = false;
  };
  static constexpr int MAX_VOICES = 8;
  static constexpr int DELAY_LEN = 22050;  // 0.5 s echo

  static void callback(void* user, Uint8* stream, int len);
  void render(float* out, int frames);
  float noise();

  SDL_AudioDeviceID dev_ = 0;
  int rate_ = 44100;

  // Shared parameters (written under the device lock)
  float target_[2] = {0.f, 0.f};
  float dmg_target_[2] = {0.f, 0.f};
  float master_target_ = 1.f;
  float music_target_ = 0.5f, sfx_target_ = 1.f;
  Voice voices_[MAX_VOICES];

  // Mixer state (audio thread only)
  float level_[2] = {0.f, 0.f};
  float dmg_[2] = {0.f, 0.f};
  float lp_[2] = {0.f, 0.f};
  float rumble_phase_[2] = {0.f, 0.f};
  float crackle_phase_[2] = {0.f, 0.f};  // free-running phase for damage crackle bursts
  float master_ = 0.f, music_gain_ = 0.f, sfx_gain_ = 0.f;
  uint32_t rng_ = 12345;

  // Music
  double music_t_ = 0.0;
  int last_step_ = -1;
  float pad_phase_[3][3] = {};
  struct Note { float freq, phase, env; };
  Note arp_[6] = {};
  int arp_next_ = 0;
  std::vector<float> delay_l_, delay_r_;
  int delay_pos_ = 0;
  float music_lp_[2] = {0.f, 0.f};
};
