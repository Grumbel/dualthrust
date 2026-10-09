// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "systems.hpp"

#include <vector>

#include <algorithm>
#include <cmath>

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
        for (int i = 0; i < std::min(thruster_count(d), Thrusters::MAX); ++i) {
          const ThrusterPose tp = thruster_pose(d, i);
          const float dmg = clampf(th.damage[i], 0.f, 1.f);
          const float lvl = th.output[i];  // damage + flutter already baked in by forces_system
          if (dmg >= tune::ENGINE_DEAD || lvl < 0.05f) { th.emit_acc[i] = 0.f; continue; }
          const Vec2 down = rotate(tp.flame, t.angle);  // exhaust direction
          const Vec2 side{-down.y, down.x};
          // Damaged nozzles belch more volume of dirtier gas for the same thrust
          const float rate_mul = 1.f + 0.7f * dmg;
          th.emit_acc[i] += tune::EXHAUST_RATE * lvl * rate_mul * dt;
          Vec2 nozzle = to_world(t, tp.nozzle);
          for (; th.emit_acc[i] >= 1.f; th.emit_acc[i] -= 1.f) {
            Particle p;
            const bool spark = dmg > 0.08f && g.rng.next() < 0.12f + 0.35f * dmg;
            if (spark) {
              // Hot sparks: short-lived, fast, gravity-affected
              p.ttl = p.life = g.rng.range(0.08f, 0.22f);
              p.size = g.rng.range(1.0f, 2.2f);
              p.drag = 0.6f;
              p.gravity = 0.35f;
              p.from = pal::SPARK;
              p.to = with_alpha(pal::HOT, 0);
              Vec2 v = m.vel + down * (180.f + 220.f * g.rng.next()) + side * g.rng.range(-55.f, 55.f);
              emit(g, nozzle + down * g.rng.range(0.f, 4.f), v, p);
            } else {
              p.ttl = p.life = g.rng.range(0.18f, 0.45f + 0.25f * dmg);
              p.size = g.rng.range(1.5f, 3.f + 1.5f * dmg);
              p.drag = 1.5f + 0.8f * dmg;
              // Healthy: bright flame. Damaged: soot/smoke mixed in, core dims toward grey.
              if (dmg < 0.05f) {
                p.from = pal::FLAME_CORE;
                p.to = with_alpha(pal::FLAME_EDGE, 0);
              } else {
                p.from = mix(pal::FLAME_CORE, pal::SMOKE, 0.25f + 0.65f * dmg);
                p.to = with_alpha(mix(pal::FLAME_EDGE, pal::SMOKE, 0.4f + 0.5f * dmg), 0);
              }
              const float spread = 35.f + 40.f * dmg;
              Vec2 v = m.vel + down * (140.f + 160.f * lvl * g.rng.next()) + side * g.rng.range(-spread, spread);
              emit(g, nozzle + down * g.rng.range(0.f, 6.f), v, p);
            }
          }
        }
      });
}


void wreckage_system(Game& g, float dt) {
  if (g.wreckage.empty()) return;
  for (Game::WreckPart& w : g.wreckage) {
    if (w.settled) continue;
    // Integrate
    w.vel.y += tune::GRAVITY * 0.85f * dt;
    w.vel.x *= (1.f - 0.4f * dt);
    w.vel.y *= (1.f - 0.15f * dt);
    const Vec2 mid0 = (w.a + w.b) * 0.5f;
    const Vec2 d = (w.b - w.a) * 0.5f;
    float ang = std::atan2(d.y, d.x);
    ang += w.ang_vel * dt;
    w.ang_vel *= (1.f - 1.2f * dt);
    const float half = length(d);
    const Vec2 mid1 = mid0 + w.vel * dt;
    // Ground / rock collision at midpoint
    if (g.cave.is_solid_world(mid1.x, mid1.y)) {
      // Nudge out and settle
      w.vel = {};
      w.ang_vel = 0.f;
      w.settled = true;
      // Rest just above rock: step up a few cells
      Vec2 rest = mid0;
      for (int k = 0; k < 12; ++k) {
        if (!g.cave.is_solid_world(rest.x, rest.y - 2.f)) break;
        rest.y -= 2.f;
      }
      const float c = std::cos(ang), s = std::sin(ang);
      w.a = {rest.x - half * c, rest.y - half * s};
      w.b = {rest.x + half * c, rest.y + half * s};
      continue;
    }
    const float c = std::cos(ang), s = std::sin(ang);
    w.a = {mid1.x - half * c, mid1.y - half * s};
    w.b = {mid1.x + half * c, mid1.y + half * s};
    // Low speed settle
    if (length(w.vel) < 18.f && g.cave.is_solid_world(mid1.x, mid1.y + 4.f)) {
      w.vel = {};
      w.ang_vel = 0.f;
      w.settled = true;
    }
  }
}

void particle_system(Game& g, float dt) {
  const Cave& cave = g.cave;
  g.ecs.view<Particle, Transform, Motion>([&](Entity e, Particle& p, Transform& t, Motion& m) {
    p.life -= dt;
    m.vel = m.vel * std::max(0.f, 1.f - p.drag * dt);
    m.vel.y += tune::GRAVITY * p.gravity * dt;
    t.pos += m.vel * dt;
    // Sparks die in rock, but get a moment of grace so bursts can leave the surface
    const bool embedded = p.ttl - p.life > 0.08f && cave.is_solid_world(t.pos.x, t.pos.y);
    if (p.life <= 0.f || embedded) g.dead.push_back(e);
  });
  for (Entity e : g.dead) g.ecs.destroy(e);
  g.dead.clear();
}

// Scatter ship outline scraps into permanent wreckage (survives respawn).
static void spawn_wreckage(Game& g) {
  if (!g.ecs.has<Transform>(g.ship) || !g.ecs.has<Hull>(g.ship)) return;
  const Transform& tf = g.ecs.get<Transform>(g.ship);
  const Motion& m = g.ecs.get<Motion>(g.ship);
  const ShipDef& d = *g.ecs.get<Hull>(g.ship).def;
  const HullGeom hg = hull_geom(d);
  const LegGeom lg = leg_geom(d);

  auto push_part = [&](Vec2 la, Vec2 lb, Rgba col) {
    while (g.wreckage.size() >= tune::MAX_WRECKAGE) {
      // Drop oldest settled first, else oldest
      size_t drop = 0;
      for (size_t i = 0; i < g.wreckage.size(); ++i)
        if (g.wreckage[i].settled) { drop = i; break; }
      g.wreckage[drop] = g.wreckage.back();
      g.wreckage.pop_back();
    }
    const Vec2 wa = tf.pos + rotate(la, tf.angle);
    const Vec2 wb = tf.pos + rotate(lb, tf.angle);
    const Vec2 mid = (wa + wb) * 0.5f;
    const Vec2 outward = mid - tf.pos;
    const float olen = length(outward);
    Vec2 kick = olen > 1.f ? outward * (1.f / olen) : Vec2{0.f, -1.f};
    kick = kick * g.rng.range(80.f, 220.f) + m.vel * g.rng.range(0.2f, 0.7f);
    kick.x += g.rng.range(-60.f, 60.f);
    kick.y += g.rng.range(-40.f, 40.f);
    Game::WreckPart p;
    p.a = wa;
    p.b = wb;
    p.vel = kick;
    p.ang_vel = g.rng.range(-8.f, 8.f);
    p.settled = false;
    p.col = col;
    g.wreckage.push_back(p);
  };

  // Hull silhouette broken into short segments
  const Vec2 outline[] = {{0.f, hg.nose_y}, {hg.cabin_hw(), hg.cabin_y}, {hg.belly_hw(), hg.belly_y},
                          {-hg.belly_hw(), hg.belly_y}, {-hg.cabin_hw(), hg.cabin_y}};
  constexpr int N = 5;
  for (int i = 0; i < N; ++i) {
    const Vec2 a = outline[i], b = outline[(i + 1) % N];
    // Split each edge into 2–3 scraps
    for (int k = 0; k < 3; ++k) {
      const float t0 = static_cast<float>(k) / 3.f;
      const float t1 = static_cast<float>(k + 1) / 3.f;
      push_part(a + (b - a) * t0, a + (b - a) * t1, mix(pal::BRIGHT, pal::HOT, g.rng.range(0.f, 0.5f)));
    }
  }
  // Engine bells
  for (int i = 0; i < thruster_count(d); ++i) {
    const ThrusterPose tp = thruster_pose(d, i);
    const Vec2 f = tp.flame, p = {-f.y, f.x};
    const float w = 6.f + 2.f * tp.power;
    push_part(tp.pos - p * w, tp.pos + p * w, pal::WARN);
    push_part(tp.pos, tp.pos + f * 10.f, mix(pal::MID, pal::HOT, 0.4f));
  }
  // Legs
  for (int i = 0; i < 2; ++i) {
    const float side = i == 0 ? -1.f : 1.f;
    const Vec2 attach{side * lg.attach_x, lg.attach_y};
    const Vec2 foot{side * lg.foot_x, lg.foot_y};
    push_part(attach, foot, pal::MID);
    push_part({foot.x - lg.foot_half_w, foot.y}, {foot.x + lg.foot_half_w, foot.y}, pal::DIM);
  }
  // Extra random scrap cloud
  for (int i = 0; i < 18; ++i) {
    const float lx = g.rng.range(-d.half_w, d.half_w);
    const float ly = g.rng.range(-d.half_h, d.half_h);
    const float ang = g.rng.range(0.f, 6.28f);
    const float len = g.rng.range(4.f, 14.f);
    push_part({lx, ly}, {lx + std::cos(ang) * len, ly + std::sin(ang) * len},
              mix(pal::BRIGHT, pal::SMOKE, g.rng.range(0.2f, 0.8f)));
  }
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
      default: break;  // beeps have no particles
      case SimEventKind::Crashed:
        burst(g, ev.pos, ev.normal, 140, 340.f, PI * 1.6f, pal::FLAME_CORE, with_alpha(pal::HOT, 0), 0.8f,
              1.6f);
        burst(g, ev.pos, ev.normal, 40, 160.f, PI * 2.f, pal::BRIGHT, with_alpha(pal::DIM, 0), 1.f, 2.2f);
        spawn_wreckage(g);
        g.cam.shake = 1.f;
        break;
    }
  }
  g.events.clear();
}


// ---------------------------------------------------------------------------
// Rigid body: forces in, Box2D step, state out
// ---------------------------------------------------------------------------
void stat_add(Game& g, double Stats::* field, double v) {
  if (!g.stats_enabled) return;
  g.stats.*field += v;
  g.stats_dirty = true;
}

// Engine thrust at the nozzle mounts, and the leg springs. The legs are light bodies on prismatic joints; the
// joint only guides them, the spring (stiffness for half the ship's mass each, so it feels the same whatever the
// leg mass) is applied here, which also lets the retract button simply move its target.
void forces_system(Game& g) {
  g.ecs.view<Body, Hull, Thrusters, Flight, Legs>([&](Entity, Body& body, Hull& h, Thrusters& th, Flight& f, Legs& lg) {
    const ShipDef& d = *h.def;
    const ShipBodies& sb = body.b;
    const b2Rot q = b2Body_GetRotation(sb.hull);

    // Clear effective outputs every step so landed/crashed ships go silent in exhaust and audio.
    for (int i = 0; i < Thrusters::MAX; ++i) th.output[i] = 0.f;

    if (f.state != FlightState::Crashed) {
      // Power scales with fuel but floors at FUEL_LIMP so the ship can always limp home
      float power = f.fuel >= tune::FUEL_LIMP ? 1.f
                    : (0.35f + 0.65f * (f.fuel / tune::FUEL_LIMP));
      power *= 1.f - 0.45f * f.hurt;  // soft damage trims thrust; still flies
      const int n_eng = std::min(thruster_count(d), Thrusters::MAX);
      for (int i = 0; i < n_eng; ++i) {
        const ThrusterPose tp = thruster_pose(d, i);
        const float dmg = clampf(th.damage[i], 0.f, 1.f);
        float level = th.level[tp.channel];
        if (level <= 0.f || dmg >= tune::ENGINE_DEAD) continue;
        // Mild sputter when limping or hurt (fun feedback, not a hard fail)
        if ((f.fuel < tune::FUEL_LIMP || f.hurt > 0.2f) && (tp.channel & 1))
          level *= 0.75f + 0.25f * std::sin(g.time * 17.f + float(i));

        // Per-engine health: damaged nozzles lose most of their force before going dead.
        float health = 1.f - 0.9f * dmg;

        // Always-on random flutter so thrust is never perfectly steady.
        // Multi-frequency sines + a small rng kick keep it irregular without pure noise jitter.
        {
          const float fi = float(i);
          const float wave =
              0.50f * std::sin(g.time * 19.3f + fi * 4.7f) +
              0.30f * std::sin(g.time * 31.1f + fi * 2.3f) +
              0.20f * std::sin(g.time * 7.9f + fi * 9.1f);
          health *= 1.f + tune::ENGINE_FLUTTER * wave + tune::ENGINE_FLUTTER * (g.rng.next() - 0.5f);
        }

        // Above ENGINE_SPUTTER the nozzle coughs: deep irregular cuts + random misfires.
        if (dmg >= tune::ENGINE_SPUTTER) {
          const float t_sp = (dmg - tune::ENGINE_SPUTTER) / (1.f - tune::ENGINE_SPUTTER);
          const float depth = 0.35f + 0.55f * t_sp;
          float wave = 0.5f + 0.5f * std::sin(g.time * (12.f + 18.f * dmg) + float(i) * 2.1f);
          wave = wave * 0.55f + g.rng.next() * 0.45f;  // less mechanical than pure sine
          if (g.rng.next() < 0.03f + 0.22f * t_sp)
            wave *= g.rng.range(0.0f, 0.3f);  // hard misfire
          health *= (1.f - depth) + depth * wave;
        }

        health = std::max(0.f, health);
        const float out = level * tp.power * power * health;
        th.output[i] = out;
        if (out < 0.01f) continue;
        const float force = d.mass * g.dbg_mass_mul * d.max_thrust * g.dbg_thrust_mul / PPM * out;
        const b2Vec2 at = b2Body_GetWorldPoint(sb.hull, to_b2(tp.pos));
        b2Body_ApplyForce(sb.hull, b2MulSV(force, b2RotateVector(q, {tp.push.x, tp.push.y})), at, true);
      }
    }

    if (!b2Body_IsAwake(sb.hull)) return;  // resting: springs are balanced; do not keep the bodies awake
    const LegGeom lgeo = leg_geom(d);
    const float m_eff = d.mass * g.dbg_mass_mul * 0.5f;
    const float omega = 2.f * PI * tune::LEG_HERTZ;
    const float k = f.state == FlightState::Crashed ? 0.f : m_eff * omega * omega;
    const float c = 2.f * tune::LEG_DAMPING * m_eff * omega;
    const float target = lg.deployed ? 0.f : -lgeo.travel / PPM;
    for (int i = 0; i < 2; ++i) {
      const float side = i == 0 ? -1.f : 1.f;
      const b2Vec2 axis = b2RotateVector(q, {side * lgeo.axis_x, lgeo.axis_y});
      const float x = b2PrismaticJoint_GetTranslation(sb.joint[i]);
      const float v = b2PrismaticJoint_GetSpeed(sb.joint[i]);
      const float F = k * (target - x) - c * v;  // along the strut, outward positive
      const b2Vec2 fv = b2MulSV(F, axis);
      b2Body_ApplyForceToCenter(sb.leg[i], fv, false);
      b2Body_ApplyForce(sb.hull, b2Neg(fv), b2Body_GetPosition(sb.leg[i]), false);
    }
  });
}

// Box2D -> components
void sync_system(Game& g) {
  g.ecs.view<Body, Transform, Motion, Legs, Hull>([&](Entity, Body& body, Transform& t, Motion& m, Legs& lg, Hull&) {
    const ShipBodies& sb = body.b;
    const b2Vec2 p = b2Body_GetPosition(sb.hull);
    float w = b2Body_GetAngularVelocity(sb.hull);
    if (std::abs(w) > tune::MAX_ANGULAR_VEL) {
      w = clampf(w, -tune::MAX_ANGULAR_VEL, tune::MAX_ANGULAR_VEL);
      b2Body_SetAngularVelocity(sb.hull, w);
    }
    t.pos = from_b2(p);
    t.angle = b2Rot_GetAngle(b2Body_GetRotation(sb.hull));
    m.vel = from_b2(b2Body_GetLinearVelocity(sb.hull));
    m.ang_vel = w;
    for (int i = 0; i < 2; ++i) lg.trans[i] = b2PrismaticJoint_GetTranslation(sb.joint[i]) * PPM;
  });
  g.ecs.view<Body, Rope, Hull>([&](Entity, Body& body, Rope& r, Hull& h) {
    const ShipBodies& sb = body.b;
    if (!h.def->winch || B2_IS_NULL(sb.hook)) return;
    r.anchor = from_b2(b2Body_GetWorldPoint(sb.hull, to_b2({0.f, winch_y(*h.def)})));
    r.hook_pos = from_b2(b2Body_GetPosition(sb.hook));
    r.hook_angle = b2Rot_GetAngle(b2Body_GetRotation(sb.hook));
    r.slack = std::max(0.f, r.length - length(r.hook_pos - r.anchor));
  });
  g.ecs.view<Cargo, Transform>([&](Entity, Cargo& c, Transform& t) {
    if (!b2Body_IsEnabled(c.body)) return;
    t.pos = from_b2(b2Body_GetPosition(c.body));
    t.angle = b2Rot_GetAngle(b2Body_GetRotation(c.body));
  });
}

void beep(Game& g, SimEventKind kind) { g.fired.push_back({kind, {}, {}, 200.f}); }

void notice(Game& g, const char* text) {
  std::snprintf(g.notice, sizeof g.notice, "%s", text);
  g.notice_timer = 2.2f;
}

// Crates sleep as frozen (disabled) bodies until the ground around them is built, and are frozen again once
// they are at rest and the ship has moved on.
void cargo_activation(Game& g) {
  Entity held = NULL_ENTITY;
  g.ecs.view<Rope>([&](Entity, Rope& r) { held = r.held; });
  g.ecs.view<Cargo, Transform>([&](Entity e, Cargo& c, Transform& t) {
    if (!b2Body_IsEnabled(c.body)) {
      if (g.phys.terrain_at(t.pos)) {
        b2Body_Enable(c.body);
        b2Body_SetAwake(c.body, true);
      }
    } else if (e != held && !b2Body_IsAwake(c.body) && !g.phys.terrain_at(t.pos)) {
      b2Body_Disable(c.body);
    }
  });
}

// Reel the cable in and out
void rope_system(Game& g, float dt) {
  g.ecs.view<Body, Rope, Hull>([&](Entity, Body& body, Rope& r, Hull& h) {
    if (!h.def->winch || B2_IS_NULL(body.b.cable) || B2_IS_NULL(body.b.hook)) return;
    const float speed = r.held != NULL_ENTITY ? rope::REEL_SPEED_LOADED : rope::REEL_SPEED;
    const float before = r.length;
    const float target = r.out ? rope::OUT_LEN : rope::MIN_LEN;  // all the way out or all the way in
    r.length = r.length < target ? std::min(target, r.length + speed * dt) : std::max(target, r.length - speed * dt);
    if (r.length != before) {
      b2DistanceJoint_SetLengthRange(body.b.cable, 0.05f, r.length / PPM);
      b2DistanceJoint_SetLength(body.b.cable, r.length / PPM);
      b2Body_SetAwake(body.b.hook, true);
    }
  });
}

// Delivery: a crate that was lifted and now rests on a landing pad
void cargo_system(Game& g, float dt) {
  Entity held = NULL_ENTITY;
  g.ecs.view<Rope>([&](Entity, Rope& r) { held = r.held; });
  // Convenience: landed on home with the winch almost in → drop the load for delivery
  if (held != NULL_ENTITY && g.ecs.has<Flight>(g.ship) && g.ecs.has<Rope>(g.ship) &&
      g.ecs.get<Flight>(g.ship).state == FlightState::Landed) {
    Rope& r = g.ecs.get<Rope>(g.ship);
    const int base = (g.home_pad >= 0 && g.home_pad < static_cast<int>(g.cave.pads.size()))
                         ? g.home_pad : 0;
    if (!g.cave.pads.empty() && r.length <= rope::MIN_LEN + 12.f) {
      const LandingPad& hp = g.cave.pads[static_cast<size_t>(base)];
      const Vec2 sp = ship_transform(g).pos;
      if (hp.active && sp.x >= hp.x0 - 8.f && sp.x <= hp.x1 + 8.f &&
          std::abs(sp.y - hp.y) < 120.f) {
        release_crate(g);
        held = NULL_ENTITY;
        notice(g, "LOAD DROPPED");
      }
    }
  }
  g.ecs.view<Cargo, Transform>([&](Entity e, Cargo& c, Transform& t) {
    if (!b2Body_IsEnabled(c.body)) return;
    const float speed = length(from_b2(b2Body_GetLinearVelocity(c.body)));
    c.rest_time = (speed < 8.f && e != held) ? c.rest_time + dt : 0.f;
    if (!c.picked || c.rest_time < 1.f) return;
    // Return-to-base: only the home pad counts as a delivery
    if (g.cave.pads.empty()) return;
    const int base = (g.home_pad >= 0 && g.home_pad < static_cast<int>(g.cave.pads.size()))
                         ? g.home_pad : 0;
    const LandingPad& p = g.cave.pads[static_cast<size_t>(base)];
    const float bottom = t.pos.y + c.def->half_h;
    if (!p.active || std::abs(bottom - p.y) >= 12.f || t.pos.x < p.x0 || t.pos.x > p.x1) return;

    const int pts = c.def->score > 0 ? c.def->score : tune::SCORE_CARGO;
    c.picked = false;
    stat_add(g, &Stats::cargo_delivered, 1);
    g.score += pts;
    char msg[32];
    std::snprintf(msg, sizeof msg, "+%d %s", pts, c.def->name);
    notice(g, msg);
    beep(g, SimEventKind::Delivered);

    // Respawn this crate deeper in the cave so hauling stays the loop
    if (B2_IS_NON_NULL(c.body) && b2Body_IsValid(c.body)) {
      // Find a floor spot away from base
      for (int tries = 0; tries < 40; ++tries) {
        const float x = g.rng.range(Cave::CELL * 20.f, Cave::WORLD_W - Cave::CELL * 20.f);
        const float floor = g.cave.floor_below(x, Cave::WORLD_H * 0.15f);
        if (floor <= 0.f || floor >= Cave::WORLD_H - 40.f) continue;
        if (std::abs(x - 0.5f * (p.x0 + p.x1)) < 400.f) continue;
        const Vec2 pos{x, floor - c.def->half_h - 1.f};
        if (g.cave.is_solid_world(pos.x, pos.y)) continue;
        b2Body_SetTransform(c.body, to_b2(pos), b2MakeRot(0.f));
        b2Body_SetLinearVelocity(c.body, {0.f, 0.f});
        b2Body_SetAngularVelocity(c.body, 0.f);
        b2Body_SetAwake(c.body, true);
        t.pos = pos;
        t.angle = 0.f;
        c.dest_pad = base;  // always home
        c.rest_time = 0.f;
        break;
      }
    }
    return;
  });
}

// Does the lowest contact lie on a landing pad?
bool touching_pad(const Cave& cave, Vec2 pt) {
  for (const LandingPad& p : cave.pads)
    if (p.active && std::abs(pt.y - p.y) < Cave::CELL && pt.x >= p.x0 - 4.f && pt.x <= p.x1 + 4.f)
      return true;
  return false;
}

void release_crate(Game& g);

// Impacts from Box2D's hit events: hard hits destroy the ship, softer ones bounce with sparks and noise.
void impact_system(Game& g) {
  const b2ContactEvents ce = b2World_GetContactEvents(g.phys.world());
  g.ecs.view<Flight>([&](Entity, Flight& f) {
    for (int i = 0; i < ce.hitCount; ++i) {
      const b2ContactHitEvent& ev = ce.hitEvents[i];
      const Part pa = shape_part(ev.shapeIdA), pb = shape_part(ev.shapeIdB);
      const bool a_terrain = pa == Part::Terrain;
      const Part ship_part = a_terrain ? pb : pa;
      if (!is_ship_part(ship_part) || (a_terrain ? false : pb != Part::Terrain)) continue;  // only ship against rock
      const Vec2 n = Vec2{ev.normal.x, ev.normal.y} * (a_terrain ? 1.f : -1.f);  // out of the rock, unit
      const Vec2 pos{ev.point.x * PPM, ev.point.y * PPM};
      const float speed = ev.approachSpeed * PPM;
      if (f.state == FlightState::Crashed) continue;
      const float limit = (ship_part == Part::Hull || ship_part == Part::Engine)
                              ? tune::CRASH_HULL_SPEED
                              : tune::CRASH_FOOT_SPEED;
      if (speed > limit) {
        f.state = FlightState::Crashed;
        f.timer = f.settle = 0.f;
        g.events.push_back({SimEventKind::Crashed, pos, n, speed});
        stat_add(g, &Stats::crashes, 1);
        std::printf("CRASH impact=%.1f part=%d\n", speed, static_cast<int>(ship_part));
      } else if (speed > tune::HIT_MIN_SPEED) {
        g.events.push_back({SimEventKind::Bounce, pos, n, speed});
        stat_add(g, &Stats::hard_hits, 1);
        Flight& fl = g.ecs.get<Flight>(g.ship);
        if (fl.state == FlightState::Crashed) continue;
        const float hit = clampf((speed - tune::HIT_MIN_SPEED) / 80.f, 0.3f, 1.f);
        fl.hurt = std::min(1.f, fl.hurt + tune::HURT_FROM_HIT * hit);
        // Engine damage only when the nozzle shape itself hits rock
        if (ship_part == Part::Engine && g.ecs.has<Thrusters>(g.ship)) {
          const b2ShapeId eng_shape = a_terrain ? ev.shapeIdB : ev.shapeIdA;
          int ei = shape_engine_index(eng_shape);
          Thrusters& th = g.ecs.get<Thrusters>(g.ship);
          const int n_eng = std::min(thruster_count(*g.ecs.get<Hull>(g.ship).def), Thrusters::MAX);
          if (ei < 0 || ei >= n_eng) ei = 0;
          const float base = tune::ENGINE_DAMAGE_FROM_HIT * hit;
          const float before = th.damage[ei];
          th.damage[ei] = std::min(1.f, th.damage[ei] + base);
          if (before < tune::ENGINE_DEAD && th.damage[ei] >= tune::ENGINE_DEAD)
            notice(g, "ENGINE OUT");
          else if (before < tune::ENGINE_SPUTTER && th.damage[ei] >= tune::ENGINE_SPUTTER)
            notice(g, "ENGINE HURT");
        }
      }
    }
  });
}

// Touching, resting and landed/flying bookkeeping
void ground_system(Game& g, float dt) {
  const float real_dt = dt / tune::TIME_SCALE;
  g.ecs.view<Body, Flight, Motion, Thrusters, Transform>([&](Entity, Body& body, Flight& f, Motion& m, Thrusters& th, Transform& t) {
    f.contacts = 0;
    float lowest = -1e9f;
    for (b2BodyId b : {body.b.hull, body.b.leg[0], body.b.leg[1]}) {
      b2ContactData cd[8];
      const int n = b2Body_GetContactData(b, cd, 8);
      for (int i = 0; i < n; ++i)
        for (int k = 0; k < cd[i].manifold.pointCount; ++k) {
          ++f.contacts;
          const Vec2 pt = from_b2(cd[i].manifold.points[k].point);
          if (pt.y > lowest) { lowest = pt.y; f.contact_pt = pt; }
        }
    }

    const float speed = length(m.vel);
    float thr_max = th.level[0];
    for (int i = 1; i < 6; ++i) thr_max = std::max(thr_max, th.level[i]);
    const bool thrust = thr_max > 0.05f;
    f.timer += dt;
    if (f.state == FlightState::Flying) {
      const bool resting = f.contacts > 0 && !thrust && speed < tune::SETTLE_SPEED && std::abs(m.ang_vel) < tune::SETTLE_ANGVEL &&
                           std::abs(t.angle) < tune::SETTLE_MAX_ANGLE;  // on its side or upside down is not landed
      f.settle = resting ? f.settle + dt : 0.f;
      if (f.settle >= tune::SETTLE_TIME) {
        f.state = FlightState::Landed;
        f.timer = 0.f;
        const bool pad = touching_pad(g.cave, f.contact_pt);
        g.events.push_back({SimEventKind::Landed, f.contact_pt, {0.f, -1.f}, 0.f});
        stat_add(g, &Stats::landings, 1);
        if (pad) {
          stat_add(g, &Stats::pad_landings, 1);
          g.score += tune::SCORE_PAD_LANDING;
          notice(g, "+100 PAD");
          for (int i = 0; i < static_cast<int>(g.cave.pads.size()); ++i) {
            LandingPad& p = g.cave.pads[static_cast<size_t>(i)];
            if (std::abs(f.contact_pt.y - p.y) < Cave::CELL &&
                f.contact_pt.x >= p.x0 - 4.f && f.contact_pt.x <= p.x1 + 4.f) {
              g.last_pad = i;
              p.active = true;   // landing discovers the pad if sonar had not
              p.visited = true;  // teleportable
              break;
            }
          }
        }
        std::printf("LANDED%s\n", pad ? " (pad)" : "");
      }
    } else if (f.state == FlightState::Landed) {
      if (thrust || speed > tune::UNSETTLE_SPEED || f.contacts == 0) {
        f.state = FlightState::Flying;
        f.timer = f.settle = 0.f;
      }
    }

    // Statistics
    if (f.state == FlightState::Flying) {
      stat_add(g, &Stats::flight_time, real_dt);
      stat_add(g, &Stats::distance, speed * dt / PPM);
    } else if (f.state == FlightState::Landed) {
      stat_add(g, &Stats::landed_time, real_dt);
    }
    if (f.state != FlightState::Crashed) {
      float thr_sum = 0.f;
      for (int i = 0; i < 6; ++i) thr_sum += th.level[i];
      stat_add(g, &Stats::thrust_time, thr_sum * real_dt);
    }

    // Fuel: burn while thrusting; refill while settled on a pad
    float demand = 0.f;
    for (int i = 0; i < 6; ++i) demand += th.level[i];
    if (f.state != FlightState::Crashed && f.fuel > 0.f && demand > 0.02f)
      f.fuel = std::max(0.f, f.fuel - tune::FUEL_BURN * demand * 0.25f * dt);
    if (f.state == FlightState::Landed && touching_pad(g.cave, f.contact_pt)) {
      f.fuel = std::min(1.f, f.fuel + tune::FUEL_REFUEL * dt);
      if (f.hurt > 0.f) f.hurt = std::max(0.f, f.hurt - tune::HURT_REPAIR * dt);
      for (int i = 0; i < Thrusters::MAX; ++i)
        if (th.damage[i] > 0.f) th.damage[i] = std::max(0.f, th.damage[i] - tune::ENGINE_REPAIR * dt);
    }
  });
}

}  // namespace

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

// Crates for the current cave (new ones when the cave changed)
void ensure_cargo(Game& g) {
  if (g.cargo_generation == g.cave.generation) return;
  // load_world can call this before the first place_ship / build_bodies; the world
  // must exist before any create_cargo / DestroyBody.
  if (!g.phys.ready()) g.phys.init();
  g.cargo_generation = g.cave.generation;
  g.dead.clear();
  g.ecs.view<Cargo>([&](Entity e, Cargo& c) {
    if (B2_IS_NON_NULL(c.body) && b2Body_IsValid(c.body)) b2DestroyBody(c.body);
    g.dead.push_back(e);
  });
  for (Entity e : g.dead) g.ecs.destroy(e);
  g.dead.clear();
  for (const CargoSpot& s : g.cave.cargo) {
    const CargoDef& def = CARGO_DEFS[std::clamp(s.kind, 0, CARGO_DEF_COUNT - 1)];
    const Vec2 pos{s.x, s.floor_y - def.half_h - 1.f};
    Entity e = g.ecs.create();
    g.ecs.add<Transform>(e, {pos, 0.f});
    const int dest = g.cave.pads.empty() ? -1 : g.home_pad;  // return to base
    g.ecs.add<Cargo>(e, {g.phys.create_cargo(pos, 0.f, def), &def, false, 0.f, dest});
  }
}

// Let go of the crate on the hook (if any) and return it to a free crate's behaviour
void release_crate(Game& g) {
  Rope& r = g.ecs.get<Rope>(g.ship);
  Body& body = g.ecs.get<Body>(g.ship);
  if (r.held == NULL_ENTITY) return;
  if (B2_IS_NON_NULL(body.b.grip) && b2Joint_IsValid(body.b.grip)) b2DestroyJoint(body.b.grip);
  body.b.grip = b2_nullJointId;
  Cargo& c = g.ecs.get<Cargo>(r.held);
  b2Body_SetLinearDamping(c.body, 0.05f);
  b2Body_SetAngularDamping(c.body, 0.3f);
  b2ShapeId sid;
  if (b2Body_GetShapes(c.body, &sid, 1) == 1) {
    b2Filter f = b2Shape_GetFilter(sid);
    f.maskBits |= CAT_SHIP;
    b2Shape_SetFilter(sid, f);
  }
  r.held = NULL_ENTITY;
}

// (Re)create the ship's rigid bodies at a pose, keeping or resetting its state
void build_bodies(Game& g, Vec2 pos, float angle, Vec2 vel, float ang_vel) {
  Body& body = g.ecs.get<Body>(g.ship);
  g.phys.init();
  ensure_cargo(g);
  release_crate(g);
  g.phys.destroy_ship(body.b);
  g.ecs.get<Rope>(g.ship) = {};
  g.phys.stream(g.cave, pos, 9);  // the ground must exist before the ship does
  const ShipDef& def = *g.ecs.get<Hull>(g.ship).def;
  body.b = g.phys.create_ship(def, pos, angle, vel, ang_vel);
  if (g.dbg_mass_mul != 1.f) g.phys.apply_ship_mass(body.b, def, g.dbg_mass_mul);
  Transform& t = g.ecs.get<Transform>(g.ship);
  t.pos = pos;
  t.angle = angle;
  g.ecs.get<Motion>(g.ship) = {vel, ang_vel};
}

void create_ship(Game& g, int def_index) {
  g.ship = g.ecs.create();
  g.ecs.add<Transform>(g.ship);
  g.ecs.add<Motion>(g.ship);
  g.ecs.add<Hull>(g.ship, {&SHIP_DEFS[std::clamp(def_index, 0, SHIP_DEF_COUNT - 1)]});
  g.ecs.add<Thrusters>(g.ship);
  g.ecs.add<Flight>(g.ship);
  g.ecs.add<Body>(g.ship);
  g.ecs.add<Legs>(g.ship);
  g.ecs.add<Rope>(g.ship);
}

int ship_def_index(const Game& g) { return static_cast<int>(g.ecs.get<Hull>(g.ship).def - SHIP_DEFS); }

void set_ship_def(Game& g, int def_index) {
  def_index = (def_index % SHIP_DEF_COUNT + SHIP_DEF_COUNT) % SHIP_DEF_COUNT;
  const ShipDef& old_def = *g.ecs.get<Hull>(g.ship).def;
  const ShipDef& new_def = SHIP_DEFS[def_index];
  g.ecs.get<Hull>(g.ship).def = &new_def;
  const Transform t = g.ecs.get<Transform>(g.ship);
  const Motion m = g.ecs.get<Motion>(g.ship);
  // Keep the feet where they were: a longer ship moves up by the difference, a shorter one down
  Vec2 pos = t.pos + rotate({0.f, leg_geom(old_def).foot_y - leg_geom(new_def).foot_y}, t.angle);
  // ... and if that still leaves it in the rock (wider feet, a nose in a low ceiling) lift it out
  const LegGeom lg = leg_geom(new_def);
  const HullGeom hg = hull_geom(new_def);
  const float need_w = std::max(new_def.half_w, lg.foot_x + lg.foot_half_w) + 4.f;
  const float top = hg.hh + 4.f, bottom = lg.foot_y + 2.f;
  for (int i = 0; i < 80 && !g.cave.is_open_box(pos.x, pos.y + 0.5f * (bottom - top), need_w, 0.5f * (bottom + top)); ++i)
    pos.y -= 4.f;
  build_bodies(g, pos, t.angle, m.vel * 0.7f, m.ang_vel * 0.5f);  // a different body: same pose, fresh physics
  g.ecs.get<Legs>(g.ship).deployed = true;
}

void place_ship(Game& g, Vec2 pos, float angle) {
  g.ecs.get<Thrusters>(g.ship) = {};
  g.ecs.get<Flight>(g.ship) = {};
  g.ecs.get<Legs>(g.ship).deployed = true;
  build_bodies(g, pos, angle, {}, 0.f);
}

void debug_rope(Game& g, float len) {
  if (!g.ecs.get<Hull>(g.ship).def->winch) return;
  Rope& r = g.ecs.get<Rope>(g.ship);
  const ShipBodies& sb = g.ecs.get<Body>(g.ship).b;
  if (B2_IS_NULL(sb.cable) || B2_IS_NULL(sb.hook)) return;
  r.length = clampf(len, rope::MIN_LEN, rope::OUT_LEN);
  r.out = len > rope::MIN_LEN;
  b2DistanceJoint_SetLengthRange(sb.cable, 0.05f, r.length / PPM);
  b2DistanceJoint_SetLength(sb.cable, r.length / PPM);
  const Transform& t = g.ecs.get<Transform>(g.ship);
  b2Body_SetTransform(sb.hook, to_b2(t.pos + rotate({0.f, winch_y(*g.ecs.get<Hull>(g.ship).def) + r.length}, t.angle)), b2MakeRot(0.f));
}

void set_winch(Game& g, bool out) {
  if (g.ecs.get<Flight>(g.ship).state == FlightState::Crashed) return;
  if (!g.ecs.get<Hull>(g.ship).def->winch) return;
  Rope& r = g.ecs.get<Rope>(g.ship);
  if (r.out == out) return;
  r.out = out;
  beep(g, out ? SimEventKind::HookOut : SimEventKind::HookIn);
}

Entity nearest_crate(Game& g, float reach) {
  Body& body = g.ecs.get<Body>(g.ship);
  Entity best = NULL_ENTITY;
  float best_d = reach;
  g.ecs.view<Cargo, Transform>([&](Entity e, Cargo& c, Transform&) {
    if (!b2Body_IsEnabled(c.body)) return;
    const b2Vec2 lp = b2Body_GetLocalPoint(c.body, b2Body_GetPosition(body.b.hook));
    const float dx = std::max(std::abs(lp.x) * PPM - c.def->half_w, 0.f);
    const float dy = std::max(std::abs(lp.y) * PPM - c.def->half_h, 0.f);
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d < best_d) { best_d = d; best = e; }
  });
  return best;
}

static const char* grip_name(int g) {
  switch (g) {
    case GRIP_MAGNET: return "MAGNET";
    case GRIP_HOOK: return "HOOK";
    case GRIP_CLAMP: return "CLAMP";
    default: return "NONE";
  }
}

// Can this ship latch this crate? Grip tier + lift capacity.
static bool can_lift(const ShipDef& ship, const CargoDef& cargo, char* why, size_t why_n) {
  if (!ship.winch || ship.grip <= GRIP_NONE) {
    if (why && why_n) std::snprintf(why, why_n, "NO WINCH");
    return false;
  }
  if (ship.grip < cargo.grip) {
    if (why && why_n) std::snprintf(why, why_n, "NEED %s", grip_name(cargo.grip));
    return false;
  }
  if (cargo.mass > ship.lift_cap + 0.001f) {
    if (why && why_n) std::snprintf(why, why_n, "TOO HEAVY");
    return false;
  }
  return true;
}

void grab_crate(Game& g, Entity best) {
  if (best == NULL_ENTITY) return;
  Rope& r = g.ecs.get<Rope>(g.ship);
  if (r.held != NULL_ENTITY) return;
  Body& body = g.ecs.get<Body>(g.ship);
  Cargo& c = g.ecs.get<Cargo>(best);
  const ShipDef& ship = *g.ecs.get<Hull>(g.ship).def;
  char why[24];
  if (!can_lift(ship, *c.def, why, sizeof why)) {
    notice(g, why);
    beep(g, SimEventKind::NoTarget);
    return;
  }
  b2RevoluteJointDef jd = b2DefaultRevoluteJointDef();  // hangs from the hook like a pendulum
  jd.bodyIdA = body.b.hook;
  jd.bodyIdB = c.body;
  jd.localAnchorA = {0.f, 0.f};
  jd.localAnchorB = b2Body_GetLocalPoint(c.body, b2Body_GetPosition(body.b.hook));
  jd.collideConnected = false;
  body.b.grip = b2CreateRevoluteJoint(g.phys.world(), &jd);
  b2RevoluteJoint_EnableMotor(body.b.grip, true);  // a motor that holds still = friction in the pivot
  b2RevoluteJoint_SetMotorSpeed(body.b.grip, 0.f);
  b2RevoluteJoint_SetMaxMotorTorque(body.b.grip, rope::GRIP_FRICTION * c.def->mass);
  b2Body_SetLinearDamping(c.body, rope::HELD_LINEAR_DAMPING);
  b2Body_SetAngularDamping(c.body, rope::HELD_ANGULAR_DAMPING);
  b2ShapeId sid;
  if (b2Body_GetShapes(c.body, &sid, 1) == 1) {
    b2Filter f = b2Shape_GetFilter(sid);
    f.maskBits &= ~CAT_SHIP;
    b2Shape_SetFilter(sid, f);
  }
  b2Body_SetAwake(c.body, true);
  r.held = best;
  if (!c.picked) stat_add(g, &Stats::cargo_picked, 1);
  c.picked = true;
  notice(g, "CARGO PICKED UP");
  beep(g, SimEventKind::Grab);
}

void toggle_grip(Game& g) {
  if (!g.ecs.get<Hull>(g.ship).def->winch) return;
  Rope& r = g.ecs.get<Rope>(g.ship);
  if (g.ecs.get<Flight>(g.ship).state == FlightState::Crashed) return;
  if (r.held != NULL_ENTITY) {  // let go
    release_crate(g);
    notice(g, "RELEASED");
    beep(g, SimEventKind::Release);
    return;
  }
  Entity best = nearest_crate(g, rope::GRAB_REACH);
  if (best == NULL_ENTITY) {
    notice(g, "NOTHING IN REACH");
    beep(g, SimEventKind::NoTarget);
    return;
  }
  grab_crate(g, best);  // prints NEED CLAMP / TOO HEAVY if refused
}

// Magnet: cable out and empty → latch a crate that drifts into AUTO_GRAB_REACH.
void auto_grab_update(Game& g) {
  if (!rope::AUTO_GRAB) return;
  const ShipDef& ship = *g.ecs.get<Hull>(g.ship).def;
  // Only magnet grippers auto-latch, and only Magnet-grade parcels
  if (!ship.winch || ship.grip != GRIP_MAGNET) return;
  Rope& r = g.ecs.get<Rope>(g.ship);
  if (!r.out || r.held != NULL_ENTITY) return;
  if (g.ecs.get<Flight>(g.ship).state == FlightState::Crashed) return;
  Entity best = NULL_ENTITY;
  float best_d = rope::AUTO_GRAB_REACH;
  Body& body = g.ecs.get<Body>(g.ship);
  if (B2_IS_NULL(body.b.hook)) return;
  g.ecs.view<Cargo, Transform>([&](Entity e, Cargo& c, Transform&) {
    if (!c.def || c.def->grip > GRIP_MAGNET) return;
    if (!can_lift(ship, *c.def, nullptr, 0)) return;
    if (!b2Body_IsEnabled(c.body)) return;
    const b2Vec2 lp = b2Body_GetLocalPoint(c.body, b2Body_GetPosition(body.b.hook));
    const float dx = std::max(std::abs(lp.x) * PPM - c.def->half_w, 0.f);
    const float dy = std::max(std::abs(lp.y) * PPM - c.def->half_h, 0.f);
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d < best_d) { best_d = d; best = e; }
  });
  if (best != NULL_ENTITY) grab_crate(g, best);
}

void toggle_legs(Game& g) {
  Legs& l = g.ecs.get<Legs>(g.ship);
  l.deployed = !l.deployed;
  beep(g, l.deployed ? SimEventKind::LegsOut : SimEventKind::LegsIn);
  b2Body_SetAwake(g.ecs.get<Body>(g.ship).b.hull, true);
}

void respawn_ship(Game& g, float wx) {
  const Cave& cave = g.cave;
  const ShipDef& d = *g.ecs.get<Hull>(g.ship).def;
  const LegGeom lg = leg_geom(d);
  const HullGeom hg = hull_geom(d);
  Vec2 pos;
  // The ship hangs from nose to feet; check an open box around that extent
  const float need_w = std::max(d.half_w, lg.foot_x + lg.foot_half_w) + 8.f;
  const float top = hg.hh + 8.f, bottom = lg.foot_y + 8.f;
  const float box_cy = 0.5f * (bottom - top), box_hh = 0.5f * (bottom + top);

  auto try_pad = [&](const LandingPad& p) {
    float cx = 0.5f * (p.x0 + p.x1);
    for (float y = p.y - lg.foot_y - 4.f; y > p.y - 320.f; y -= 8.f)
      if (cave.is_open_box(cx, y + box_cy, need_w, box_hh)) { pos = {cx, y}; return true; }
    return false;
  };

  // Prefer last visited pad, then active pad nearest wx, then any active pad, then open air near wx
  bool ok = false;
  if (g.last_pad >= 0 && g.last_pad < static_cast<int>(cave.pads.size()) &&
      cave.pads[static_cast<size_t>(g.last_pad)].active)
    ok = try_pad(cave.pads[static_cast<size_t>(g.last_pad)]);
  if (!ok) {
    int best = -1;
    float best_d = 1e12f;
    for (int i = 0; i < static_cast<int>(cave.pads.size()); ++i) {
      if (!cave.pads[static_cast<size_t>(i)].active) continue;
      float dist = std::abs(wx - 0.5f * (cave.pads[static_cast<size_t>(i)].x0 + cave.pads[static_cast<size_t>(i)].x1));
      if (dist < best_d) { best_d = dist; best = i; }
    }
    if (best >= 0) ok = try_pad(cave.pads[static_cast<size_t>(best)]);
  }
  for (size_t i = 0; i < cave.pads.size() && !ok; ++i)
    if (cave.pads[i].active) ok = try_pad(cave.pads[i]);
  for (float y = Cave::WORLD_H * 0.2f; y < Cave::WORLD_H * 0.8f && !ok; y += 16.f)
    if (cave.is_open_box(wx, y + box_cy, need_w, box_hh)) { pos = {wx, y}; ok = true; }
  if (!ok) pos = {wx, Cave::WORLD_H * 0.4f};

  g.ecs.get<Thrusters>(g.ship) = {};
  g.ecs.get<Flight>(g.ship) = {};
  g.ecs.get<Legs>(g.ship).deployed = true;
  build_bodies(g, pos, 0.f, {}, 0.f);

  // Drop leftover particles
  for (size_t i = 0; i < g.ecs.pool<Particle>().size(); ++i) g.dead.push_back(g.ecs.pool<Particle>().owner(i));
  for (Entity e : g.dead) g.ecs.destroy(e);
  g.dead.clear();
  g.events.clear();
  g.fired.clear();
}

void set_thrust(Game& g, float left, float right) {
  Thrusters& th = g.ecs.get<Thrusters>(g.ship);
  th.level[0] = left;
  th.level[1] = right;
}

void set_thrusts(Game& g, const float levels[6]) {
  Thrusters& th = g.ecs.get<Thrusters>(g.ship);
  for (int i = 0; i < 6; ++i) th.level[i] = levels[i];
}

int ship_channels(const Game& g) { return channel_count(*g.ecs.get<Hull>(g.ship).def); }

void set_zoom(Game& g, int index) { g.cam.zoom = std::clamp(index, 0, ZOOM_COUNT - 1); }

void update_view(Game& g, float dt, float aspect, bool snap) {
  Camera& c = g.cam;
  const float target = ZOOM_LEVELS[c.zoom].visible_h;
  // Glide in log space so zooming in and out feel alike
  const float k = snap ? 1.f : 1.f - std::exp(-9.f * dt);
  c.vh = std::exp(std::log(c.vh) + (std::log(target) - std::log(c.vh)) * k);
  if (std::abs(c.vh - target) < 0.25f) c.vh = target;
  c.vw = c.vh * aspect;
}

void snap_camera(Game& g) {
  const Vec2 p = ship_transform(g).pos;
  g.cam.x = p.x - g.cam.vw * 0.5f;
  g.cam.y = p.y - g.cam.vh * 0.55f;
  g.cam.shake = 0.f;
}

void reset_fog(Game& g) {
  const size_t ncells = static_cast<size_t>(Cave::GW) * Cave::GH;
  g.revealed.assign(ncells, 0);
  g.reveal_dirty = true;
  g.sonar = {};
  g.last_pad = -1;
  g.home_pad = 0;
  g.cells_explored = 0;
  g.explore_tier = 0;
  g.signals_cleared = false;
  g.pads_cleared = false;
  g.signals.clear();
  g.residues.clear();
  g.echoes.clear();
  g.wreckage.clear();
  for (LandingPad& p : g.cave.pads) { p.active = false; p.visited = false; }
}

// Scatter a few deep-cave signals in open air for the pilot to find with sonar.
void place_echoes(Game& g) {
  g.echoes.clear();
  constexpr int WANT = 14;
  for (int tries = 0; tries < 500 && static_cast<int>(g.echoes.size()) < WANT; ++tries) {
    const float x = g.rng.range(Cave::CELL * 10.f, Cave::WORLD_W - Cave::CELL * 10.f);
    const float y = g.rng.range(Cave::CELL * 10.f, Cave::WORLD_H - Cave::CELL * 10.f);
    if (g.cave.is_solid_world(x, y)) continue;
    const float ang = g.rng.range(0.f, 6.2831853f);
    const float spd = g.rng.range(12.f, 28.f);
    g.echoes.push_back({Vec2{x, y}, Vec2{std::cos(ang) * spd, std::sin(ang) * spd}, g.rng.range(0.f, 6.f), 0.f});
  }
}

void place_signals(Game& g) {
  // Signal beacons retired — cargo return-to-base is the objective.
  g.signals.clear();
  return;
  constexpr int WANT = 8;
  for (int tries = 0; tries < 400 && static_cast<int>(g.signals.size()) < WANT; ++tries) {
    const float x = g.rng.range(Cave::CELL * 8.f, Cave::WORLD_W - Cave::CELL * 8.f);
    const float y = g.rng.range(Cave::CELL * 8.f, Cave::WORLD_H - Cave::CELL * 8.f);
    if (g.cave.is_solid_world(x, y)) continue;
    // Keep away from pads so they stay distinct goals
    bool near_pad = false;
    for (const LandingPad& p : g.cave.pads) {
      const float cx = 0.5f * (p.x0 + p.x1);
      if (std::abs(x - cx) < 180.f && std::abs(y - p.y) < 120.f) { near_pad = true; break; }
    }
    if (near_pad) continue;
    g.signals.push_back({Vec2{x, y}, false});
  }
}

// Bring the pad nearest wx online and remember it as home (used at cave start).
void activate_home_pad(Game& g, float wx) {
  if (g.cave.pads.empty()) return;
  int best = 0;
  float best_d = 1e12f;
  for (int i = 0; i < static_cast<int>(g.cave.pads.size()); ++i) {
    const LandingPad& p = g.cave.pads[static_cast<size_t>(i)];
    const float d = std::abs(wx - 0.5f * (p.x0 + p.x1));
    if (d < best_d) { best_d = d; best = i; }
  }
  LandingPad& home = g.cave.pads[static_cast<size_t>(best)];
  home.active = true;
  home.visited = true;  // spawn pad is already explored
  g.last_pad = best;
  g.home_pad = best;
  // Paint a small revealed blob so the home pad shows on the chart
  const int gy = static_cast<int>(home.y / Cave::CELL);
  for (float x = home.x0 - Cave::CELL; x <= home.x1 + Cave::CELL; x += Cave::CELL * 0.5f) {
    const int gx = static_cast<int>(x / Cave::CELL);
    for (int dy = -2; dy <= 1; ++dy) {
      if (!Cave::in_grid(gx, gy + dy)) continue;
      g.revealed[static_cast<size_t>((gy + dy) * Cave::GW + gx)] = 255;
    }
  }
  g.reveal_dirty = true;
  if (g.signals.empty()) place_signals(g);
  if (g.echoes.empty()) place_echoes(g);
}

float home_pad_x(const Game& g) {
  if (g.last_pad >= 0 && g.last_pad < static_cast<int>(g.cave.pads.size())) {
    const LandingPad& p = g.cave.pads[static_cast<size_t>(g.last_pad)];
    return 0.5f * (p.x0 + p.x1);
  }
  for (const LandingPad& p : g.cave.pads)
    if (p.active) return 0.5f * (p.x0 + p.x1);
  if (!g.cave.pads.empty())
    return 0.5f * (g.cave.pads[0].x0 + g.cave.pads[0].x1);
  return Cave::WORLD_W * 0.5f;
}

// Teleport to a specific pad index (must be visited). Returns false if invalid.
bool teleport_pad(Game& g, int pad_index) {
  if (g.ecs.get<Flight>(g.ship).state != FlightState::Landed) {
    notice(g, "LAND FIRST");
    return false;
  }
  if (pad_index < 0 || pad_index >= static_cast<int>(g.cave.pads.size())) return false;
  const LandingPad& pad = g.cave.pads[static_cast<size_t>(pad_index)];
  if (!pad.visited) {
    notice(g, "PAD UNKNOWN");
    return false;
  }
  g.last_pad = pad_index;
  respawn_ship(g, 0.5f * (pad.x0 + pad.x1));
  snap_camera(g);
  return true;
}

// Teleport between visited (explored) pads. delta = +1 next, -1 previous.
bool cycle_pad(Game& g, int delta) {
  if (g.ecs.get<Flight>(g.ship).state != FlightState::Landed) {
    notice(g, "LAND FIRST");
    return false;
  }
  std::vector<int> visited;
  visited.reserve(g.cave.pads.size());
  for (int i = 0; i < static_cast<int>(g.cave.pads.size()); ++i)
    if (g.cave.pads[static_cast<size_t>(i)].visited) visited.push_back(i);
  if (visited.size() < 2) {
    notice(g, visited.empty() ? "NO PADS" : "ONLY ONE PAD");
    return false;
  }
  int cur = -1;
  if (g.last_pad >= 0) {
    for (int i = 0; i < static_cast<int>(visited.size()); ++i)
      if (visited[static_cast<size_t>(i)] == g.last_pad) { cur = i; break; }
  }
  if (cur < 0) {
    const float sx = ship_transform(g).pos.x;
    float best_d = 1e12f;
    for (int i = 0; i < static_cast<int>(visited.size()); ++i) {
      const LandingPad& p = g.cave.pads[static_cast<size_t>(visited[static_cast<size_t>(i)])];
      const float d = std::abs(0.5f * (p.x0 + p.x1) - sx);
      if (d < best_d) { best_d = d; cur = i; }
    }
  }
  const int n = static_cast<int>(visited.size());
  const int next = (cur + delta % n + n) % n;
  const int pi = visited[static_cast<size_t>(next)];
  if (!teleport_pad(g, pi)) return false;
  char buf[32];
  std::snprintf(buf, sizeof buf, "PAD %d/%d", next + 1, n);
  notice(g, buf);
  return true;
}

void fire_sonar(Game& g) {
  if (g.sonar.active || g.sonar_cool > 0.f) return;
  Flight& fl = g.ecs.get<Flight>(g.ship);
  if (fl.state == FlightState::Crashed) return;
  if (fl.fuel > 0.f)
    fl.fuel = std::max(0.f, fl.fuel - tune::FUEL_SONAR);
  const Vec2 p = ship_transform(g).pos;
  SonarPing s;
  s.active = true;
  s.fading = false;
  s.origin = p;
  s.radius = 0.f;
  s.prev_radius = 0.f;
  s.max_radius = tune::SONAR_MAX_RADIUS;
  s.speed = tune::SONAR_SPEED;
  s.fade = 1.f;
  s.pad_hit.assign(g.cave.pads.size(), 0);
  s.signal_hit.assign(g.signals.size(), 0);
  g.sonar = std::move(s);
  g.sonar_cool = tune::SONAR_COOLDOWN;
  g.events.push_back({SimEventKind::SonarPing, p, {}, 200.f});
}

// Explored fraction of *open air* cells only — solid rock is not explorable.
int explore_percent(const Game& g) {
  if (g.revealed.empty() || g.cave.solid.empty()) return 0;
  const size_t n = g.revealed.size();
  if (n != g.cave.solid.size()) return 0;
  int open_total = 0, open_lit = 0;
  for (size_t i = 0; i < n; ++i) {
    if (g.cave.solid[i]) continue;
    ++open_total;
    if (g.revealed[i] >= 80) ++open_lit;
  }
  return open_total > 0 ? (open_lit * 100) / open_total : 0;
}

void check_exploration_milestones(Game& g) {
  if (g.revealed.empty()) return;
  const int pct = explore_percent(g);
  static constexpr int TIERS[] = {25, 50, 75, 100};
  for (int t = g.explore_tier; t < 4; ++t) {
    if (pct < TIERS[t]) break;
    g.explore_tier = t + 1;
    g.score += tune::SCORE_MILESTONE;
    char buf[32];
    std::snprintf(buf, sizeof buf, "MAP %d%%", TIERS[t]);
    notice(g, buf);
  }
  if (false && !g.signals_cleared && !g.signals.empty()) {
    bool all = true;
    for (const Game::Signal& s : g.signals)
      if (!s.found) { all = false; break; }
    if (all) {
      g.signals_cleared = true;
      g.score += tune::SCORE_MILESTONE * 2;
      notice(g, "ALL SIGNALS");
    }
  }
  if (!g.pads_cleared && !g.cave.pads.empty()) {
    bool all = true;
    for (const LandingPad& p : g.cave.pads)
      if (!p.active) { all = false; break; }
    if (all) {
      g.pads_cleared = true;
      g.score += tune::SCORE_MILESTONE * 2;
      notice(g, "ALL PADS");
    }
  }
}

// Activate any pad whose deck cells passive explore (or a sonar tag blob) has revealed.
void discover_pads(Game& g) {
  for (LandingPad& p : g.cave.pads) {
    if (p.active) continue;
    const int gy = static_cast<int>(p.y / Cave::CELL);
    for (float x = p.x0; x <= p.x1; x += Cave::CELL * 0.5f) {
      const int gx = static_cast<int>(x / Cave::CELL);
      if (!Cave::in_grid(gx, gy)) continue;
      const size_t i = static_cast<size_t>(gy * Cave::GW + gx);
      if (i < g.revealed.size() && g.revealed[i] >= 80) {
        p.active = true;
        notice(g, "PAD ONLINE");
        break;
      }
      if (Cave::in_grid(gx, gy - 1)) {
        const size_t j = static_cast<size_t>((gy - 1) * Cave::GW + gx);
        if (j < g.revealed.size() && g.revealed[j] >= 80) {
          p.active = true;
          notice(g, "PAD ONLINE");
          break;
        }
      }
    }
  }
}

// Paint a small permanent chart blob around a world point (sonar tag / home pad).
static void paint_blob(Game& g, float wx, float wy, int radius_cells, uint8_t str) {
  const int cgx = static_cast<int>(wx / Cave::CELL);
  const int cgy = static_cast<int>(wy / Cave::CELL);
  bool painted = false;
  for (int dy = -radius_cells; dy <= radius_cells; ++dy)
    for (int dx = -radius_cells; dx <= radius_cells; ++dx) {
      if (dx * dx + dy * dy > radius_cells * radius_cells) continue;
      const int gx = cgx + dx, gy = cgy + dy;
      if (!Cave::in_grid(gx, gy)) continue;
      const size_t i = static_cast<size_t>(gy * Cave::GW + gx);
      if (i >= g.revealed.size()) continue;
      if (str > g.revealed[i]) {
        if (g.revealed[i] == 0) g.cells_explored += 1;
        g.revealed[i] = str;
        painted = true;
      }
    }
  if (painted) g.reveal_dirty = true;
}

// Passive minimap uncover: a circular region around the ship, LOS-limited like the old sonar.
void update_explore(Game& g, float dt) {
  (void)dt;
  if (!tune::PASSIVE_EXPLORE) return;
  if (g.revealed.empty()) return;
  if (g.ecs.get<Flight>(g.ship).state == FlightState::Crashed) return;
  const Vec2 o = ship_transform(g).pos;
  const float R = tune::EXPLORE_RADIUS;
  const float full = R * tune::EXPLORE_FADE;
  const float pad = Cave::CELL * 2.f;
  const int gx0 = std::max(0, static_cast<int>((o.x - R - pad) / Cave::CELL));
  const int gx1 = std::min(Cave::GW - 1, static_cast<int>((o.x + R + pad) / Cave::CELL));
  const int gy0 = std::max(0, static_cast<int>((o.y - R - pad) / Cave::CELL));
  const int gy1 = std::min(Cave::GH - 1, static_cast<int>((o.y + R + pad) / Cave::CELL));

  auto strength_at = [&](float d) -> uint8_t {
    if (d <= full) return 255;
    if (d >= R) return 0;
    const float u = (d - full) / (R - full);
    return static_cast<uint8_t>(255.f * (1.f - u) + 40.f * u);
  };

  // Soft LOS: a couple of solid cells may be punched so thin pillars do not block the whole sector.
  auto clear_path = [&](int tgx, int tgy) -> bool {
    const float tx = (tgx + 0.5f) * Cave::CELL, ty = (tgy + 0.5f) * Cave::CELL;
    const float dx = tx - o.x, dy = ty - o.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < 1.f) return true;
    const int steps = std::max(2, static_cast<int>(dist / (Cave::CELL * 0.4f)));
    int solid_budget = 2;
    for (int i = 1; i <= steps; ++i) {
      const float u = static_cast<float>(i) / static_cast<float>(steps);
      const int gx = static_cast<int>((o.x + dx * u) / Cave::CELL);
      const int gy = static_cast<int>((o.y + dy * u) / Cave::CELL);
      if (gx == tgx && gy == tgy) return true;
      if (g.cave.is_solid_cell(gx, gy)) {
        if (--solid_budget < 0) return false;
      }
    }
    return true;
  };

  auto rock_depth = [&](int gx, int gy) -> int {
    if (!g.cave.is_solid_cell(gx, gy)) return 0;
    int best = 8;
    for (int r = 1; r <= 2; ++r) {
      for (int oy = -r; oy <= r; ++oy)
        for (int ox = -r; ox <= r; ++ox) {
          if (std::abs(ox) != r && std::abs(oy) != r) continue;
          if (!g.cave.is_solid_cell(gx + ox, gy + oy)) best = std::min(best, r);
        }
      if (best <= r) break;
    }
    return best;
  };

  bool painted = false;
  int new_cells = 0;
  for (int gy = gy0; gy <= gy1; ++gy) {
    for (int gx = gx0; gx <= gx1; ++gx) {
      const float cx = (gx + 0.5f) * Cave::CELL;
      const float cy = (gy + 0.5f) * Cave::CELL;
      const float dx = cx - o.x, dy = cy - o.y;
      const float d = std::sqrt(dx * dx + dy * dy);
      if (d > R) continue;
      if (!clear_path(gx, gy)) continue;
      const uint8_t str = strength_at(d);
      if (str == 0) continue;
      const size_t i = static_cast<size_t>(gy * Cave::GW + gx);
      if (!g.cave.is_solid_cell(gx, gy)) {
        if (str > g.revealed[i]) {
          if (g.revealed[i] == 0) ++new_cells;
          g.revealed[i] = str;
          painted = true;
        }
        continue;
      }
      const int depth = rock_depth(gx, gy);
      if (depth >= 1 && depth <= 2 && str > g.revealed[i]) {
        if (g.revealed[i] == 0) ++new_cells;
        g.revealed[i] = str;
        painted = true;
      }
    }
  }
  if (painted) {
    g.reveal_dirty = true;
    discover_pads(g);
    if (new_cells > 0) {
      const int bonus = std::min(new_cells, 12);
      g.score += bonus * tune::SCORE_REVEAL_CELL;
      g.cells_explored += new_cells;
      check_exploration_milestones(g);
    }
  }
}

void update_sonar(Game& g, float dt) {
  if (!g.sonar.active) return;
  SonarPing& s = g.sonar;
  const tune::SonarModeDef& mode = tune::sonar_mode();

  // Age reflection pulses (drawn even while fading)
  for (SonarReflection& e : s.echoes) e.age += dt;

  // Ring always keeps expanding; past max_radius it only fades (never freezes in mid-air)
  s.prev_radius = s.radius;
  s.radius += s.speed * dt;
  if (s.radius >= s.max_radius) {
    s.fading = true;
    const float fade_t = std::max(0.05f, tune::SONAR_FADE_TIME);
    s.fade = std::max(0.f, s.fade - dt / fade_t);
  } else {
    s.fading = false;
    s.fade = 1.f;
  }
  if (s.fade <= 0.02f) {
    s.active = false;
    s.fading = false;
    s.fade = 0.f;
    s.echoes.clear();
    return;
  }

  // Paint / reflect only while the wavefront is still sweeping the useful range
  if (s.prev_radius >= s.max_radius) return;
  const float r0 = s.prev_radius, r1 = std::min(s.radius, s.max_radius);

  auto clear_path = [&](int tgx, int tgy) -> bool {
    const float tx = (tgx + 0.5f) * Cave::CELL, ty = (tgy + 0.5f) * Cave::CELL;
    const float dx = tx - s.origin.x, dy = ty - s.origin.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < 1.f) return true;
    const int steps = std::max(2, static_cast<int>(dist / (Cave::CELL * 0.4f)));
    int solid_budget = 2;
    for (int i = 1; i <= steps; ++i) {
      const float u = static_cast<float>(i) / static_cast<float>(steps);
      const int gx = static_cast<int>((s.origin.x + dx * u) / Cave::CELL);
      const int gy = static_cast<int>((s.origin.y + dy * u) / Cave::CELL);
      if (gx == tgx && gy == tgy) return true;
      if (g.cave.is_solid_cell(gx, gy)) {
        if (--solid_budget < 0) return false;
      }
    }
    return true;
  };

  auto los_world = [&](float tx, float ty) -> bool {
    const float dx = tx - s.origin.x, dy = ty - s.origin.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < 1.f) return true;
    const int steps = std::max(2, static_cast<int>(dist / (Cave::CELL * 0.45f)));
    int solid_budget = 2;
    for (int i = 1; i < steps; ++i) {
      const float u = static_cast<float>(i) / static_cast<float>(steps);
      if (g.cave.is_solid_world(s.origin.x + dx * u, s.origin.y + dy * u)) {
        if (--solid_budget < 0) return false;
      }
    }
    return true;
  };

  // --- Classic path: annulus paints the fog map (PAINT / BOTH) ---
  if (mode.paint_fog && !g.revealed.empty()) {
    const float pad = Cave::CELL * 2.f;
    const int gx0 = std::max(0, static_cast<int>((s.origin.x - r1 - pad) / Cave::CELL));
    const int gx1 = std::min(Cave::GW - 1, static_cast<int>((s.origin.x + r1 + pad) / Cave::CELL));
    const int gy0 = std::max(0, static_cast<int>((s.origin.y - r1 - pad) / Cave::CELL));
    const int gy1 = std::min(Cave::GH - 1, static_cast<int>((s.origin.y + r1 + pad) / Cave::CELL));
    auto strength_at = [&](float d) -> uint8_t {
      const float full = s.max_radius * 0.75f;
      if (d <= full) return 255;
      if (d >= s.max_radius) return 40;
      const float u = (d - full) / (s.max_radius - full);
      return static_cast<uint8_t>(255.f * (1.f - u) + 40.f * u);
    };
    auto rock_depth = [&](int gx, int gy) -> int {
      if (!g.cave.is_solid_cell(gx, gy)) return 0;
      int best = 8;
      for (int r = 1; r <= 2; ++r) {
        for (int oy = -r; oy <= r; ++oy)
          for (int ox = -r; ox <= r; ++ox) {
            if (std::abs(ox) != r && std::abs(oy) != r) continue;
            if (!g.cave.is_solid_cell(gx + ox, gy + oy)) best = std::min(best, r);
          }
        if (best <= r) break;
      }
      return best;
    };
    bool painted = false;
    int new_cells = 0;
    auto mark = [&](int gx, int gy, uint8_t str, bool residue = false) {
      if (!Cave::in_grid(gx, gy) || str == 0) return;
      const size_t i = static_cast<size_t>(gy * Cave::GW + gx);
      if (i >= g.revealed.size()) return;
      const uint8_t old = g.revealed[i];
      uint8_t next = str;
      if (old > 0 && old < 255) {
        const int boosted = static_cast<int>(old) + static_cast<int>(str) / 3 + 8;
        next = static_cast<uint8_t>(std::min(255, std::max(static_cast<int>(str), boosted)));
      }
      if (next > old) {
        if (old == 0) ++new_cells;
        g.revealed[i] = next;
        painted = true;
      }
      if (residue && g.cave.is_solid_cell(gx, gy) && next >= 80 && g.residues.size() < 120)
        g.residues.push_back({Vec2{(gx + 0.5f) * Cave::CELL, (gy + 0.5f) * Cave::CELL}, tune::RESIDUE_TTL});
    };
    for (int gy = gy0; gy <= gy1; ++gy) {
      for (int gx = gx0; gx <= gx1; ++gx) {
        const float cx = (gx + 0.5f) * Cave::CELL, cy = (gy + 0.5f) * Cave::CELL;
        const float dx = cx - s.origin.x, dy = cy - s.origin.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d < r0 || d >= r1) continue;
        if (!clear_path(gx, gy)) continue;
        const uint8_t str = strength_at(d);
        if (!g.cave.is_solid_cell(gx, gy)) {
          mark(gx, gy, str);
          continue;
        }
        const int depth = rock_depth(gx, gy);
        if (depth >= 1 && depth <= 2) mark(gx, gy, str, true);
      }
    }
    g.ecs.view<Cargo, Transform>([&](Entity, const Cargo&, const Transform& tf) {
      const float dx = tf.pos.x - s.origin.x, dy = tf.pos.y - s.origin.y;
      const float d = std::sqrt(dx * dx + dy * dy);
      if (d < r0 || d >= r1) return;
      const int gx = static_cast<int>(tf.pos.x / Cave::CELL);
      const int gy = static_cast<int>(tf.pos.y / Cave::CELL);
      const uint8_t str = strength_at(d);
      for (int oy = -1; oy <= 1; ++oy)
        for (int ox = -1; ox <= 1; ++ox) mark(gx + ox, gy + oy, str);
    });
    if (painted) {
      g.reveal_dirty = true;
      discover_pads(g);
      if (new_cells > 0) {
        g.score += std::min(new_cells, 40) * tune::SCORE_REVEAL_CELL;
        g.cells_explored += new_cells;
        check_exploration_milestones(g);
      }
    }
  }

  // --- Reflect path: wavefront hits pads / cargo / signals (REFLECT / BOTH) ---
  if (mode.reflect_targets) {
    auto add_echo = [&](float tx, float ty, SonarReflection::Kind kind) {
      const float dx = tx - s.origin.x, dy = ty - s.origin.y;
      const float d = std::sqrt(dx * dx + dy * dy);
      if (d < r0 || d >= r1 || d > s.max_radius) return false;
      if (!los_world(tx, ty)) return false;
      SonarReflection e;
      e.angle = std::atan2(dy, dx);
      e.hit_r = d;
      e.age = 0.f;
      e.life = 1.4f;
      e.kind = kind;
      s.echoes.push_back(e);
      return true;
    };

    if (s.pad_hit.size() != g.cave.pads.size()) s.pad_hit.assign(g.cave.pads.size(), 0);
    for (int i = 0; i < static_cast<int>(g.cave.pads.size()); ++i) {
      if (s.pad_hit[static_cast<size_t>(i)]) continue;
      LandingPad& pad = g.cave.pads[static_cast<size_t>(i)];
      const float cx = 0.5f * (pad.x0 + pad.x1);
      const float cy = pad.y - 4.f;
      if (!add_echo(cx, cy, SonarReflection::Kind::Pad)) continue;
      s.pad_hit[static_cast<size_t>(i)] = 1;
      if (mode.activate_pad_on_hit && !pad.active) {
        pad.active = true;
        notice(g, "PAD PING");
      }
      if (mode.tag_blob_on_hit) paint_blob(g, cx, pad.y, 3, 255);
      g.events.push_back({SimEventKind::SonarPing, {cx, cy}, {}, 80.f});
    }

    g.ecs.view<Cargo, Transform>([&](Entity e, const Cargo&, const Transform& tf) {
      for (Entity h : s.cargo_hit)
        if (h == e) return;
      if (!add_echo(tf.pos.x, tf.pos.y, SonarReflection::Kind::Cargo)) return;
      s.cargo_hit.push_back(e);
      if (mode.tag_blob_on_hit) paint_blob(g, tf.pos.x, tf.pos.y, 2, 220);
      notice(g, "CARGO PING");
      g.events.push_back({SimEventKind::SonarPing, tf.pos, {}, 60.f});
    });

    if (s.signal_hit.size() != g.signals.size()) s.signal_hit.assign(g.signals.size(), 0);
    for (int i = 0; i < static_cast<int>(g.signals.size()); ++i) {
      if (s.signal_hit[static_cast<size_t>(i)]) continue;
      Game::Signal& sig = g.signals[static_cast<size_t>(i)];
      if (!add_echo(sig.pos.x, sig.pos.y, SonarReflection::Kind::Signal)) continue;
      s.signal_hit[static_cast<size_t>(i)] = 1;
      if (!sig.found) {
        sig.found = true;
        /* signals no longer score */ (void)0;
        notice(g, "+75 SIGNAL");
        check_exploration_milestones(g);
      }
      if (mode.tag_blob_on_hit) paint_blob(g, sig.pos.x, sig.pos.y, 2, 255);
      if (g.residues.size() < 120)
        g.residues.push_back({sig.pos, tune::RESIDUE_TTL * 1.5f});
    }
  }

  // Ambient cave life chirps when the wavefront passes (any mode)
  for (Game::Echo& echo : g.echoes) {
    if (echo.cool > 0.f) continue;
    const float dx = echo.pos.x - s.origin.x, dy = echo.pos.y - s.origin.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d < r0 || d >= r1) continue;
    echo.cool = 8.f;
    /* echoes no longer score */ (void)0;
    if (g.residues.size() < 120)
      g.residues.push_back({echo.pos, tune::RESIDUE_TTL * 1.2f});
  }
}

void step_sim(Game& g, float dt) {
  g.time += dt;
  g.notice_timer = std::max(0.f, g.notice_timer - dt / tune::TIME_SCALE);
  g.sonar_cool = std::max(0.f, g.sonar_cool - dt);
  for (size_t i = 0; i < g.residues.size();) {
    g.residues[i].life -= dt;
    if (g.residues[i].life <= 0.f) {
      g.residues[i] = g.residues.back();
      g.residues.pop_back();
    } else
      ++i;
  }
  // Passive cue: near an unfound signal, leave a faint residue so the pilot can home in
  {
    const Vec2 sp = ship_transform(g).pos;
    for (const Game::Signal& sig : g.signals) {
      if (sig.found) continue;
      const float dx = sig.pos.x - sp.x, dy = sig.pos.y - sp.y;
      const float d = std::sqrt(dx * dx + dy * dy);
      if (d > tune::SIGNAL_PROX) continue;
      if (g.residues.size() < 120 && std::fmod(g.time + d * 0.01f, 0.45f) < dt)
        g.residues.push_back({sig.pos, 0.5f});
    }
  }
  // Ambient life drifts slowly through open air
  for (Game::Echo& e : g.echoes) {
    e.cool = std::max(0.f, e.cool - dt);
    e.phase += dt;
    e.pos.x += e.vel.x * dt;
    e.pos.y += e.vel.y * dt;
    // Bounce off rock / world bounds
    if (e.pos.x < Cave::CELL * 4.f || e.pos.x > Cave::WORLD_W - Cave::CELL * 4.f) e.vel.x = -e.vel.x;
    if (e.pos.y < Cave::CELL * 4.f || e.pos.y > Cave::WORLD_H - Cave::CELL * 4.f) e.vel.y = -e.vel.y;
    if (g.cave.is_solid_world(e.pos.x, e.pos.y)) {
      e.vel.x = -e.vel.x;
      e.vel.y = -e.vel.y;
      e.pos.x += e.vel.x * dt * 2.f;
      e.pos.y += e.vel.y * dt * 2.f;
    }
    // Gentle wander
    if (std::fmod(e.phase, 3.f) < dt) {
      const float ang = g.rng.range(0.f, 6.2831853f);
      const float spd = g.rng.range(12.f, 28.f);
      e.vel = {std::cos(ang) * spd, std::sin(ang) * spd};
    }
  }
  // Ground is needed around the ship and around every crate that is moving
  static std::vector<Vec2> anchors;
  anchors.assign(1, ship_transform(g).pos);
  g.ecs.view<Cargo, Transform>([&](Entity, Cargo& c, Transform& t) {
    if (b2Body_IsEnabled(c.body) && b2Body_IsAwake(c.body)) anchors.push_back(t.pos);
  });
  g.phys.stream(g.cave, anchors.data(), static_cast<int>(anchors.size()));
  cargo_activation(g);
  rope_system(g, dt);
  forces_system(g);
  g.phys.step(dt);
  sync_system(g);
  cargo_system(g, dt);
  auto_grab_update(g);
  impact_system(g);
  ground_system(g, dt);
  event_system(g);
  exhaust_system(g, dt);
  particle_system(g, dt);
  wreckage_system(g, dt);
  update_explore(g, dt);
  update_sonar(g, dt);
}

void update_camera(Game& g, float dt) {
  Camera& c = g.cam;
  const Transform& t = ship_transform(g);
  const Motion& m = g.ecs.get<Motion>(g.ship);
  // Look ahead a little in the direction of travel
  const Vec2 lead = {clampf(m.vel.x * 0.35f, -180.f, 180.f), clampf(m.vel.y * 0.25f, -120.f, 120.f)};
  const float k = 1.f - std::exp(-5.f * dt);
  c.x += (t.pos.x + lead.x - c.vw * 0.5f - c.x) * k;
  c.y += (t.pos.y + lead.y - c.vh * 0.55f - c.y) * k;
  c.x = clampf(c.x, -100.f, Cave::WORLD_W - c.vw + 100.f);  // the rock ring may show, not the void beyond it
  c.y = clampf(c.y, -100.f, Cave::WORLD_H - c.vh * 0.3f);

  c.shake = std::max(0.f, c.shake - dt * 2.2f);
  c.shake_off = c.shake > 0.f ? Vec2{g.rng.range(-1.f, 1.f), g.rng.range(-1.f, 1.f)} * (c.shake * c.shake * 14.f)
                              : Vec2{};
}
