// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "save.hpp"

#include "config.hpp"
#include "physics.hpp"
#include "systems.hpp"

#include <box2d/box2d.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int WORLD_VERSION = 1;

std::string path_world() { return state_dir_path() + "/world"; }
std::string path_fog() { return state_dir_path() + "/world.fog"; }

bool write_fog(const std::vector<uint8_t>& revealed) {
  const size_t n = static_cast<size_t>(Cave::GW) * Cave::GH;
  if (revealed.size() != n) return false;
  if (!make_dirs(state_dir_path())) return false;
  const std::string tmp = path_fog() + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(revealed.data(), 1, n, f) == n && std::fclose(f) == 0 &&
                  std::rename(tmp.c_str(), path_fog().c_str()) == 0;
  if (!ok) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

bool read_fog(std::vector<uint8_t>& revealed) {
  const size_t n = static_cast<size_t>(Cave::GW) * Cave::GH;
  std::FILE* f = std::fopen(path_fog().c_str(), "rb");
  if (!f) return false;
  revealed.assign(n, 0);
  const size_t got = std::fread(revealed.data(), 1, n, f);
  std::fclose(f);
  return got == n;
}

}  // namespace

std::string world_file_path() { return path_world(); }
std::string world_fog_path() { return path_fog(); }

bool world_save_exists() {
  std::FILE* f = std::fopen(path_world().c_str(), "r");
  if (!f) return false;
  std::fclose(f);
  return true;
}

void clear_world_save() {
  std::remove(path_world().c_str());
  std::remove(path_fog().c_str());
}

bool save_world(const Game& g, int ship_index) {
  if (state_dir_path().empty()) return false;
  if (!make_dirs(state_dir_path())) {
    std::fprintf(stderr, "dualthrust: cannot create %s: %s\n", state_dir_path().c_str(), std::strerror(errno));
    return false;
  }
  if (!write_fog(g.revealed)) {
    std::fprintf(stderr, "dualthrust: cannot write %s\n", path_fog().c_str());
    return false;
  }

  const Transform* tf = g.ship != NULL_ENTITY && g.ecs.has<Transform>(g.ship) ? &g.ecs.get<Transform>(g.ship) : nullptr;
  const Motion* mot = g.ship != NULL_ENTITY && g.ecs.has<Motion>(g.ship) ? &g.ecs.get<Motion>(g.ship) : nullptr;
  const Flight* fl = g.ship != NULL_ENTITY && g.ecs.has<Flight>(g.ship) ? &g.ecs.get<Flight>(g.ship) : nullptr;
  const Legs* legs = g.ship != NULL_ENTITY && g.ecs.has<Legs>(g.ship) ? &g.ecs.get<Legs>(g.ship) : nullptr;
  const Rope* rope = g.ship != NULL_ENTITY && g.ecs.has<Rope>(g.ship) ? &g.ecs.get<Rope>(g.ship) : nullptr;

  // Ordered cargo list for stable indices
  std::vector<Entity> cargo_ents;
  g.ecs.view<Cargo>([&](Entity e, const Cargo&) { cargo_ents.push_back(e); });

  int held_idx = -1;
  if (rope && rope->held != NULL_ENTITY) {
    for (int i = 0; i < static_cast<int>(cargo_ents.size()); ++i)
      if (cargo_ents[static_cast<size_t>(i)] == rope->held) { held_idx = i; break; }
  }

  const std::string tmp = path_world() + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f) {
    std::fprintf(stderr, "dualthrust: cannot write %s: %s\n", tmp.c_str(), std::strerror(errno));
    return false;
  }

  std::fprintf(f, "# dualthrust world v%d (XDG state)\n", WORLD_VERSION);
  std::fprintf(f, "version=%d\n", WORLD_VERSION);
  std::fprintf(f, "seed=%u\n", g.cave.seed);
  std::fprintf(f, "ship=%d\n", ship_index);
  std::fprintf(f, "score=%d\n", g.score);
  std::fprintf(f, "last_pad=%d\n", g.last_pad);
  std::fprintf(f, "time=%.4f\n", g.time);
  std::fprintf(f, "explore_tier=%d\n", g.explore_tier);
  std::fprintf(f, "cells_explored=%d\n", g.cells_explored);
  std::fprintf(f, "signals_cleared=%d\n", g.signals_cleared ? 1 : 0);
  std::fprintf(f, "pads_cleared=%d\n", g.pads_cleared ? 1 : 0);

  if (tf) {
    std::fprintf(f, "pos_x=%.4f\n", tf->pos.x);
    std::fprintf(f, "pos_y=%.4f\n", tf->pos.y);
    std::fprintf(f, "angle=%.6f\n", tf->angle);
  }
  if (mot) {
    std::fprintf(f, "vel_x=%.4f\n", mot->vel.x);
    std::fprintf(f, "vel_y=%.4f\n", mot->vel.y);
    std::fprintf(f, "ang_vel=%.6f\n", mot->ang_vel);
  }
  if (fl) {
    std::fprintf(f, "flight=%d\n", static_cast<int>(fl->state));
    std::fprintf(f, "fuel=%.4f\n", fl->fuel);
    std::fprintf(f, "hurt=%.4f\n", fl->hurt);
  }
  if (legs) std::fprintf(f, "legs=%d\n", legs->deployed ? 1 : 0);
  if (rope) {
    std::fprintf(f, "rope_len=%.4f\n", rope->length);
    std::fprintf(f, "rope_out=%d\n", rope->out ? 1 : 0);
    std::fprintf(f, "held=%d\n", held_idx);
  }
  if (g.ship != NULL_ENTITY && g.ecs.has<Thrusters>(g.ship) && g.ecs.has<Hull>(g.ship)) {
    const Thrusters& th = g.ecs.get<Thrusters>(g.ship);
    const int n_eng = std::min(thruster_count(*g.ecs.get<Hull>(g.ship).def), Thrusters::MAX);
    std::fprintf(f, "eng_n=%d\n", n_eng);
    for (int i = 0; i < n_eng; ++i)
      std::fprintf(f, "eng_%d=%.4f\n", i, th.damage[i]);
  }

  std::fprintf(f, "pad_n=%d\n", static_cast<int>(g.cave.pads.size()));
  for (int i = 0; i < static_cast<int>(g.cave.pads.size()); ++i) {
    const LandingPad& pad = g.cave.pads[static_cast<size_t>(i)];
    std::fprintf(f, "pad_%d=%d,%d\n", i, pad.active ? 1 : 0, pad.visited ? 1 : 0);
  }

  std::fprintf(f, "signal_n=%d\n", static_cast<int>(g.signals.size()));
  for (int i = 0; i < static_cast<int>(g.signals.size()); ++i) {
    const Game::Signal& s = g.signals[static_cast<size_t>(i)];
    std::fprintf(f, "sig_%d=%.4f,%.4f,%d\n", i, s.pos.x, s.pos.y, s.found ? 1 : 0);
  }

  std::fprintf(f, "cargo_n=%d\n", static_cast<int>(cargo_ents.size()));
  for (int i = 0; i < static_cast<int>(cargo_ents.size()); ++i) {
    Entity e = cargo_ents[static_cast<size_t>(i)];
    const Cargo& c = g.ecs.get<Cargo>(e);
    const Transform& ct = g.ecs.get<Transform>(e);
    int kind = 0;
    if (c.def) {
      for (int k = 0; k < CARGO_DEF_COUNT; ++k)
        if (&CARGO_DEFS[k] == c.def) { kind = k; break; }
    }
    const float ang = b2Rot_GetAngle(b2Body_GetRotation(c.body));
    std::fprintf(f, "cargo_%d=%d,%.4f,%.4f,%.6f,%d,%d,%d\n", i, kind, ct.pos.x, ct.pos.y, ang,
                 c.picked ? 1 : 0, c.dest_pad, b2Body_IsEnabled(c.body) ? 1 : 0);
  }

  const bool ok = std::fclose(f) == 0 && std::rename(tmp.c_str(), path_world().c_str()) == 0;
  if (!ok) {
    std::remove(tmp.c_str());
    std::fprintf(stderr, "dualthrust: cannot save %s: %s\n", path_world().c_str(), std::strerror(errno));
    return false;
  }
  return true;
}

bool load_world(Game& g, int* ship_index_out) {
  std::FILE* f = std::fopen(path_world().c_str(), "r");
  if (!f) return false;

  unsigned seed = 0;
  int ship = DEFAULT_SHIP;
  int score = 0, last_pad = -1, explore_tier = 0, cells_explored = 0;
  int signals_cleared = 0, pads_cleared = 0;
  float time = 0.f;
  float pos_x = 0.f, pos_y = 0.f, angle = 0.f;
  float vel_x = 0.f, vel_y = 0.f, ang_vel = 0.f;
  int flight = 0, legs = 1, rope_out = 0, held = -1;
  float fuel = 1.f, hurt = 0.f, rope_len = rope::MIN_LEN;
  bool have_pos = false;

  std::vector<int> pad_active;
  std::vector<int> pad_visited;
  std::vector<Game::Signal> signals;
  struct CargoRow {
    int kind = 0;
    float x = 0.f, y = 0.f, angle = 0.f;
    int picked = 0, dest = -1, alive = 1;
  };
  std::vector<CargoRow> cargos;

  char line[512];
  while (std::fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') continue;
    char key[64];
    char val[440];
    if (std::sscanf(line, "%63[^=]=%439[^\n]", key, val) != 2) continue;
    if (std::strcmp(key, "version") == 0) {
      int v = 0;
      std::sscanf(val, "%d", &v);
      if (v != WORLD_VERSION) { std::fclose(f); return false; }
    } else if (std::strcmp(key, "seed") == 0) std::sscanf(val, "%u", &seed);
    else if (std::strcmp(key, "ship") == 0) std::sscanf(val, "%d", &ship);
    else if (std::strcmp(key, "score") == 0) std::sscanf(val, "%d", &score);
    else if (std::strcmp(key, "last_pad") == 0) std::sscanf(val, "%d", &last_pad);
    else if (std::strcmp(key, "time") == 0) std::sscanf(val, "%f", &time);
    else if (std::strcmp(key, "explore_tier") == 0) std::sscanf(val, "%d", &explore_tier);
    else if (std::strcmp(key, "cells_explored") == 0) std::sscanf(val, "%d", &cells_explored);
    else if (std::strcmp(key, "signals_cleared") == 0) std::sscanf(val, "%d", &signals_cleared);
    else if (std::strcmp(key, "pads_cleared") == 0) std::sscanf(val, "%d", &pads_cleared);
    else if (std::strcmp(key, "pos_x") == 0) { std::sscanf(val, "%f", &pos_x); have_pos = true; }
    else if (std::strcmp(key, "pos_y") == 0) std::sscanf(val, "%f", &pos_y);
    else if (std::strcmp(key, "angle") == 0) std::sscanf(val, "%f", &angle);
    else if (std::strcmp(key, "vel_x") == 0) std::sscanf(val, "%f", &vel_x);
    else if (std::strcmp(key, "vel_y") == 0) std::sscanf(val, "%f", &vel_y);
    else if (std::strcmp(key, "ang_vel") == 0) std::sscanf(val, "%f", &ang_vel);
    else if (std::strcmp(key, "flight") == 0) std::sscanf(val, "%d", &flight);
    else if (std::strcmp(key, "fuel") == 0) std::sscanf(val, "%f", &fuel);
    else if (std::strcmp(key, "hurt") == 0) std::sscanf(val, "%f", &hurt);
    else if (std::strcmp(key, "legs") == 0) std::sscanf(val, "%d", &legs);
    else if (std::strcmp(key, "rope_len") == 0) std::sscanf(val, "%f", &rope_len);
    else if (std::strcmp(key, "rope_out") == 0) std::sscanf(val, "%d", &rope_out);
    else if (std::strcmp(key, "held") == 0) std::sscanf(val, "%d", &held);
    else if (std::strcmp(key, "pad_n") == 0) {
      int n = 0;
      std::sscanf(val, "%d", &n);
      pad_active.assign(std::max(0, n), 0);
      pad_visited.assign(std::max(0, n), 0);
    } else if (std::strncmp(key, "pad_", 4) == 0) {
      int i = 0, a = 0, v = 0;
      if (std::sscanf(key, "pad_%d", &i) == 1) {
        const int nread = std::sscanf(val, "%d,%d", &a, &v);
        if (nread >= 1 && i >= 0 && i < static_cast<int>(pad_active.size())) {
          pad_active[static_cast<size_t>(i)] = a;
          if (nread >= 2) pad_visited[static_cast<size_t>(i)] = v;
          else if (a) pad_visited[static_cast<size_t>(i)] = 1;  // legacy: active implied home visits
        }
      }
    } else if (std::strcmp(key, "signal_n") == 0) {
      int n = 0;
      std::sscanf(val, "%d", &n);
      signals.clear();
      signals.reserve(std::max(0, n));
    } else if (std::strncmp(key, "sig_", 4) == 0) {
      float x = 0.f, y = 0.f;
      int found = 0;
      if (std::sscanf(val, "%f,%f,%d", &x, &y, &found) == 3) signals.push_back({Vec2{x, y}, found != 0});
    } else if (std::strcmp(key, "cargo_n") == 0) {
      int n = 0;
      std::sscanf(val, "%d", &n);
      cargos.clear();
      cargos.reserve(std::max(0, n));
    } else if (std::strncmp(key, "cargo_", 6) == 0) {
      CargoRow row;
      if (std::sscanf(val, "%d,%f,%f,%f,%d,%d,%d", &row.kind, &row.x, &row.y, &row.angle, &row.picked, &row.dest,
                      &row.alive) >= 6)
        cargos.push_back(row);
    }
  }
  std::fclose(f);

  if (seed == 0 && !have_pos) return false;
  // Cave must already match this seed (caller generates first)
  if (g.cave.seed != seed) {
    std::fprintf(stderr, "dualthrust: world save seed %u != cave seed %u\n", seed, g.cave.seed);
    return false;
  }

  if (!read_fog(g.revealed)) {
    std::fprintf(stderr, "dualthrust: missing or short world.fog\n");
    return false;
  }
  g.reveal_dirty = true;

  for (int i = 0; i < static_cast<int>(g.cave.pads.size()) && i < static_cast<int>(pad_active.size()); ++i) {
    g.cave.pads[static_cast<size_t>(i)].active = pad_active[static_cast<size_t>(i)] != 0;
    if (i < static_cast<int>(pad_visited.size()))
      g.cave.pads[static_cast<size_t>(i)].visited = pad_visited[static_cast<size_t>(i)] != 0;
  }

  g.signals = std::move(signals);
  g.signals_cleared = signals_cleared != 0;
  g.pads_cleared = pads_cleared != 0;
  g.score = score;
  g.last_pad = last_pad;
  g.time = time;
  g.explore_tier = explore_tier;
  g.cells_explored = cells_explored;
  g.sonar = {};
  g.sonar_cool = 0.f;

  // Cargo: rebuild from save rows (ignore cave spots)
  ensure_cargo(g);  // creates from spots first
  // Destroy all and recreate from rows for exact state
  {
    std::vector<Entity> kill;
    g.ecs.view<Cargo>([&](Entity e, Cargo& c) {
      b2DestroyBody(c.body);
      kill.push_back(e);
    });
    for (Entity e : kill) g.ecs.destroy(e);
    g.cargo_generation = g.cave.generation;
    for (const CargoRow& row : cargos) {
      if (!row.alive) continue;
      const int kind = std::clamp(row.kind, 0, CARGO_DEF_COUNT - 1);
      const CargoDef& def = CARGO_DEFS[kind];
      const Vec2 pos{row.x, row.y};
      Entity e = g.ecs.create();
      g.ecs.add<Transform>(e, {pos, row.angle});
      g.ecs.add<Cargo>(e, {g.phys.create_cargo(pos, row.angle, def), &def, row.picked != 0, 0.f, row.dest});
    }
  }

  if (ship_index_out) *ship_index_out = std::clamp(ship, 0, SHIP_DEF_COUNT - 1);

  if (have_pos && g.ship != NULL_ENTITY) {
    place_ship(g, {pos_x, pos_y}, angle);
    if (g.ecs.has<Motion>(g.ship)) {
      Motion& m = g.ecs.get<Motion>(g.ship);
      m.vel = {vel_x, vel_y};
      m.ang_vel = ang_vel;
      // push into Box2D
      Body& body = g.ecs.get<Body>(g.ship);
      b2Body_SetLinearVelocity(body.b.hull, {vel_x / PPM, vel_y / PPM});
      b2Body_SetAngularVelocity(body.b.hull, ang_vel);
    }
    if (g.ecs.has<Flight>(g.ship)) {
      Flight& fl = g.ecs.get<Flight>(g.ship);
      fl.state = static_cast<FlightState>(std::clamp(flight, 0, 2));
      fl.fuel = clampf(fuel, 0.f, 1.f);
      fl.hurt = clampf(hurt, 0.f, 1.f);
    }
    if (g.ecs.has<Legs>(g.ship)) g.ecs.get<Legs>(g.ship).deployed = legs != 0;
    if (g.ecs.has<Rope>(g.ship)) {
      debug_rope(g, clampf(rope_len, rope::MIN_LEN, rope::OUT_LEN));
      g.ecs.get<Rope>(g.ship).out = rope_out != 0;
    }
    // Re-grab held cargo
    if (held >= 0) {
      std::vector<Entity> ents;
      g.ecs.view<Cargo>([&](Entity e, const Cargo&) { ents.push_back(e); });
      if (held < static_cast<int>(ents.size())) grab_crate(g, ents[static_cast<size_t>(held)]);
    }
  }

  snap_camera(g);
  return true;
}
