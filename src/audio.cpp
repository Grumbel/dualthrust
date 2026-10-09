// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "audio.hpp"

#include <cstdio>
#include <vector>

namespace {

constexpr float TAU = 2.f * PI;

// --- Music data -------------------------------------------------------------
// Slow Am - F - C - G loop; pad chord tones in semitones from the root.
struct Chord {
  float root_hz;
  int tones[3];
};
constexpr Chord CHORDS[] = {
    {110.00f, {0, 7, 15}},  // A  (A E C')
    {87.31f, {0, 7, 16}},   // F  (F C A')
    {130.81f, {0, 7, 16}},  // C  (C G E')
    {98.00f, {0, 7, 14}},   // G  (G D B')
};
constexpr int CHORD_COUNT = 4;
constexpr double BPM = 66.0;
constexpr int STEPS_PER_BEAT = 2;
constexpr int STEPS_PER_CHORD = 8 * STEPS_PER_BEAT;  // two bars of 4/4
// A minor pentatonic around A4 (semitones)
constexpr int ARP_SCALE[] = {0, 3, 5, 7, 10, 12, 15, 17};
constexpr int ARP_SCALE_N = sizeof(ARP_SCALE) / sizeof(ARP_SCALE[0]);

float semitone(float base, int n) { return base * std::pow(2.f, n / 12.f); }
float soft_clip(float x) { return std::tanh(x); }

}  // namespace

bool Audio::init() {
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    std::fprintf(stderr, "audio disabled: %s\n", SDL_GetError());
    return false;
  }
  SDL_AudioSpec want{}, have{};
  want.freq = 44100;
  want.format = AUDIO_F32SYS;
  want.channels = 2;
  want.samples = 2048;  // handheld CPUs: favour fewer, larger callbacks
  want.callback = &Audio::callback;
  want.userdata = this;
  dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);  // exact format only
  if (!dev_) {
    std::fprintf(stderr, "audio disabled: %s\n", SDL_GetError());
    return false;
  }
  rate_ = have.freq;
  delay_l_.assign(DELAY_LEN, 0.f);
  delay_r_.assign(DELAY_LEN, 0.f);
  SDL_PauseAudioDevice(dev_, 0);
  std::printf("  audio:      %d Hz\n", rate_);
  return true;
}

void Audio::shutdown() {
  if (dev_) SDL_CloseAudioDevice(dev_);
  dev_ = 0;
}

void Audio::set_enabled(bool on) {
  if (!dev_) return;
  SDL_LockAudioDevice(dev_);
  master_target_ = on ? 1.f : 0.f;
  SDL_UnlockAudioDevice(dev_);
}

void Audio::set_volumes(float music, float sfx) {
  if (!dev_) return;
  SDL_LockAudioDevice(dev_);
  music_target_ = music * music * 1.4f;  // squared: slider steps feel even
  sfx_target_ = sfx * sfx;
  SDL_UnlockAudioDevice(dev_);
}

void Audio::set_engines(float left, float right, float left_dmg, float right_dmg) {
  if (!dev_) return;
  SDL_LockAudioDevice(dev_);
  target_[0] = left;
  target_[1] = right;
  dmg_target_[0] = left_dmg < 0.f ? 0.f : (left_dmg > 1.f ? 1.f : left_dmg);
  dmg_target_[1] = right_dmg < 0.f ? 0.f : (right_dmg > 1.f ? 1.f : right_dmg);
  SDL_UnlockAudioDevice(dev_);
}

void Audio::trigger(SimEventKind kind, float strength) {
  if (!dev_) return;
  SDL_LockAudioDevice(dev_);
  for (Voice& v : voices_)
    if (!v.active) {
      v = {kind, 0.f, clampf(strength / 200.f, 0.25f, 1.f), true};
      break;
    }
  SDL_UnlockAudioDevice(dev_);
}

void Audio::callback(void* user, Uint8* stream, int len) {
  static_cast<Audio*>(user)->render(reinterpret_cast<float*>(stream), len / static_cast<int>(sizeof(float) * 2));
}

float Audio::noise() {
  rng_ = rng_ * 1664525u + 1013904223u;
  return (static_cast<int32_t>(rng_) >> 8) / static_cast<float>(1 << 23);  // [-1,1)
}

void Audio::render(float* out, int frames) {
  const float dt = 1.f / rate_;
  const double step_len = 60.0 / BPM / STEPS_PER_BEAT;

  for (int i = 0; i < frames; ++i) {
    master_ += (master_target_ - master_) * 0.0008f;  // click-free mute fades
    music_gain_ += (music_target_ - music_gain_) * 0.0008f;
    sfx_gain_ += (sfx_target_ - sfx_gain_) * 0.0008f;
    float l = 0.f, r = 0.f;

    // --- Engines: filtered noise + low rumble, panned per side ---
    // Damage adds crackle, amplitude flutter and a dirtier (brighter) noise bed.
    for (int e = 0; e < 2; ++e) {
      level_[e] += (target_[e] - level_[e]) * 0.0015f;
      dmg_[e] += (dmg_target_[e] - dmg_[e]) * 0.0008f;
      const float lv = level_[e];
      const float dg = dmg_[e];
      if (lv < 0.002f) continue;
      // Brighter noise floor when damaged (metal scraping / open exhaust)
      float cutoff = 0.03f + 0.22f * lv + 0.18f * dg;
      lp_[e] += (noise() - lp_[e]) * cutoff;
      // Rumble rate wobbles with damage
      float rumble_hz = 48.f + 38.f * lv + (e ? 1.5f : 0.f) + 12.f * dg * noise();
      rumble_phase_[e] += rumble_hz * dt;
      rumble_phase_[e] -= std::floor(rumble_phase_[e]);
      float rumble = std::sin(rumble_phase_[e] * TAU) + 0.4f * std::sin(rumble_phase_[e] * TAU * 2.f);
      // Amplitude flutter: healthy engines are steady; damaged ones pulse irregularly
      float flutter = 1.f;
      if (dg > 0.05f) {
        crackle_phase_[e] += (9.f + 28.f * dg) * dt;
        crackle_phase_[e] -= std::floor(crackle_phase_[e]);
        float wave = 0.5f + 0.5f * std::sin(crackle_phase_[e] * TAU);
        // Random hard cuts (misfire pops) when heavily damaged
        if (dg > 0.25f && (noise() * 0.5f + 0.5f) < 0.012f + 0.04f * dg)
          wave *= 0.05f + 0.2f * (noise() * 0.5f + 0.5f);
        flutter = (1.f - 0.55f * dg) + 0.55f * dg * wave;
      }
      // Crackle: sparse high-amplitude noise bursts
      float crackle = 0.f;
      if (dg > 0.08f) {
        float burst = noise();
        if (burst > 0.92f - 0.25f * dg)
          crackle = burst * burst * (0.35f + 0.65f * dg);
      }
      float s = (lp_[e] * (1.6f + 0.9f * dg) + rumble * 0.35f * (1.f - 0.4f * dg) + crackle) *
                lv * (0.35f + 0.65f * lv) * 0.55f * flutter;
      l += s * (e == 0 ? 0.85f : 0.35f);
      r += s * (e == 0 ? 0.35f : 0.85f);
    }

    // --- Effects ---
    for (Voice& v : voices_) {
      if (!v.active) continue;
      const float t = v.t;
      float s = 0.f, life = 1.f;
      switch (v.kind) {
        case SimEventKind::Landed: {  // two-note chime, E5 then A5
          life = 1.6f;
          float env1 = std::exp(-t * 4.f), env2 = t > 0.14f ? std::exp(-(t - 0.14f) * 3.5f) : 0.f;
          s = (std::sin(t * TAU * 659.25f) * env1 + std::sin(t * TAU * 880.f) * env2 +
               0.25f * std::sin(t * TAU * 1318.5f) * env1) * 0.22f;
          break;
        }
        case SimEventKind::Bounce: {  // dull thud
          life = 0.35f;
          float f = 70.f * std::exp(-t * 8.f) + 38.f;
          s = (std::sin(t * TAU * f) * 0.7f + noise() * 0.25f * std::exp(-t * 40.f)) * std::exp(-t * 11.f) * 0.5f;
          break;
        }
        // --- Pilot feedback: short beeps ---
        case SimEventKind::HookOut:  // winch out: two rising beeps
        case SimEventKind::HookIn: {  // winch in: two falling beeps
          life = 0.3f;
          const bool out = v.kind == SimEventKind::HookOut;
          const float f1 = out ? 660.f : 990.f, f2 = out ? 990.f : 660.f;
          s = (t < 0.09f ? std::sin(t * TAU * f1) * (1.f - t / 0.09f)
                         : (t > 0.12f && t < 0.24f ? std::sin(t * TAU * f2) * (1.f - (t - 0.12f) / 0.12f) : 0.f)) * 0.16f;
          break;
        }
        case SimEventKind::Grab: {  // latch clunk, then a confirming chirp
          life = 0.4f;
          s = std::sin(t * TAU * (90.f * std::exp(-t * 20.f) + 55.f)) * std::exp(-t * 28.f) * 0.55f +
              (t > 0.07f ? std::sin(t * TAU * 1320.f) * std::exp(-(t - 0.07f) * 22.f) * 0.2f : 0.f);
          break;
        }
        case SimEventKind::Release: {  // falling blip
          life = 0.25f;
          s = std::sin(t * TAU * (900.f * std::exp(-t * 9.f) + 300.f)) * std::exp(-t * 14.f) * 0.2f;
          break;
        }
        case SimEventKind::NoTarget: {  // low double buzz
          life = 0.3f;
          const float gate = (t < 0.1f || (t > 0.15f && t < 0.25f)) ? 1.f : 0.f;
          const float ph = std::fmod(t * 140.f, 1.f);
          s = (ph < 0.5f ? 0.14f : -0.14f) * gate;
          break;
        }
        case SimEventKind::Delivered: {  // cheerful rising arpeggio C6 E6 G6 C7
          life = 1.2f;
          static constexpr float notes[4] = {1046.5f, 1318.5f, 1568.f, 2093.f};
          for (int n = 0; n < 4; ++n) {
            const float tn = t - n * 0.09f;
            if (tn > 0.f) s += std::sin(tn * TAU * notes[n]) * std::exp(-tn * (n == 3 ? 4.f : 9.f)) * 0.14f;
          }
          break;
        }
        case SimEventKind::LegsOut:  // servo whirr: a short sweep down / up with a little grit
        case SimEventKind::LegsIn: {
          life = 0.3f;
          const bool out = v.kind == SimEventKind::LegsOut;
          const float f = out ? 330.f - 400.f * t : 200.f + 400.f * t;
          const float env = std::min(t / 0.02f, 1.f) * std::max(0.f, 1.f - t / 0.28f);
          s = (std::sin(t * TAU * f) * 0.12f + noise() * 0.03f) * env;
          break;
        }
        case SimEventKind::Crashed: {  // rumbling explosion
          life = 2.6f;
          float f = 55.f * std::exp(-t * 1.4f) + 22.f;
          float n = noise();
          s = (std::sin(t * TAU * f) * 0.8f + n * 0.7f * std::exp(-t * 7.f) +
               n * 0.3f * std::exp(-t * 1.8f) * (0.5f + 0.5f * std::sin(t * 31.f))) *
              std::exp(-t * 1.5f) * 0.85f;
          break;
        }
        case SimEventKind::SonarPing: {  // falling chirp + short static burst (active scan)
          life = 0.85f;
          const float f = 1400.f * std::exp(-t * 3.2f) + 180.f;
          const float env = std::min(t / 0.02f, 1.f) * std::exp(-t * 2.4f);
          s = (std::sin(t * TAU * f) * 0.35f + noise() * 0.12f * std::exp(-t * 8.f)) * env;
          // faint echo of the same sweep
          if (t > 0.18f) {
            const float te = t - 0.18f;
            const float fe = 1100.f * std::exp(-te * 3.5f) + 160.f;
            s += std::sin(te * TAU * fe) * std::exp(-te * 3.f) * 0.12f;
          }
          break;
        }
      }
      s *= v.amp;
      l += s;
      r += s;
      v.t += dt;
      if (v.t > life) v.active = false;
    }

    l *= sfx_gain_;  // engines and effects, before the music joins
    r *= sfx_gain_;

    // --- Music: pad + sparse echoing arpeggio ---
    double step_pos = music_t_ / step_len;
    int step = static_cast<int>(step_pos);
    music_t_ += dt;
    if (step != last_step_) {
      last_step_ = step;
      const Chord& ch = CHORDS[(step / STEPS_PER_CHORD) % CHORD_COUNT];
      // Trigger an arp note on most even steps, pseudo-randomly
      if (step % 2 == 0 && (noise() * 0.5f + 0.5f) < 0.7f) {
        int idx = static_cast<int>((noise() * 0.5f + 0.5f) * ARP_SCALE_N) % ARP_SCALE_N;
        float base = ch.root_hz * 4.f;  // two octaves above the pad root
        arp_[arp_next_] = {semitone(base, ARP_SCALE[idx] - 12), 0.f, 1.f};
        arp_next_ = (arp_next_ + 1) % 6;
      }
    }
    const Chord& ch = CHORDS[(step / STEPS_PER_CHORD) % CHORD_COUNT];
    const float within = static_cast<float>(std::fmod(step_pos, STEPS_PER_CHORD)) / STEPS_PER_CHORD;
    const float swell = std::sin(within * PI);                  // each chord fades in and out
    float pad = 0.f;
    for (int n = 0; n < 3; ++n) {
      float f = semitone(ch.root_hz, ch.tones[n]);
      for (int d = 0; d < 3; ++d) {  // three detuned voices per tone
        float& ph = pad_phase_[n][d];
        ph += f * (1.f + (d - 1) * 0.004f) * dt;
        ph -= std::floor(ph);
        pad += std::sin(ph * TAU) + 0.3f * std::sin(ph * TAU * 2.f);
      }
    }
    pad *= 0.022f * (0.35f + 0.65f * swell);
    float arp = 0.f;
    for (Note& nt : arp_) {
      if (nt.env < 0.0005f) continue;
      nt.phase += nt.freq * dt;
      nt.phase -= std::floor(nt.phase);
      arp += (std::sin(nt.phase * TAU) + 0.2f * std::sin(nt.phase * TAU * 3.f)) * nt.env;
      nt.env *= 0.99993f;
    }
    arp *= 0.035f;
    float ml = pad + arp * 0.8f, mr = pad + arp * 1.0f;
    // Echo (ping-pong) on the music bus only
    float dl = delay_l_[delay_pos_], dr = delay_r_[delay_pos_];
    delay_l_[delay_pos_] = ml + dr * 0.45f;
    delay_r_[delay_pos_] = mr + dl * 0.45f;
    delay_pos_ = (delay_pos_ + 1) % DELAY_LEN;
    ml += dl * 0.5f;
    mr += dr * 0.5f;
    music_lp_[0] += (ml - music_lp_[0]) * 0.35f;  // keep it soft
    music_lp_[1] += (mr - music_lp_[1]) * 0.35f;
    l += music_lp_[0] * music_gain_;
    r += music_lp_[1] * music_gain_;

    out[i * 2] = soft_clip(l * master_);
    out[i * 2 + 1] = soft_clip(r * master_);
  }
}
