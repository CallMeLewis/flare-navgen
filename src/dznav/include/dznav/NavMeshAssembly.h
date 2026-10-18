// A live dtNavMesh (DayZ-patched Detour) that tiles are added to, linked, and exported from.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "dznav/NavMeshFormat.h"

class dtNavMesh;

namespace dznav {

class NavMeshAssembly {
 public:
  NavMeshAssembly();
  ~NavMeshAssembly();
  NavMeshAssembly(const NavMeshAssembly&) = delete;
  NavMeshAssembly& operator=(const NavMeshAssembly&) = delete;

  // Creates an empty mesh for a tilesX x tilesZ grid.
  void init(int tilesX, int tilesZ, float tileWidth, float tileHeight, const float orig[3]);
  // Creates the mesh from a file and adds its tiles in file order, keeping their refs.
  void load(const NavMeshFile& file);

  // Adds a tile blob (tile version 12), replacing any tile at the same grid cell. The previous
  // tile's slot is reused so the tile order of the file is kept. Returns the new tile ref.
  uint64_t setTile(std::vector<uint8_t> data);
  bool removeTile(int x, int z);
  bool hasTile(int x, int z) const;

  // Exports every tile in tile-index order (the order DayZ's navmeshes are saved in).
  NavMeshFile toFile() const;

  struct IslandFilterResult {
    bool seedFound = false;
    float seedPoint[3]{};
    size_t reachable = 0, isolated = 0;
  };
  // Flood fill from the poly nearest `seed` along every link and flag unreached
  // polys 0x8000 (existing 0x8000 flags are cleared first). The seed snaps with
  // half extents {30, 30, 3}; with searchAllHeights the vertical extent covers the whole world.
  IslandFilterResult filterIsolatedIslands(const float seed[3], bool searchAllHeights = false);

  dtNavMesh* nav() { return nav_; }
  const dtNavMesh* nav() const { return nav_; }
  int tilesX() const { return tilesX_; }
  int tilesZ() const { return tilesZ_; }

 private:
  dtNavMesh* nav_ = nullptr;
  int tilesX_ = 0, tilesZ_ = 0;
  // Obstacle sections as loaded. addTile re-snaps obstacle Y, which isn't idempotent; untouched
  // tiles keep their original bytes on export.
  std::map<uint64_t, std::vector<Obstacle>> loadedObstacles_;
};

}  // namespace dznav
