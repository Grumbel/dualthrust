// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "physics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {

constexpr int CC = 30;  // chunk size in cells (divides both GW and GH, so the X seam falls on a chunk border)
constexpr float CHUNK_PX = CC * Cave::CELL;
constexpr int CHUNKS_X = Cave::GW / CC;
constexpr int CHUNKS_Y = Cave::GH / CC;
static_assert(Cave::GW % CC == 0 && Cave::GH % CC == 0, "chunks must tile the cave");

constexpr int STREAM_RADIUS = 1;  // chunks around the anchor that must exist
constexpr int KEEP_RADIUS = 2;    // chunks further away than this are dropped
constexpr int SUBSTEPS = 4;

uint64_t chunk_key(int kx, int ky) { return (static_cast<uint64_t>(kx) << 16) | static_cast<uint32_t>(ky); }

// ---------------------------------------------------------------------------
// Terrain: marching-squares segments -> polylines -> one-sided chains
// ---------------------------------------------------------------------------
// Contour points sit on cell-edge midpoints; in units of half a cell (8 px) they are integers, which makes
// them exact keys for joining segments (neighbouring cells name the shared edge midpoint identically).
constexpr int EDGE_X[4] = {1, 2, 1, 0};  // top, right, bottom, left midpoints within the marching square
constexpr int EDGE_Y[4] = {0, 1, 2, 1};
constexpr int CORNER_X[4] = {0, 2, 2, 0};  // TL, TR, BR, BL (bit i of the contour case)
constexpr int CORNER_Y[4] = {0, 0, 2, 2};

struct Seg {
  int ax, ay, bx, by;  // end points in half-cell units (unwrapped)
  uint64_t ka, kb;
  uint8_t mask, e0, e1;
  bool pad;
  bool used = false;
};

uint64_t point_key(int ix, int iy) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(ix + (1 << 20))) << 32) | static_cast<uint32_t>(iy + (1 << 20));
}

// Is the right-hand side of the segment e_from -> e_to (the side a Box2D chain collides on) rock? The segment
// cuts off one corner of its marching square (adjacent edges) or splits it in half (opposite edges); a corner on
// either side tells which side is rock.
bool right_is_rock(int mask, int e_from, int e_to) {
  const int lo = std::min(e_from, e_to), hi = std::max(e_from, e_to);
  int corner = 0;  // opposite edges: TL lies in the top / left half
  if (hi - lo != 2) {
    if (lo == 0 && hi == 1) corner = 1;
    else if (lo == 1 && hi == 2) corner = 2;
    else if (lo == 2 && hi == 3) corner = 3;
  }
  const bool rock = (mask >> corner) & 1;
  const int dx = EDGE_X[e_to] - EDGE_X[e_from], dy = EDGE_Y[e_to] - EDGE_Y[e_from];
  const int rx = dy, ry = -dx;  // right-hand normal
  const int side = (CORNER_X[corner] - EDGE_X[e_from]) * rx + (CORNER_Y[corner] - EDGE_Y[e_from]) * ry;
  return side > 0 ? rock : !rock;
}

struct PolyPoint {
  int ix, iy;
  bool pad;  // the segment leaving this point is a landing pad
};

bool on_pad_segment(const Cave& cave, const Seg& s) {
  if (s.ay != s.by) return false;
  const float y = s.ay * (Cave::CELL * 0.5f);
  const float mx = (s.ax + s.bx) * 0.5f * (Cave::CELL * 0.5f);
  for (const LandingPad& p : cave.pads)
    if (std::abs(y - p.y) < 1.f && mx >= p.x0 && mx <= p.x1) return true;
  return false;
}

b2BodyId build_chunk(b2WorldId world, const Cave& cave, int kx, int ky) {
  const int gx0 = kx * CC, gy0 = ky * CC;
  constexpr float H = Cave::CELL * 0.5f;

  // Cells one step beyond the chunk are included so every segment inside has its neighbours (smooth
  // normals); the polylines are cut at that outer ring.
  std::vector<Seg> segs;
  for (int gy = std::max(gy0 - 1, 0); gy <= std::min(gy0 + CC, Cave::GH - 1); ++gy)
    for (int gx = std::max(gx0 - 1, 0); gx <= std::min(gx0 + CC, Cave::GW - 1); ++gx) {
      const uint8_t mask = cave.contour_at(gx, gy);
      const ContourCase& cc = CONTOUR_CASES[mask];
      for (const auto& sg : cc.seg) {
        if (sg[0] < 0) continue;
        Seg s{};
        s.ax = 2 * gx + 1 + EDGE_X[sg[0]];
        s.ay = 2 * gy + 1 + EDGE_Y[sg[0]];
        s.bx = 2 * gx + 1 + EDGE_X[sg[1]];
        s.by = 2 * gy + 1 + EDGE_Y[sg[1]];
        s.ka = point_key(s.ax, s.ay);
        s.kb = point_key(s.bx, s.by);
        s.mask = mask;
        s.e0 = static_cast<uint8_t>(sg[0]);
        s.e1 = static_cast<uint8_t>(sg[1]);
        s.pad = on_pad_segment(cave, s);
        segs.push_back(s);
      }
    }

  b2BodyDef bd = b2DefaultBodyDef();
  bd.type = b2_staticBody;
  const Vec2 center{(gx0 + CC * 0.5f) * Cave::CELL, (gy0 + CC * 0.5f) * Cave::CELL};
  bd.position = to_b2(center);
  const b2BodyId body = b2CreateBody(world, &bd);
  if (segs.empty()) return body;

  // key -> segments touching it (a contour has at most two per point)
  std::unordered_map<uint64_t, std::array<int, 2>> adj;
  adj.reserve(segs.size() * 2);
  auto link = [&](uint64_t k, int si) {
    auto it = adj.try_emplace(k, std::array<int, 2>{-1, -1}).first;
    (it->second[0] < 0 ? it->second[0] : it->second[1]) = si;
  };
  for (int i = 0; i < static_cast<int>(segs.size()); ++i) {
    link(segs[i].ka, i);
    link(segs[i].kb, i);
  }

  auto emit_chain = [&](std::vector<PolyPoint> pts, bool loop, const Seg& first, bool first_forward) {
    // Orient: the open side must be on the right of the direction of travel
    if (right_is_rock(first.mask, first_forward ? first.e0 : first.e1, first_forward ? first.e1 : first.e0)) {
      std::reverse(pts.begin(), pts.end());
      // the pad flag belongs to the segment *leaving* a point: shift it onto the reversed order
      std::vector<bool> flags(pts.size());
      for (size_t i = 0; i < pts.size(); ++i) flags[i] = pts[i].pad;
      const size_t n = pts.size();
      for (size_t i = 0; i < n; ++i) pts[i].pad = loop ? flags[(i + 1) % n] : (i + 1 < n ? flags[i + 1] : false);
    }

    // Drop points in the middle of straight runs of the same material
    std::vector<PolyPoint> m;
    const size_t n = pts.size();
    for (size_t i = 0; i < n; ++i) {
      const bool has_prev = loop || i > 0, has_next = loop || i + 1 < n;
      if (has_prev && has_next) {
        const PolyPoint &p = pts[(i + n - 1) % n], &c = pts[i], &q = pts[(i + 1) % n];
        const int d1x = c.ix - p.ix, d1y = c.iy - p.iy, d2x = q.ix - c.ix, d2y = q.iy - c.iy;
        if (d1x * d2y - d1y * d2x == 0 && d1x * d2x + d1y * d2y > 0 && p.pad == c.pad) continue;
      }
      m.push_back(pts[i]);
    }

    auto local = [&](const PolyPoint& p) {
      return b2Vec2{(p.ix * H - center.x) / PPM, (p.iy * H - center.y) / PPM};
    };
    std::vector<b2Vec2> v;
    std::vector<b2SurfaceMaterial> mats;
    auto material = [&](bool pad) {
      b2SurfaceMaterial sm = b2DefaultSurfaceMaterial();
      sm.friction = pad ? tune::PAD_FRICTION : tune::ROCK_FRICTION;
      sm.restitution = 0.f;
      sm.userMaterialId = pad ? 1 : 0;
      return sm;
    };
    if (loop) {
      if (m.size() < 4) return;
      for (const PolyPoint& p : m) {
        v.push_back(local(p));
        mats.push_back(material(p.pad));
      }
    } else {
      if (m.size() < 2) return;
      // An open chain's first and last points only serve as ghost vertices: add one beyond each end
      const b2Vec2 a0 = local(m[0]), a1 = local(m[1]);
      const b2Vec2 z1 = local(m[m.size() - 1]), z0 = local(m[m.size() - 2]);
      v.push_back({2.f * a0.x - a1.x, 2.f * a0.y - a1.y});
      mats.push_back(material(false));
      for (const PolyPoint& p : m) {
        v.push_back(local(p));
        mats.push_back(material(p.pad));
      }
      v.push_back({2.f * z1.x - z0.x, 2.f * z1.y - z0.y});
      mats.push_back(material(false));
    }
    b2ChainDef cd = b2DefaultChainDef();
    cd.points = v.data();
    cd.count = static_cast<int>(v.size());
    cd.materials = mats.data();
    cd.materialCount = cd.count;
    cd.isLoop = loop;
    cd.filter.categoryBits = CAT_TERRAIN;
    cd.filter.maskBits = CAT_SHIP | CAT_CARGO | CAT_HOOK;
    b2CreateChain(body, &cd);
  };

  // Walk from `start_key` along unused segments, collecting the points; true when the walk came back around.
  auto walk = [&](uint64_t start_key, std::vector<PolyPoint>& out, int& first_seg, bool& first_forward) {
    uint64_t k = start_key;
    int end_x = 0, end_y = 0;
    first_seg = -1;
    for (;;) {
      int next = -1;
      for (int si : adj.find(k)->second)
        if (si >= 0 && !segs[si].used) { next = si; break; }
      if (next < 0) break;
      Seg& s = segs[next];
      s.used = true;
      const bool fwd = s.ka == k;
      if (first_seg < 0) { first_seg = next; first_forward = fwd; }
      out.push_back({fwd ? s.ax : s.bx, fwd ? s.ay : s.by, s.pad});
      end_x = fwd ? s.bx : s.ax;
      end_y = fwd ? s.by : s.ay;
      k = fwd ? s.kb : s.ka;
      if (k == start_key) return true;
    }
    if (first_seg >= 0) out.push_back({end_x, end_y, false});  // open: the far end of the last segment
    return false;
  };

  // Open polylines first (they start at a point touched by a single segment), then the closed loops
  for (int pass = 0; pass < 2; ++pass)
    for (int i = 0; i < static_cast<int>(segs.size()); ++i) {
      if (segs[i].used) continue;
      uint64_t start = segs[i].ka;
      if (pass == 0) {
        const bool a_end = adj[segs[i].ka][1] < 0, b_end = adj[segs[i].kb][1] < 0;
        if (!a_end && !b_end) continue;
        start = a_end ? segs[i].ka : segs[i].kb;
      }
      std::vector<PolyPoint> pts;
      int first_seg = -1;
      bool first_forward = true;
      const bool closed = walk(start, pts, first_seg, first_forward);
      if (first_seg >= 0) emit_chain(std::move(pts), closed, segs[first_seg], first_forward);
    }
  return body;
}

}  // namespace

// ---------------------------------------------------------------------------
// World, terrain streaming
// ---------------------------------------------------------------------------
void Physics::init() {
  if (ready()) return;
  b2WorldDef wd = b2DefaultWorldDef();
  wd.gravity = {0.f, tune::GRAVITY / PPM};
  wd.enableSleep = true;
  wd.hitEventThreshold = tune::HIT_MIN_SPEED / PPM;
  wd.restitutionThreshold = 1.f;
  world_ = b2CreateWorld(&wd);
}

void Physics::shutdown() {
  if (ready()) b2DestroyWorld(world_);
  world_ = b2_nullWorldId;
  chunks_.clear();
}

void Physics::step(float dt) { b2World_Step(world_, dt, SUBSTEPS); }

void Physics::reset_terrain() {
  for (auto& [key, body] : chunks_) b2DestroyBody(body);
  chunks_.clear();
}

void Physics::stream(const Cave& cave, const Vec2* anchors, int count, int max_new) {
  if (generation_ != cave.generation) {
    reset_terrain();
    generation_ = cave.generation;
  }
  auto chunk_of = [](Vec2 p, int& kx, int& ky) {
    kx = std::clamp(static_cast<int>(std::floor(p.x / CHUNK_PX)), 0, CHUNKS_X - 1);
    ky = std::clamp(static_cast<int>(std::floor(p.y / CHUNK_PX)), 0, CHUNKS_Y - 1);
  };

  for (auto it = chunks_.begin(); it != chunks_.end();) {
    const int kx = static_cast<int>(it->first >> 16), ky = static_cast<int>(it->first & 0xffff);
    bool keep = false;
    for (int i = 0; i < count && !keep; ++i) {
      int ax, ay;
      chunk_of(anchors[i], ax, ay);
      keep = std::abs(kx - ax) <= KEEP_RADIUS && std::abs(ky - ay) <= KEEP_RADIUS;
    }
    if (!keep) {
      b2DestroyBody(it->second);
      it = chunks_.erase(it);
    } else {
      ++it;
    }
  }

  // Nearest missing chunks first, for each anchor in turn
  for (int ring = 0; ring <= STREAM_RADIUS && max_new > 0; ++ring)
    for (int i = 0; i < count && max_new > 0; ++i) {
      int ax, ay;
      chunk_of(anchors[i], ax, ay);
      for (int dy = -ring; dy <= ring && max_new > 0; ++dy)
        for (int dx = -ring; dx <= ring && max_new > 0; ++dx) {
          if (std::max(std::abs(dx), std::abs(dy)) != ring) continue;
          const int kx = ax + dx, ky = ay + dy;
          if (kx < 0 || kx >= CHUNKS_X || ky < 0 || ky >= CHUNKS_Y) continue;
          const uint64_t key = chunk_key(kx, ky);
          if (chunks_.count(key)) continue;
          chunks_[key] = build_chunk(world_, cave, kx, ky);
          --max_new;
        }
    }
}

bool Physics::terrain_at(Vec2 p) const {
  // The ground a body rests on is built once the chunk around it and its neighbours below exist
  const int kx = std::clamp(static_cast<int>(std::floor(p.x / CHUNK_PX)), 0, CHUNKS_X - 1);
  const int ky = std::clamp(static_cast<int>(std::floor(p.y / CHUNK_PX)), 0, CHUNKS_Y - 1);
  for (int dy = 0; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
      const int x = kx + dx, y = ky + dy;
      if (x < 0 || x >= CHUNKS_X || y < 0 || y >= CHUNKS_Y) continue;
      if (!chunks_.count(chunk_key(x, y))) return false;
    }
  return true;
}

// ---------------------------------------------------------------------------
// Ship
// ---------------------------------------------------------------------------
namespace {

b2ShapeDef part_shape(Part part, float friction) {
  b2ShapeDef sd = b2DefaultShapeDef();
  sd.userData = part_tag(part);
  sd.density = 1.f;
  sd.material.friction = friction;
  sd.material.restitution = 0.f;
  sd.filter.categoryBits = CAT_SHIP;
  sd.filter.maskBits = CAT_TERRAIN | CAT_CARGO;
  sd.enableHitEvents = true;
  return sd;
}

b2Polygon rounded_hull(const b2Vec2* pts, int n, float radius) {
  const b2Hull hull = b2ComputeHull(pts, n);
  return b2MakePolygon(&hull, radius);
}

}  // namespace

ShipBodies Physics::create_ship(const ShipDef& d, Vec2 pos, float angle, Vec2 vel, float ang_vel) {
  ShipBodies s;
  const b2Rot rot = b2MakeRot(angle);
  const float leg_mass = d.mass * tune::LEG_MASS_FRACTION;
  const HullGeom hg = hull_geom(d);
  const LegGeom lg = leg_geom(d);

  // --- Hull: cabin / belly outline plus an engine bell on each side ---
  b2BodyDef bd = b2DefaultBodyDef();
  bd.type = b2_dynamicBody;
  bd.position = to_b2(pos);
  bd.rotation = rot;
  bd.linearVelocity = to_b2(vel);
  bd.angularVelocity = ang_vel;
  bd.linearDamping = tune::LINEAR_DRAG;
  bd.angularDamping = tune::ANGULAR_DRAG;
  s.hull = b2CreateBody(world_, &bd);

  const float R = 2.f / PPM;  // skin radius of the rounded polygons
  {
    const b2Vec2 body_pts[] = {to_b2({0.f, hg.nose_y}),
                               to_b2({hg.cabin_hw(), hg.cabin_y}),
                               to_b2({hg.belly_hw(), hg.belly_y}),
                               to_b2({-hg.belly_hw(), hg.belly_y}),
                               to_b2({-hg.cabin_hw(), hg.cabin_y})};
    const b2ShapeDef sd = part_shape(Part::Hull, tune::HULL_FRICTION);
    const b2Polygon poly = rounded_hull(body_pts, 5, R);
    b2CreatePolygonShape(s.hull, &sd, &poly);

    const bool top = d.engines_top;
    const float ey = d.eng_y();
    const float back = top ? ey + 2.f : ey - 2.f, rim = top ? ey + 16.f : ey + 12.f;
    const float bw = top ? 8.f : 7.f, rw = top ? 11.f : 10.f;
    if (d.style & STYLE_TANKS) {  // fuel tanks beside the cabin
      const float tw = 6.f, th = std::max(6.f, 0.5f * (hg.belly_y - hg.cabin_y) - 2.f), cy = 0.5f * (hg.belly_y + hg.cabin_y);
      for (float side : {-1.f, 1.f}) {
        const b2Polygon tp = b2MakeOffsetRoundedBox(tw / PPM, th / PPM, to_b2({side * (hg.cabin_hw() + 9.f), cy}), b2MakeRot(0.f), R);
        b2CreatePolygonShape(s.hull, &sd, &tp);
      }
    }
    if (d.style & STYLE_DECK) {  // flat deck across the belly that carries outboard thrusters
      const b2Polygon dp = b2MakeOffsetRoundedBox(hg.hw / PPM, 2.f / PPM, to_b2({0.f, hg.belly_y - 1.f}), b2MakeRot(0.f), R);
      b2CreatePolygonShape(s.hull, &sd, &dp);
    }
    if (d.thruster_n == 0) {
      for (float side : {-1.f, 1.f}) {
        const float ex = side * d.engine_offset_x;
        const b2Vec2 bell[] = {to_b2({ex - bw, back}), to_b2({ex + bw, back}), to_b2({ex + rw, rim}),
                               to_b2({ex - rw, rim})};
        const b2Polygon bp = rounded_hull(bell, 4, R * 0.5f);
        b2CreatePolygonShape(s.hull, &sd, &bp);
      }
    } else {  // a bell around each thruster, along its exhaust
      for (int i = 0; i < d.thruster_n; ++i) {
        const ThrusterPose tp = thruster_pose(d, i);
        const Vec2 f = tp.flame, p{-f.y, f.x};
        const float w0 = 5.f + 2.f * tp.power, w1 = 8.f + 2.f * tp.power;
        const Vec2 b0 = tp.pos - f * 2.f, b1 = tp.pos + f * 12.f;
        const b2Vec2 bell[] = {to_b2(b0 - p * w0), to_b2(b0 + p * w0), to_b2(b1 + p * w1), to_b2(b1 - p * w1)};
        const b2Polygon bp = rounded_hull(bell, 4, R * 0.5f);
        b2CreatePolygonShape(s.hull, &sd, &bp);
      }
    }
  }
  // Mass: the whole ship's mass and inertia live in the hull plus two light legs
  const float m_hull = d.mass - 2.f * leg_mass;
  const float leg_x_m = lg.foot_x / PPM;
  const float inertia = std::max(d.inertia / (PPM * PPM) - 2.f * leg_mass * leg_x_m * leg_x_m, d.inertia / (PPM * PPM) * 0.3f);
  b2Body_SetMassData(s.hull, {m_hull, to_b2({0.f, com_y(d)}), inertia});

  // --- Legs ---
  for (int i = 0; i < 2; ++i) {
    const float side = i == 0 ? -1.f : 1.f;
    const Vec2 axis{side * lg.axis_x, lg.axis_y};  // unit, hull-local, out of the hull
    const Vec2 foot{side * lg.foot_x, lg.foot_y};  // hull-local, extended
    b2BodyDef lb = b2DefaultBodyDef();
    lb.type = b2_dynamicBody;
    lb.position = to_b2(pos + rotate(foot, angle));
    lb.rotation = rot;
    lb.linearVelocity = to_b2(vel);
    lb.angularVelocity = ang_vel;
    lb.linearDamping = tune::LINEAR_DRAG;
    s.leg[i] = b2CreateBody(world_, &lb);

    const float fw = lg.foot_half_w;
    {  // landing plate
      b2ShapeDef sd = part_shape(Part::Foot, tune::FOOT_FRICTION);
      const b2Capsule cap{to_b2({-fw, 0.f}), to_b2({fw, 0.f}), 2.5f / PPM};
      b2CreateCapsuleShape(s.leg[i], &sd, &cap);
    }
    {  // strut, from the foot back up toward the hull
      b2ShapeDef sd = part_shape(Part::Strut, 0.3f);
      const b2Capsule cap{to_b2({0.f, 0.f}), to_b2({-axis.x * lg.length, -axis.y * lg.length}), 1.5f / PPM};
      b2CreateCapsuleShape(s.leg[i], &sd, &cap);
    }
    b2Body_SetMassData(s.leg[i], {leg_mass, {0.f, 0.f}, leg_mass * 0.04f});

    b2PrismaticJointDef jd = b2DefaultPrismaticJointDef();
    jd.bodyIdA = s.hull;
    jd.bodyIdB = s.leg[i];
    jd.localAnchorA = to_b2(foot);  // where the foot sits when fully out
    jd.localAnchorB = {0.f, 0.f};
    jd.localAxisA = {axis.x, axis.y};
    jd.referenceAngle = 0.f;
    jd.enableLimit = true;
    jd.lowerTranslation = -lg.travel / PPM;
    jd.upperTranslation = 0.f;
    jd.collideConnected = false;
    s.joint[i] = b2CreatePrismaticJoint(world_, &jd);
  }

  // --- Hook on its cable, tucked under the winch ---
  {
    const Vec2 winch{0.f, winch_y(d)};
    b2BodyDef hb = b2DefaultBodyDef();
    hb.type = b2_dynamicBody;
    hb.position = to_b2(pos + rotate(Vec2{0.f, com_y(d)} + Vec2{0.f, rope::MIN_LEN}, angle));
    hb.rotation = rot;
    hb.linearVelocity = to_b2(vel);
    hb.linearDamping = 0.4f;
    hb.angularDamping = 1.0f;
    s.hook = b2CreateBody(world_, &hb);
    b2ShapeDef sd = b2DefaultShapeDef();
    sd.userData = part_tag(Part::Hook);
    sd.density = 1.f;
    sd.material.friction = 0.5f;
    sd.filter.categoryBits = CAT_HOOK;
    sd.filter.maskBits = CAT_TERRAIN | CAT_CARGO;
    const b2Circle ball{{0.f, 0.f}, 5.f / PPM};
    b2CreateCircleShape(s.hook, &sd, &ball);
    const float hm = d.mass * rope::HOOK_MASS_FRACTION;
    b2Body_SetMassData(s.hook, {hm, {0.f, 0.f}, hm * 0.02f});

    b2DistanceJointDef dj = b2DefaultDistanceJointDef();
    dj.bodyIdA = s.hull;
    dj.bodyIdB = s.hook;
    // The cable pulls on the centre of mass, not on the winch eye: a swinging load then cannot twist the ship
    // (the drawn cable still starts at the winch, a few px away)
    dj.localAnchorA = to_b2({0.f, com_y(d)});
    dj.localAnchorB = {0.f, 0.f};
    dj.length = rope::MIN_LEN / PPM;
    dj.enableSpring = true;  // spring without stiffness: the cable only limits the distance (a rope)
    dj.hertz = 0.f;
    dj.enableLimit = true;
    dj.minLength = 0.05f;
    dj.maxLength = rope::MIN_LEN / PPM;
    dj.collideConnected = false;
    s.cable = b2CreateDistanceJoint(world_, &dj);
  }
  return s;
}

b2BodyId Physics::create_cargo(Vec2 pos, float angle, const CargoDef& cd) {
  b2BodyDef bd = b2DefaultBodyDef();
  bd.type = b2_dynamicBody;
  bd.position = to_b2(pos);
  bd.rotation = b2MakeRot(angle);
  bd.linearDamping = 0.05f;
  bd.angularDamping = 0.3f;
  bd.isEnabled = false;  // switched on once there is ground under it
  const b2BodyId body = b2CreateBody(world_, &bd);
  b2ShapeDef sd = b2DefaultShapeDef();
  sd.userData = part_tag(Part::Cargo);
  sd.density = 1.f;
  sd.material.friction = 0.7f;
  sd.material.restitution = 0.05f;
  sd.filter.categoryBits = CAT_CARGO;
  sd.filter.maskBits = CAT_TERRAIN | CAT_CARGO | CAT_SHIP | CAT_HOOK;
  const b2Polygon box = b2MakeRoundedBox(cd.half_w / PPM - 1.5f / PPM, cd.half_h / PPM - 1.5f / PPM, 1.5f / PPM);
  b2CreatePolygonShape(body, &sd, &box);
  const float w = 2.f * cd.half_w / PPM, h = 2.f * cd.half_h / PPM;
  b2Body_SetMassData(body, {cd.mass, {0.f, 0.f}, cd.mass * (w * w + h * h) / 12.f});
  return body;
}

void Physics::destroy_ship(ShipBodies& s) {
  if (B2_IS_NON_NULL(s.grip) && b2Joint_IsValid(s.grip)) b2DestroyJoint(s.grip);
  s.grip = b2_nullJointId;
  if (B2_IS_NON_NULL(s.cable)) b2DestroyJoint(s.cable);
  if (B2_IS_NON_NULL(s.hook)) b2DestroyBody(s.hook);
  s.cable = b2_nullJointId;
  s.hook = b2_nullBodyId;
  for (int i = 0; i < 2; ++i) {
    if (B2_IS_NON_NULL(s.joint[i])) b2DestroyJoint(s.joint[i]);
    if (B2_IS_NON_NULL(s.leg[i])) b2DestroyBody(s.leg[i]);
    s.joint[i] = b2_nullJointId;
    s.leg[i] = b2_nullBodyId;
  }
  if (B2_IS_NON_NULL(s.hull)) b2DestroyBody(s.hull);
  s.hull = b2_nullBodyId;
}

void Physics::apply_ship_mass(ShipBodies& s, const ShipDef& d, float mass_mul) {
  if (!B2_IS_NON_NULL(s.hull)) return;
  const float mul = std::max(0.05f, mass_mul);
  const float leg_mass = d.mass * mul * tune::LEG_MASS_FRACTION;
  const LegGeom lg = leg_geom(d);
  const float m_hull = d.mass * mul - 2.f * leg_mass;
  const float leg_x_m = lg.foot_x / PPM;
  const float inertia =
      std::max(d.inertia * mul / (PPM * PPM) - 2.f * leg_mass * leg_x_m * leg_x_m, d.inertia * mul / (PPM * PPM) * 0.3f);
  b2Body_SetMassData(s.hull, {m_hull, to_b2({0.f, com_y(d)}), inertia});
  for (int i = 0; i < 2; ++i) {
    if (B2_IS_NON_NULL(s.leg[i]))
      b2Body_SetMassData(s.leg[i], {leg_mass, {0.f, 0.f}, leg_mass * 0.04f});
  }
  if (B2_IS_NON_NULL(s.hook)) {
    const float hm = d.mass * mul * rope::HOOK_MASS_FRACTION;
    b2Body_SetMassData(s.hook, {hm, {0.f, 0.f}, hm * 0.02f});
  }
}

void Physics::sync_gravity() {
  if (!ready()) return;
  b2World_SetGravity(world_, {0.f, tune::GRAVITY / PPM});
}
