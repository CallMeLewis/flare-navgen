// Tile-by-tile comparison of two navmeshes, used to validate generated output against shipped maps.
#pragma once

#include <map>
#include <vector>

#include "dznav/NavMeshFormat.h"

namespace dznav {

struct TileSummary {
  int x = 0, z = 0;
  int polys = 0, verts = 0, offMesh = 0, obstacles = 0, parts = 0;
  float cellSize = 0;
  std::map<int, double> areaByType;  // XZ area (m^2) of ground polys per area id
  double walkableArea() const;
};

TileSummary summarizeTile(const Tile& tile);

struct TileDiff {
  TileSummary a, b;
  double areaDelta = 0;  // sum over area ids of |a - b|
};

struct CompareResult {
  std::vector<TileDiff> tiles;  // tiles present in both files
  int onlyInA = 0, onlyInB = 0;
  std::map<int, double> totalAreaA, totalAreaB;
};

CompareResult compareNavMeshes(const NavMeshFile& a, const NavMeshFile& b);

// Byte comparison of tiles present in both files, ignoring what depends on neighbouring tiles or
// load order (links, firstLink, snapped off-mesh endpoints, the reserved off-mesh float, obstacle
// Y) and the door "disabled" bit (random in reference files). If the reference has
// no isolated polygons (a partial save), the isolated flag is ignored too. With ignoreOrder, tree
// obstacles and door parts compare as sets (a new DayZDiag session can list objects in another
// order).
struct VerifyResult {
  int compared = 0, identical = 0;
  bool ignoredIsolated = false, ignoredOrder = false;
  std::vector<std::pair<int, int>> different;  // tile coordinates
};
VerifyResult verifyTiles(const NavMeshFile& reference, const NavMeshFile& candidate, bool ignoreOrder = false);
Tile canonicalTile(const Tile& tile, bool ignoreIsolated = false, bool ignoreOrder = false);

}  // namespace dznav
