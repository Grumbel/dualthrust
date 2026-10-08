// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Headless checks of the rigid-body game: landing, lift-off, crashes, retracting legs, the map edges and ground
// that holds. Needs no display: `ctest` or run the binary directly.

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "systems.hpp"

namespace {

int failures = 0;
#define CHECK(cond, ...)                                          \
  do {                                                            \
    if (!(cond)) {                                                \
      ++failures;                                                 \
      std::printf("FAIL %s:%d: %s  --  ", __FILE__, __LINE__, #cond); \
      std::printf(__VA_ARGS__);                                   \
      std::printf("\n");                                          \
    }                                                             \
  } while (0)

constexpr float DT = tune::SIM_STEP * tune::TIME_SCALE;

void run(Game& g, float real_seconds) {
  for (int i = 0, n = static_cast<int>(real_seconds / tune::SIM_STEP); i < n; ++i) step_sim(g, DT);
  g.fired.insert(g.fired.end(), g.events.begin(), g.events.end());
}

Flight& flight(Game& g) { return g.ecs.get<Flight>(g.ship); }
Transform& xf(Game& g) { return g.ecs.get<Transform>(g.ship); }
Motion& mo(Game& g) { return g.ecs.get<Motion>(g.ship); }

// Put the whole ship somewhere (hull and legs move together), with a velocity
void teleport(Game& g, Vec2 pos, Vec2 vel) {
  const ShipBodies& sb = g.ecs.get<Body>(g.ship).b;
  const b2Vec2 d = b2Sub(to_b2(pos), b2Body_GetPosition(sb.hull));
  for (b2BodyId b : {sb.hull, sb.leg[0], sb.leg[1]}) {
    b2Body_SetTransform(b, b2Add(b2Body_GetPosition(b), d), b2Body_GetRotation(b));
    b2Body_SetLinearVelocity(b, to_b2(vel));
    b2Body_SetAngularVelocity(b, 0.f);
    b2Body_SetAwake(b, true);  // a sleeping body stays asleep when moved
  }
  xf(g).pos = pos;
}

struct Fixture {
  Game g;
  Fixture(unsigned seed = 0xC0FFEE, int ship = DEFAULT_SHIP) {
    g.stats_enabled = false;
    g.cave.generate(seed);
    create_ship(g, ship);
    respawn_ship(g, 0.5f * (g.cave.pads[0].x0 + g.cave.pads[0].x1));
  }
};

void test_settle_and_lift_off() {
  Fixture f;
  run(f.g, 4.f);
  CHECK(flight(f.g).state == FlightState::Landed, "state %d", static_cast<int>(flight(f.g).state));
  const float rest_y = xf(f.g).pos.y;
  CHECK(std::abs(mo(f.g).vel.y) < 5.f, "vy %.1f", mo(f.g).vel.y);
  set_thrust(f.g, 0.7f, 0.7f);
  run(f.g, 0.5f);
  CHECK(flight(f.g).state == FlightState::Flying, "still landed under thrust");
  CHECK(xf(f.g).pos.y < rest_y - 10.f, "rose only %.1f px", rest_y - xf(f.g).pos.y);
  set_thrust(f.g, 0.f, 0.f);
  run(f.g, 12.f);  // coasts up a little, falls back onto the legs
  CHECK(flight(f.g).state == FlightState::Landed, "no gentle return: state %d", static_cast<int>(flight(f.g).state));
}

void test_crash_and_soft_drop() {
  Fixture f;
  run(f.g, 3.f);
  // as high as the shaft allows (at most 800 px), thrown downward so even a short shaft is past the feet's limit
  float height = 0.f;
  while (height < 800.f && f.g.cave.is_open_box(xf(f.g).pos.x, xf(f.g).pos.y - height - 16.f, 50.f, 40.f)) height += 16.f;
  CHECK(height >= 160.f, "shaft only %.0f px tall", height);
  teleport(f.g, xf(f.g).pos + Vec2{0.f, -height}, {0.f, 150.f});
  run(f.g, 8.f);
  CHECK(flight(f.g).state == FlightState::Crashed, "fell %.0f px and survived", height);

  Fixture h;
  run(h.g, 3.f);
  teleport(h.g, xf(h.g).pos + Vec2{0.f, -30.f}, {});
  run(h.g, 6.f);
  CHECK(flight(h.g).state == FlightState::Landed, "30 px drop: state %d", static_cast<int>(flight(h.g).state));
}

void test_legs_retract() {
  Fixture f;
  run(f.g, 4.f);
  const float y_out = xf(f.g).pos.y;
  toggle_legs(f.g);
  run(f.g, 4.f);
  const Legs& l = f.g.ecs.get<Legs>(f.g.ship);
  const float travel = leg_geom(*f.g.ecs.get<Hull>(f.g.ship).def).travel;
  CHECK(!l.deployed && l.trans[0] < -travel * 0.8f && l.trans[1] < -travel * 0.8f, "legs at %.1f/%.1f of %.1f", l.trans[0], l.trans[1], travel);
  CHECK(xf(f.g).pos.y > y_out + 8.f, "hull did not come down: %.1f -> %.1f", y_out, xf(f.g).pos.y);
  toggle_legs(f.g);
  set_thrust(f.g, 0.6f, 0.6f);
  run(f.g, 1.5f);
  set_thrust(f.g, 0.f, 0.f);
  CHECK(l.deployed && l.trans[0] > -3.f, "legs did not extend again (%.1f)", l.trans[0]);
}

// Switching ships on the pad: the new ship must stand on its feet, never sink into the ground
void test_ship_switch() {
  Fixture f(0xC0FFEE, 11);  // Gnat, the smallest
  run(f.g, 2.f);
  int bad = 0;
  for (int to : {9, 12, 0, 17, 13, 4, 15, 8, 11, 7}) {  // Spire, Orca, ... long and wide ships after small ones
    set_ship_def(f.g, to);
    const Vec2 p = xf(f.g).pos;
    CHECK(!f.g.cave.is_solid_world(p.x, p.y), "ship %d centre in rock right after switching", to);
    run(f.g, 3.f);
    const Vec2 q = xf(f.g).pos;
    if (f.g.cave.is_solid_world(q.x, q.y) || flight(f.g).state == FlightState::Crashed || std::abs(q.y - p.y) > 40.f) ++bad;
    CHECK(flight(f.g).state == FlightState::Landed, "ship %d did not settle after switching (state %d)", to, static_cast<int>(flight(f.g).state));
  }
  CHECK(bad == 0, "%d switches ended badly", bad);
}

// The big ships with four thrusters: each control channel must push the ship the way its thrusters point
void test_big_ships() {
  for (int ship = 0; ship < SHIP_DEF_COUNT; ++ship) {
    const ShipDef& d = SHIP_DEFS[ship];
    if (!d.thruster_n) continue;
    Fixture f(0xC0FFEE, ship);
    run(f.g, 3.f);
    CHECK(flight(f.g).state == FlightState::Landed, "%s did not settle", d.name);
    CHECK(ship_channels(f.g) == 4, "%s has %d channels", d.name, ship_channels(f.g));
    const Vec2 base = xf(f.g).pos;
    float height = 0.f;
    while (height < 200.f && f.g.cave.is_open_box(base.x, base.y - height - 16.f, 120.f, 60.f)) height += 16.f;
    for (int ch = 0; ch < 4; ++ch) {
      Vec2 expect{};
      for (int i = 0; i < d.thruster_n; ++i) {
        const ThrusterPose tp = thruster_pose(d, i);
        if (tp.channel == ch) expect += tp.push * tp.power;
      }
      auto fly = [&](float level) {  // a short free-flight run from the same spot, returns the velocity
        flight(f.g) = {};
        teleport(f.g, base + Vec2{0.f, -height}, {});
        float lv[4] = {0.f, 0.f, 0.f, 0.f};
        lv[ch] = level;
        set_thrusts(f.g, lv);
        run(f.g, 0.25f);
        set_thrusts(f.g, std::array<float, 4>{}.data());
        return mo(f.g).vel;
      };
      const Vec2 dv = fly(1.f) - fly(0.f);
      CHECK(dot(dv, expect) > 10.f, "%s channel %d moved the ship (%.0f,%.0f), expected toward (%.2f,%.2f)", d.name, ch, dv.x,
            dv.y, expect.x, expect.y);
    }
    // all four channels are independent in the HUD data: levels are stored per channel
    float lv[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    set_thrusts(f.g, lv);
    const Thrusters& th = f.g.ecs.get<Thrusters>(f.g.ship);
    CHECK(th.level[2] == 0.3f && th.level[3] == 0.4f, "levels not stored per channel");
  }
}

Entity first_cargo(Game& g) {
  Entity found = NULL_ENTITY;
  g.ecs.view<Cargo>([&](Entity e, Cargo&) { if (found == NULL_ENTITY) found = e; });
  return found;
}

void put_body(b2BodyId b, Vec2 pos) {
  b2Body_SetTransform(b, to_b2(pos), b2MakeRot(0.f));
  b2Body_SetLinearVelocity(b, {0.f, 0.f});
  b2Body_SetAngularVelocity(b, 0.f);
  b2Body_SetAwake(b, true);
}

void test_cargo_and_rope() {
  Fixture f;
  f.g.stats_enabled = true;
  run(f.g, 3.f);
  int total = 0, frozen = 0;
  f.g.ecs.view<Cargo>([&](Entity, Cargo& c) { ++total; frozen += !b2Body_IsEnabled(c.body); });
  CHECK(total == 12, "%d crates", total);
  CHECK(frozen >= total - 3, "only %d of %d crates frozen far from the ship", frozen, total);

  // A crate beside the ship on its pad
  const Entity ce = first_cargo(f.g);
  Cargo& c = f.g.ecs.get<Cargo>(ce);
  const Vec2 sp = xf(f.g).pos;
  const float rest_y = sp.y + leg_geom(*f.g.ecs.get<Hull>(f.g.ship).def).foot_y - c.def->half_h - 1.f;
  b2Body_Enable(c.body);
  put_body(c.body, {sp.x + 90.f, rest_y});
  f.g.ecs.get<Transform>(ce).pos = {sp.x + 90.f, rest_y};
  run(f.g, 1.5f);
  const float y0 = f.g.ecs.get<Transform>(ce).pos.y;
  CHECK(std::abs(y0 - rest_y) < 12.f, "crate did not rest on the pad (%.1f vs %.1f)", y0, rest_y);

  // Pay out some cable, put the hook on the crate and grab it
  set_winch(f.g, true);
  run(f.g, 5.f);
  const Rope& r = f.g.ecs.get<Rope>(f.g.ship);
  CHECK(r.length == rope::OUT_LEN, "cable did not deploy fully (%.0f)", r.length);
  set_winch(f.g, false);
  run(f.g, 5.f);
  CHECK(r.length == rope::MIN_LEN, "cable did not retract fully (%.0f)", r.length);
  const ShipBodies& sb = f.g.ecs.get<Body>(f.g.ship).b;
  put_body(sb.hook, f.g.ecs.get<Transform>(ce).pos + Vec2{0.f, -c.def->half_h + 2.f});
  toggle_grip(f.g);
  CHECK(r.held == ce && c.picked, "grab failed");
  CHECK(f.g.stats.cargo_picked == 1, "pick-up not counted");

  // Lift off with the crate: first the slack of the fully paid-out cable, then the crate comes up
  set_thrust(f.g, 0.45f, 0.45f);
  float max_tilt = 0.f;
  for (int i = 0; i < static_cast<int>(8.f / tune::SIM_STEP) && f.g.ecs.get<Transform>(ce).pos.y > y0 - 15.f; ++i) {
    step_sim(f.g, DT);
    max_tilt = std::max(max_tilt, std::abs(xf(f.g).angle));
  }
  CHECK(max_tilt < 0.8f, "a swinging load twisted the ship by %.2f rad", max_tilt);
  set_thrust(f.g, 0.f, 0.f);
  CHECK(f.g.ecs.get<Transform>(ce).pos.y < y0 - 15.f, "crate stayed put (%.1f)", f.g.ecs.get<Transform>(ce).pos.y);
  CHECK(r.held == ce, "lost the crate");
  toggle_grip(f.g);
  CHECK(r.held == NULL_ENTITY, "release failed");
  run(f.g, 8.f);

  // Delivery: a lifted crate that comes to rest on a pad
  const LandingPad& pad = f.g.cave.pads[0];
  put_body(c.body, {0.5f * (pad.x0 + pad.x1) - 60.f, pad.y - c.def->half_h - 1.f});
  c.picked = true;
  run(f.g, 3.f);
  CHECK(!c.picked && f.g.stats.cargo_delivered == 1, "delivery not counted (picked=%d delivered=%.0f)", c.picked, f.g.stats.cargo_delivered);
}

// The map is ringed by rock: full thrust sideways and up must stop at the walls, never leave the map.
void test_world_edges() {
  for (float dir : {-1.f, 1.f}) {
    Fixture f;
    run(f.g, 3.f);
    set_thrust(f.g, dir < 0 ? 1.f : 0.3f, dir < 0 ? 0.3f : 1.f);  // lean toward one side, climbing
    for (int i = 0; i < static_cast<int>(60.f / tune::SIM_STEP); ++i) {
      step_sim(f.g, DT);
      const Vec2 p = xf(f.g).pos;
      if (p.x < 0.f || p.x > Cave::WORLD_W || p.y < 0.f || p.y > Cave::WORLD_H) {
        CHECK(false, "left the map at (%.0f,%.0f)", p.x, p.y);
        break;
      }
    }
  }
}

// Drop the ship at many random open places, with random velocities: it must never end up inside the rock.
void test_ground_holds() {
  for (int ship : {1, 3, 5}) {
    Fixture f(0xBEEF + ship, ship);
    Rng rng(1234);
    int placed = 0, embedded = 0, crashed = 0;
    for (int attempt = 0; attempt < 400 && placed < 40; ++attempt) {
      const float x = rng.range(100.f, Cave::WORLD_W - 100.f), y = rng.range(200.f, Cave::WORLD_H - 200.f);
      if (!f.g.cave.is_open_box(x, y, 70.f, 70.f)) continue;
      respawn_ship(f.g, x);
      f.g.phys.stream(f.g.cave, {x, y}, 9);
      teleport(f.g, {x, y}, {rng.range(-150.f, 150.f), rng.range(-150.f, 150.f)});
      flight(f.g) = {};
      run(f.g, 6.f);
      ++placed;
      const Vec2 p = xf(f.g).pos;
      if (f.g.cave.is_solid_world(p.x, p.y)) {
        ++embedded;
        std::printf("  ship %d: centre in rock at (%.0f,%.0f), started (%.0f,%.0f)\n", ship, p.x, p.y, x, y);
      }
      crashed += flight(f.g).state == FlightState::Crashed;
    }
    std::printf("ground test, ship %d: %d drops, %d crashed, %d embedded\n", ship, placed, crashed, embedded);
    CHECK(placed >= 20, "only %d open places found", placed);
    CHECK(embedded == 0, "%d ships ended inside the rock", embedded);
  }
}

}  // namespace

int main() {
  test_settle_and_lift_off();
  test_crash_and_soft_drop();
  test_legs_retract();
  test_big_ships();
  test_ship_switch();
  test_cargo_and_rope();
  test_world_edges();
  test_ground_holds();
  std::printf(failures ? "%d FAILED\n" : "all physics tests passed\n", failures);
  return failures ? 1 : 0;
}
