#pragma once
// Loads sim/data/tree_positions.csv (written by
// mapping/generate_sim_positions.py) into an engine LedGeometry.

#include <string>

#include "neotree/geometry.hpp"

namespace neotree::sim {

// Returns false and fills error on failure. On success the geometry is
// finalized and ready for Engine::init.
bool load_positions_csv(const std::string &path, LedGeometry &geometry, std::string &error);

// Parses durations like "90", "90s", "2.5m", "8h" into seconds. Returns a
// negative value if it can't.
double parse_duration_s(const std::string &text);

}  // namespace neotree::sim
