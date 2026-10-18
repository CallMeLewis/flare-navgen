// Builds one DayZ navmesh tile from data-server input, byte for byte as in DayZ's navmeshes.
#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include "dznav/Generator.h"
#include "dznav/WorldInput.h"

namespace dznav {

enum class DoorStatePolicy {
  Closed,   // doors start closed: open-state (area 12) polys get the disabled flag
  Open,     // doors start open: closed-state (area 13) polys get the disabled flag
  Enabled,  // no door poly is disabled
};

struct TileBuildOptions {
  bool jumpLinks = true;
  DoorStatePolicy doors = DoorStatePolicy::Closed;
  // Overrides the sea level sent by the server (it is the live tide value). NaN keeps it.
  float seaLevel = std::numeric_limits<float>::quiet_NaN();
};

// Returns the tile blob (empty when the tile has no walkable surface). Throws std::runtime_error
// on build failure.
std::vector<uint8_t> buildTile(const WorldParams& params, const TileInput& input, TileBuildStats& stats,
                               const TileBuildOptions& options = {});

}  // namespace dznav
