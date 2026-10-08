// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Permanent statistics: lifetime counters kept in $XDG_STATE_HOME/dualthrust/stats. Every counter is a row in
// STAT_FIELDS (key in the file, label in the menu, how to print it), so adding one is adding a row.

#include <cstddef>

struct Stats {
  double flight_time = 0;      // s, engines-on or coasting (not sitting landed)
  double landed_time = 0;      // s parked
  double thrust_time = 0;      // engine-seconds at full thrust
  double distance = 0;         // m flown
  double landings = 0;
  double pad_landings = 0;
  double crashes = 0;
  double hard_hits = 0;        // bounces off the rock that were not fatal
  double caves = 0;            // caves generated
  double cargo_picked = 0;     // reserved for the rope and hook
  double cargo_delivered = 0;
};

enum class StatFmt { Count, Time, Distance };
struct StatField {
  const char* key;
  const char* label;
  double Stats::* member;
  StatFmt fmt;
};

inline constexpr StatField STAT_FIELDS[] = {
    {"flight_time", "FLIGHT TIME", &Stats::flight_time, StatFmt::Time},
    {"landed_time", "TIME LANDED", &Stats::landed_time, StatFmt::Time},
    {"thrust_time", "THRUST TIME", &Stats::thrust_time, StatFmt::Time},
    {"distance", "DISTANCE", &Stats::distance, StatFmt::Distance},
    {"landings", "LANDINGS", &Stats::landings, StatFmt::Count},
    {"pad_landings", "PAD LANDINGS", &Stats::pad_landings, StatFmt::Count},
    {"hard_hits", "HARD HITS", &Stats::hard_hits, StatFmt::Count},
    {"crashes", "CRASHES", &Stats::crashes, StatFmt::Count},
    {"caves", "CAVES VISITED", &Stats::caves, StatFmt::Count},
    {"cargo_picked", "CARGO PICKED UP", &Stats::cargo_picked, StatFmt::Count},
    {"cargo_delivered", "CARGO DELIVERED", &Stats::cargo_delivered, StatFmt::Count},
};
inline constexpr int STAT_FIELD_COUNT = static_cast<int>(sizeof(STAT_FIELDS) / sizeof(STAT_FIELDS[0]));

Stats load_stats();
bool save_stats(const Stats& s);
void format_stat(const StatField& f, const Stats& s, char* buf, size_t n);
