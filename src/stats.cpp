// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "stats.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include "config.hpp"

namespace {
std::string stats_path() { return state_dir_path() + "/stats"; }
}  // namespace

Stats load_stats() {
  Stats s;
  std::FILE* f = std::fopen(stats_path().c_str(), "r");
  if (!f) return s;
  char line[128];
  while (std::fgets(line, sizeof line, f)) {
    char key[64];
    double val = 0;
    if (line[0] == '#' || std::sscanf(line, "%63[^=]=%lf", key, &val) != 2) continue;
    for (const StatField& fld : STAT_FIELDS)
      if (!std::strcmp(key, fld.key)) s.*fld.member = val < 0 ? 0 : val;
  }
  std::fclose(f);
  return s;
}

bool save_stats(const Stats& s) {
  if (!make_dirs(state_dir_path())) {
    std::fprintf(stderr, "dualthrust: cannot create %s: %s\n", state_dir_path().c_str(), std::strerror(errno));
    return false;
  }
  // Write beside, then rename: a crash mid-write never leaves a truncated file behind
  const std::string tmp = stats_path() + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f) {
    std::fprintf(stderr, "dualthrust: cannot write %s: %s\n", tmp.c_str(), std::strerror(errno));
    return false;
  }
  std::fprintf(f, "# dualthrust statistics (XDG state)\n");
  for (const StatField& fld : STAT_FIELDS) std::fprintf(f, "%s=%.2f\n", fld.key, s.*fld.member);
  const bool ok = std::fclose(f) == 0 && std::rename(tmp.c_str(), stats_path().c_str()) == 0;
  if (!ok) std::fprintf(stderr, "dualthrust: cannot save %s: %s\n", stats_path().c_str(), std::strerror(errno));
  flush_user_files();
  return ok;
}

void format_stat(const StatField& f, const Stats& s, char* buf, size_t n) {
  const double v = s.*f.member;
  switch (f.fmt) {
    case StatFmt::Count: std::snprintf(buf, n, "%.0f", v); break;
    case StatFmt::Time: {
      const long t = static_cast<long>(v);
      if (t >= 3600) std::snprintf(buf, n, "%ldH %02ldM", t / 3600, t / 60 % 60);
      else std::snprintf(buf, n, "%ldM %02ldS", t / 60, t % 60);
      break;
    }
    case StatFmt::Distance:
      if (v >= 1000.0) std::snprintf(buf, n, "%.2f KM", v / 1000.0);
      else std::snprintf(buf, n, "%.0f M", v);
      break;
  }
}
