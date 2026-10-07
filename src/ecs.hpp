// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Minimal sparse-set ECS: one dense pool per component type, entities are plain ids.
// Do not create/destroy entities from inside view(); queue them and destroy afterwards.

#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

using Entity = uint32_t;
inline constexpr Entity NULL_ENTITY = ~Entity{0};

template <class T>
class Pool {
 public:
  bool has(Entity e) const { return e < sparse_.size() && sparse_[e] != NIL; }

  T& add(Entity e, const T& v = T{}) {
    if (e >= sparse_.size()) sparse_.resize(e + 1, NIL);
    if (sparse_[e] != NIL) return dense_[sparse_[e]] = v;
    sparse_[e] = static_cast<uint32_t>(dense_.size());
    dense_.push_back(v);
    owners_.push_back(e);
    return dense_.back();
  }

  void remove(Entity e) {
    if (!has(e)) return;
    uint32_t i = sparse_[e];
    Entity last = owners_.back();
    dense_[i] = std::move(dense_.back());
    owners_[i] = last;
    sparse_[last] = i;
    dense_.pop_back();
    owners_.pop_back();
    sparse_[e] = NIL;
  }

  T& get(Entity e) { return dense_[sparse_[e]]; }
  const T& get(Entity e) const { return dense_[sparse_[e]]; }
  size_t size() const { return dense_.size(); }
  Entity owner(size_t i) const { return owners_[i]; }
  T& at(size_t i) { return dense_[i]; }
  const T& at(size_t i) const { return dense_[i]; }

 private:
  static constexpr uint32_t NIL = ~uint32_t{0};
  std::vector<T> dense_;
  std::vector<Entity> owners_;
  std::vector<uint32_t> sparse_;
};

template <class... Cs>
class Registry {
 public:
  Entity create() {
    if (!free_.empty()) {
      Entity e = free_.back();
      free_.pop_back();
      return e;
    }
    return next_++;
  }
  void destroy(Entity e) {
    (pool<Cs>().remove(e), ...);
    free_.push_back(e);
  }

  template <class C> Pool<C>& pool() { return std::get<Pool<C>>(pools_); }
  template <class C> const Pool<C>& pool() const { return std::get<Pool<C>>(pools_); }

  template <class C> C& add(Entity e, const C& c = C{}) { return pool<C>().add(e, c); }
  template <class C> bool has(Entity e) const { return pool<C>().has(e); }
  template <class C> C& get(Entity e) { return pool<C>().get(e); }
  template <class C> const C& get(Entity e) const { return pool<C>().get(e); }

  // Calls f(entity, D&, Q&...) for every entity owning D and all of Q.
  // D drives the iteration, so list the rarest component first.
  template <class D, class... Q, class F>
  void view(F&& f) {
    Pool<D>& drive = pool<D>();
    for (size_t i = 0; i < drive.size(); ++i) {
      Entity e = drive.owner(i);
      if ((... && pool<Q>().has(e))) f(e, drive.at(i), pool<Q>().get(e)...);
    }
  }
  template <class D, class... Q, class F>
  void view(F&& f) const {
    const Pool<D>& drive = pool<D>();
    for (size_t i = 0; i < drive.size(); ++i) {
      Entity e = drive.owner(i);
      if ((... && pool<Q>().has(e))) f(e, drive.at(i), pool<Q>().get(e)...);
    }
  }

 private:
  std::tuple<Pool<Cs>...> pools_;
  std::vector<Entity> free_;
  Entity next_ = 0;
};
