// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "systems.hpp"

#include <array>
#include <cstdio>

namespace {

Vec2 to_world(const Transform& t, Vec2 local) { return t.pos + rotate(local, t.angle); }

// ---------------------------------------------------------------------------
// Particles
// ---------------------------------------------------------------------------
void emit(Game& g, Vec2 pos, Vec2 vel, const Particle& proto) {
  if (g.ecs.pool<Particle>().size() >= tune::MAX_PARTICLES) return;
  Entity e = g.ecs.create();
  g.ecs.add<Transform>(e, {pos, 0.f});
  g.ecs.add<Motion>(e, {vel, 0.f});
  g.ecs.add<Particle>(e, proto);
}

void burst(Game& g, Vec2 pos, Vec2 normal, int count, float speed, float spread, Rgba from, Rgba to,
           float gravity, float ttl) {
  for (int i = 0; i < count; ++i) {
    float a = (g.rng.next() - 0.5f) * spread;
    Vec2 dir = rotate(normal, a);
    Particle p;
    p.ttl = p.life = ttl * g.rng.range(0.5f, 1.f);
    p.size = g.rng.range(1.5f, 3.5f);
    p.drag = 1.2f;
    p.gravity = gravity;
    p.from = from;
    p.to = to;
    emit(g, pos, dir * (speed * g.rng.range(0.25f, 1.f)), p);
  }
}

void exhaust_system(Game& g, float dt) {
  g.ecs.view<Thrusters, Transform, Motion, Hull, Flight>(
      [&](Entity, Thrusters& th, Transform& t, Motion& m, Hull& h, Flight& f) {
        if (f.state != FlightState::Flying) return;
        const ShipDef& d = *h.def;
        Vec2 down = rotate({0.f, 1.f}, t.angle);  // exhaust direction
        Vec2 side{-down.y, down.x};
        for (int i = 0; i < 2; ++i) {
          float lvl = th.level[i];
          if (lvl < 0.05f) { th.emit_acc[i] = 0.f; continue; }
          th.emit_acc[i] += tune::EXHAUST_RATE * lvl * dt;
          Vec2 nozzle = to_world(t, {i == 0 ? -d.engine_offset_x : d.engine_offset_x, d.nozzle_y()});
          for (; th.emit_acc[i] >= 1.f; th.emit_acc[i] -= 1.f) {
            Particle p;
            p.ttl = p.life = g.rng.range(0.18f, 0.45f);
            p.size = g.rng.range(1.5f, 3.f);
            p.drag = 1.5f;
            p.from = pal::FLAME_CORE;
            p.to = with_alpha(pal::FLAME_EDGE, 0);
            Vec2 v = m.vel + down * (140.f + 160.f * lvl * g.rng.next()) + side * g.rng.range(-35.f, 35.f);
            emit(g, nozzle + down * g.rng.range(0.f, 6.f), v, p);
          }
        }
      });
}

void particle_system(Game& g, float dt) {
  const Cave& cave = g.cave;
  g.ecs.view<Particle, Transform, Motion>([&](Entity e, Particle& p, Transform& t, Motion& m) {
    p.life -= dt;
    m.vel = m.vel * std::max(0.f, 1.f - p.drag * dt);
    m.vel.y += tune::GRAVITY * p.gravity * dt;
    t.pos += m.vel * dt;
    t.pos.x = Cave::wrap_x(t.pos.x);
    // Sparks die in rock, but get a moment of grace so bursts can leave the surface
    const bool embedded = p.ttl - p.life > 0.08f && cave.is_solid_world(t.pos.x, t.pos.y);
    if (p.life <= 0.f || embedded) g.dead.push_back(e);
  });
  for (Entity e : g.dead) g.ecs.destroy(e);
  g.dead.clear();
}

void event_system(Game& g) {
  for (SimEvent& ev : g.events) {
    g.fired.push_back(ev);
    ev.pos += ev.normal * 8.f;  // contact points lie inside the rock; start effects in the air
    switch (ev.kind) {
      case SimEventKind::Landed:
        burst(g, ev.pos, ev.normal, 14, 90.f, PI * 1.2f, with_alpha(pal::PAD, 200), with_alpha(pal::PAD, 0),
              0.f, 0.8f);
        break;
      case SimEventKind::Bounce:
        burst(g, ev.pos, ev.normal, static_cast<int>(ev.strength / 8.f), 140.f, PI * 0.9f, pal::WARN,
              with_alpha(pal::HOT, 0), 0.6f, 0.6f);
        g.cam.shake = std::max(g.cam.shake, clampf(ev.strength / 250.f, 0.f, 0.5f));
        break;
      case SimEventKind::Crashed:
        burst(g, ev.pos, ev.normal, 140, 340.f, PI * 1.6f, pal::FLAME_CORE, with_alpha(pal::HOT, 0), 0.8f,
              1.6f);
        burst(g, ev.pos, ev.normal, 40, 160.f, PI * 2.f, pal::BRIGHT, with_alpha(pal::DIM, 0), 1.f, 2.2f);
        g.cam.shake = 1.f;
        break;
    }
  }
  g.events.clear();
}

// ---------------------------------------------------------------------------
// Flight: thrust, gravity, drag, integration
// ---------------------------------------------------------------------------
void flight_system(Game& g, float dt) {
  g.ecs.view<Hull, Transform, Motion, Thrusters, Flight>(
      [&](Entity, Hull& h, Transform& t, Motion& m, Thrusters& th, Flight& f) {
        if (f.state != FlightState::Flying) return;
        const ShipDef& d = *h.def;
        const Vec2 thrust_dir = rotate({0.f, -1.f}, t.angle);  // toward the nose
        Vec2 force{};
        float torque = 0.f;
        for (int i = 0; i < 2; ++i) {
          if (th.level[i] <= 0.f) continue;
          Vec2 fv = thrust_dir * (d.max_thrust * th.level[i]);
          force += fv;
          Vec2 r = rotate({i == 0 ? -d.engine_offset_x : d.engine_offset_x, d.eng_y()}, t.angle);
          torque += r.x * fv.y - r.y * fv.x;
        }
        m.vel += force * (dt / d.mass);
        m.vel.y += tune::GRAVITY * dt;
        m.vel = m.vel * std::max(0.f, 1.f - tune::LINEAR_DRAG * dt);
        t.pos += m.vel * dt;
        t.pos.x = Cave::wrap_x(t.pos.x);
        m.ang_vel += (torque / d.inertia) * dt;
        m.ang_vel *= std::max(0.f, 1.f - tune::ANGULAR_DRAG * dt);
        m.ang_vel = clampf(m.ang_vel, -tune::MAX_ANGULAR_VEL, tune::MAX_ANGULAR_VEL);
        t.angle += m.ang_vel * dt;
        while (t.angle > PI) t.angle -= 2.f * PI;
        while (t.angle < -PI) t.angle += 2.f * PI;
      });
}

// ---------------------------------------------------------------------------
// Collision: probe points vs. the rock grid
// ---------------------------------------------------------------------------
using Probes = std::array<Vec2, 10>;

Probes make_probes(const ShipDef& d) {
  const float ox = d.engine_offset_x, ey = d.eng_y(), fy = d.foot_y();
  return {{
      // feet / belly (always ground-side)
      {-ox * 0.6f, fy}, {0.f, fy + 4.f}, {ox * 0.6f, fy},
      {-d.half_w * 0.7f, fy - 2.f}, {d.half_w * 0.7f, fy - 2.f},
      // engines
      {-ox, ey}, {ox, ey},
      // nose / sides
      {0.f, -d.half_h * 0.85f}, {-d.half_w * 0.55f, 0.f}, {d.half_w * 0.55f, 0.f},
  }};
}

bool first_hit(const Cave& cave, const Transform& t, const Probes& probes, Vec2& contact) {
  for (const Vec2& lp : probes) {
    Vec2 wp = to_world(t, lp);
    if (cave.is_solid_world(wp.x, wp.y)) { contact = wp; return true; }
  }
  if (cave.is_solid_world(t.pos.x, t.pos.y)) { contact = t.pos; return true; }
  return false;
}

// Outward surface normal near `contact`, from the rock density around it
Vec2 surface_normal(const Cave& cave, Vec2 contact, Vec2 vel) {
  const float s = Cave::CELL;
  static constexpr float offs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
  Vec2 grad{};
  for (const auto& o : offs)
    if (cave.is_solid_world(contact.x + o[0] * s, contact.y + o[1] * s)) grad += {o[0], o[1]};
  float gl = length(grad);
  if (gl > 1e-3f) return grad * (-1.f / gl);
  float vl = length(vel);
  return vl > 1e-3f ? vel * (-1.f / vl) : Vec2{0.f, -1.f};
}

void collision_system(Game& g) {
  const Cave& cave = g.cave;
  g.ecs.view<Hull, Transform, Motion, Flight>([&](Entity, Hull& h, Transform& t, Motion& m, Flight& f) {
    if (f.state != FlightState::Flying) return;
    const ShipDef& d = *h.def;
    const Probes probes = make_probes(d);
    Vec2 contact;
    if (!first_hit(cave, t, probes, contact)) return;

    const Vec2 n = surface_normal(cave, contact, m.vel);

    // Push the hull out along the normal
    Vec2 scratch;
    for (int step = 0; step < 24 && first_hit(cave, t, probes, scratch); ++step) {
      t.pos += n * 3.f;
      t.pos.x = Cave::wrap_x(t.pos.x);
    }

    const float vn = -dot(m.vel, n);  // > 0 when moving into the rock
    if (vn <= 0.f) return;

    const Vec2 cpos{Cave::wrap_x(contact.x), contact.y};
    const bool pad = cave.on_pad(cpos.x, cpos.y + Cave::CELL) || cave.on_pad(t.pos.x, t.pos.y + d.foot_y() + 8.f);
    const bool gentle = m.vel.y < tune::LAND_MAX_VY && std::abs(m.vel.x) < tune::LAND_MAX_VX &&
                        std::abs(t.angle) < tune::LAND_MAX_ANGLE && std::abs(m.ang_vel) < tune::LAND_MAX_ANGVEL;

    if (pad && gentle && vn < tune::CRASH_IMPACT_SPEED * 0.5f) {
      f = {FlightState::Landed, 0.f};
      m = {};
      t.angle = 0.f;
      g.events.push_back({SimEventKind::Landed, cpos, n, vn});
      std::printf("LANDED\n");
    } else if (vn > tune::CRASH_IMPACT_SPEED) {
      f = {FlightState::Crashed, 0.f};
      m = {};
      g.events.push_back({SimEventKind::Crashed, cpos, n, vn});
      std::printf("CRASH impact=%.1f\n", vn);
    } else {
      Vec2 tangent = m.vel - n * dot(m.vel, n);
      m.vel = n * (vn * tune::BOUNCE_RESTITUTION) + tangent * tune::BOUNCE_FRICTION;
      m.ang_vel *= tune::BOUNCE_ANG_DAMP;
      if (vn > 30.f) g.events.push_back({SimEventKind::Bounce, cpos, n, vn});
    }
  });
}

}  // namespace

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
void create_ship(Game& g, int def_index) {
  g.ship = g.ecs.create();
  g.ecs.add<Transform>(g.ship);
  g.ecs.add<Motion>(g.ship);
  g.ecs.add<Hull>(g.ship, {&SHIP_DEFS[std::clamp(def_index, 0, SHIP_DEF_COUNT - 1)]});
  g.ecs.add<Thrusters>(g.ship);
  g.ecs.add<Flight>(g.ship);
}

int ship_def_index(const Game& g) { return static_cast<int>(g.ecs.get<Hull>(g.ship).def - SHIP_DEFS); }

void set_ship_def(Game& g, int def_index) {
  def_index = (def_index % SHIP_DEF_COUNT + SHIP_DEF_COUNT) % SHIP_DEF_COUNT;
  g.ecs.get<Hull>(g.ship).def = &SHIP_DEFS[def_index];
  Motion& m = g.ecs.get<Motion>(g.ship);
  m.ang_vel *= 0.5f;
  m.vel = m.vel * 0.7f;
}

void respawn_ship(Game& g, float wx) {
  const Cave& cave = g.cave;
  const ShipDef& d = *g.ecs.get<Hull>(g.ship).def;
  Transform& t = ship_transform(g);
  const float need_w = std::max(d.half_w, d.engine_offset_x) + 8.f;
  const float need_h = d.half_h + d.engine_offset_y + 12.f;

  auto try_pad = [&](const LandingPad& p) {
    float cx = Cave::wrap_x(0.5f * (p.x0 + p.x1));
    for (float y = p.y - need_h - 10.f; y > p.y - 320.f; y -= 8.f)
      if (cave.is_open_box(cx, y, need_w, need_h)) { t.pos = {cx, y}; return true; }
    return false;
  };

  // Prefer the pad nearest wx, then any pad, then any open spot near wx
  bool ok = false;
  int best = -1;
  float best_d = 1e12f;
  for (int i = 0; i < static_cast<int>(cave.pads.size()); ++i) {
    float dist = std::abs(Cave::wrap_delta(wx, 0.5f * (cave.pads[i].x0 + cave.pads[i].x1)));
    if (dist < best_d) { best_d = dist; best = i; }
  }
  if (best >= 0) ok = try_pad(cave.pads[best]);
  for (size_t i = 0; i < cave.pads.size() && !ok; ++i) ok = try_pad(cave.pads[i]);
  for (float y = Cave::WORLD_H * 0.2f; y < Cave::WORLD_H * 0.8f && !ok; y += 16.f)
    if (cave.is_open_box(Cave::wrap_x(wx), y, need_w, need_h)) { t.pos = {Cave::wrap_x(wx), y}; ok = true; }
  if (!ok) t.pos = {Cave::wrap_x(wx), Cave::WORLD_H * 0.4f};

  t.angle = 0.f;
  g.ecs.get<Motion>(g.ship) = {};
  g.ecs.get<Thrusters>(g.ship) = {};
  g.ecs.get<Flight>(g.ship) = {};

  // Drop leftover particles
  for (size_t i = 0; i < g.ecs.pool<Particle>().size(); ++i) g.dead.push_back(g.ecs.pool<Particle>().owner(i));
  for (Entity e : g.dead) g.ecs.destroy(e);
  g.dead.clear();
  g.events.clear();
  g.fired.clear();
}

void relight_ship(Game& g) {
  Flight& f = g.ecs.get<Flight>(g.ship);
  if (f.state != FlightState::Landed) return;
  f = {};
  g.ecs.get<Motion>(g.ship).vel.y = -30.f;
}

void set_thrust(Game& g, float left, float right) {
  Thrusters& th = g.ecs.get<Thrusters>(g.ship);
  th.level[0] = left;
  th.level[1] = right;
}

void snap_camera(Game& g) {
  const Vec2 p = ship_transform(g).pos;
  g.cam.x = p.x - g.cam.vw * 0.5f;
  g.cam.y = p.y - g.cam.vh * 0.55f;
  g.cam.prev_ship_x = p.x;
  g.cam.have_prev = true;
  g.cam.shake = 0.f;
}

void step_sim(Game& g, float dt) {
  g.time += dt;
  flight_system(g, dt);
  collision_system(g);
  g.ecs.view<Flight>([dt](Entity, Flight& f) {
    if (f.state != FlightState::Flying) f.timer += dt;
  });
  event_system(g);
  exhaust_system(g, dt);
  particle_system(g, dt);
}

void update_camera(Game& g, float dt) {
  Camera& c = g.cam;
  const Transform& t = ship_transform(g);
  const Motion& m = g.ecs.get<Motion>(g.ship);
  if (c.have_prev) {
    float d = t.pos.x - c.prev_ship_x;
    if (d > Cave::WORLD_W * 0.5f) c.x -= Cave::WORLD_W;
    else if (d < -Cave::WORLD_W * 0.5f) c.x += Cave::WORLD_W;
  }
  c.prev_ship_x = t.pos.x;
  c.have_prev = true;
  // Look ahead a little in the direction of travel
  const Vec2 lead = {clampf(m.vel.x * 0.35f, -180.f, 180.f), clampf(m.vel.y * 0.25f, -120.f, 120.f)};
  const float k = 1.f - std::exp(-5.f * dt);
  c.x += (t.pos.x + lead.x - c.vw * 0.5f - c.x) * k;
  c.y += (t.pos.y + lead.y - c.vh * 0.55f - c.y) * k;
  c.y = clampf(c.y, -100.f, Cave::WORLD_H - c.vh * 0.3f);

  c.shake = std::max(0.f, c.shake - dt * 2.2f);
  c.shake_off = c.shake > 0.f ? Vec2{g.rng.range(-1.f, 1.f), g.rng.range(-1.f, 1.f)} * (c.shake * c.shake * 14.f)
                              : Vec2{};
}
