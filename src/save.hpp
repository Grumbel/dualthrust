// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include "game.hpp"

// Persistent world snapshot under $XDG_STATE_HOME/dualthrust/ (world + world.fog).
// Restored on the next launch so the pilot continues where they left off.

std::string world_file_path();
std::string world_fog_path();

// Write the current cave progress (seed, ship, cargo, fog, score…). Returns false on I/O error.
bool save_world(const Game& g, int ship_index);

// True if a readable world snapshot exists.
bool world_save_exists();

// Load into an already-generated cave (matching seed) and an existing ship entity.
// Returns false if the file is missing/corrupt; caller starts a fresh cave then.
bool load_world(Game& g, int* ship_index_out);

// Remove the snapshot (e.g. after New Cave so the next start is fresh).
void clear_world_save();
