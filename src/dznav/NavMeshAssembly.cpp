#include "dznav/NavMeshAssembly.h"

#include <cstring>

#include "DetourAlloc.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

namespace dznav {
namespace {

unsigned char* copyToDetour(const std::vector<uint8_t>& data) {
  auto* p = static_cast<unsigned char*>(dtAlloc(int(data.size()), DT_ALLOC_PERM));
  if (!p) throw FormatError{"out of memory"};
  std::memcpy(p, data.data(), data.size());
  return p;
}

}  // namespace

NavMeshAssembly::NavMeshAssembly() = default;

NavMeshAssembly::~NavMeshAssembly() {
  if (nav_) dtFreeNavMesh(nav_);
}

void NavMeshAssembly::init(int tilesX, int tilesZ, float tileWidth, float tileHeight, const float orig[3]) {
  if (nav_) dtFreeNavMesh(nav_);
  loadedObstacles_.clear();
  nav_ = dtAllocNavMesh();
  tilesX_ = tilesX;
  tilesZ_ = tilesZ;
  dtNavMeshParams p{};
  std::memcpy(p.orig, orig, sizeof(p.orig));
  p.tileWidth = tileWidth;
  p.tileHeight = tileHeight;
  p.maxTiles = tilesX * tilesZ;
  p.maxPolys = 1 << kPolyBits;
  if (dtStatusFailed(nav_->init(&p))) throw FormatError{"dtNavMesh::init failed"};
}

void NavMeshAssembly::load(const NavMeshFile& file) {
  const SetHeader& h = file.header;
  init(h.tilesX, h.tilesZ, h.tileWidth, h.tileHeight, h.orig);
  for (const Tile& t : file.tiles) {
    std::vector<uint8_t> data = serializeTile(t);
    dtTileRef ref = 0;
    const dtStatus st = nav_->addTile(copyToDetour(data), int(data.size()), DT_TILE_FREE_DATA, t.ref, &ref);
    if (dtStatusFailed(st)) throw FormatError{"addTile failed while loading"};
    loadedObstacles_[ref] = t.obstacles;
  }
}

uint64_t NavMeshAssembly::setTile(std::vector<uint8_t> data) {
  MeshHeader mh;
  if (data.size() < sizeof(mh)) throw FormatError{"tile data too small"};
  std::memcpy(&mh, data.data(), sizeof(mh));

  // Reuse the slot of the tile being replaced, with a new salt, so tile order is stable.
  dtTileRef lastRef = 0;
  if (const dtMeshTile* old = nav_->getTileAt(mh.x, mh.y, mh.layer)) {
    const dtTileRef oldRef = nav_->getTileRef(old);
    loadedObstacles_.erase(oldRef);
    if (dtStatusFailed(nav_->removeTile(oldRef, nullptr, nullptr))) throw FormatError{"removeTile failed"};
    const unsigned int index = nav_->decodePolyIdTile(oldRef);
    lastRef = nav_->encodePolyId(static_cast<const dtNavMesh*>(nav_)->getTile(int(index))->salt, index, 0);
  }
  dtTileRef ref = 0;
  const dtStatus st = nav_->addTile(copyToDetour(data), int(data.size()), DT_TILE_FREE_DATA, lastRef, &ref);
  if (dtStatusFailed(st)) throw FormatError{"addTile failed"};
  return ref;
}

bool NavMeshAssembly::hasTile(int x, int z) const { return nav_->getTileAt(x, z, 0) != nullptr; }

bool NavMeshAssembly::removeTile(int x, int z) {
  const dtMeshTile* t = nav_->getTileAt(x, z, 0);
  if (!t) return false;
  const dtTileRef ref = nav_->getTileRef(t);
  loadedObstacles_.erase(ref);
  return dtStatusSucceed(nav_->removeTile(ref, nullptr, nullptr));
}

NavMeshAssembly::IslandFilterResult NavMeshAssembly::filterIsolatedIslands(const float seed[3],
                                                                         bool searchAllHeights) {
  IslandFilterResult r;
  const dtNavMesh& nav = *nav_;
  // Clear previous results.
  for (int i = 0; i < nav.getMaxTiles(); ++i) {
    const dtMeshTile* t = nav.getTile(i);
    if (!t || !t->header) continue;
    for (int p = 0; p < t->header->polyCount; ++p) t->polys[p].flags &= ~kIsolatedFlag;
  }

  dtNavMeshQuery* query = dtAllocNavMeshQuery();
  if (!query || dtStatusFailed(query->init(nav_, 2048))) {
    dtFreeNavMeshQuery(query);
    throw FormatError{"dtNavMeshQuery::init failed"};
  }
  dtQueryFilter filter;
  filter.setIncludeFlags(0xffff);
  filter.setExcludeFlags(0x8002);
  const float extents[3] = {30.0f, searchAllHeights ? 10000.0f : 30.0f, 3.0f};
  dtPolyRef start = 0;
  query->findNearestPoly(seed, extents, &filter, &start, r.seedPoint);
  dtFreeNavMeshQuery(query);
  if (!start) return r;
  r.seedFound = true;

  // Depth-first walk over every link, regardless of flags (closed doors are traversed).
  std::vector<std::vector<uint8_t>> visited(size_t(nav.getMaxTiles()));
  for (int i = 0; i < nav.getMaxTiles(); ++i) {
    const dtMeshTile* t = nav.getTile(i);
    if (t && t->header) visited[i].assign(size_t(t->header->polyCount), 0);
  }
  std::vector<dtPolyRef> stack{start};
  {
    const dtMeshTile* t = nullptr;
    const dtPoly* p = nullptr;
    nav.getTileAndPolyByRefUnsafe(start, &t, &p);
    visited[nav.decodePolyIdTile(start)][nav.decodePolyIdPoly(start)] = 1;
  }
  while (!stack.empty()) {
    const dtPolyRef ref = stack.back();
    stack.pop_back();
    const dtMeshTile* t = nullptr;
    const dtPoly* p = nullptr;
    nav.getTileAndPolyByRefUnsafe(ref, &t, &p);
    for (unsigned int l = p->firstLink; l != DT_NULL_LINK; l = t->links[l].next) {
      const dtPolyRef nei = t->links[l].ref;
      if (!nei) continue;
      uint8_t& v = visited[nav.decodePolyIdTile(nei)][nav.decodePolyIdPoly(nei)];
      if (v) continue;
      v = 1;
      stack.push_back(nei);
    }
  }
  for (int i = 0; i < nav.getMaxTiles(); ++i) {
    const dtMeshTile* t = nav.getTile(i);
    if (!t || !t->header) continue;
    for (int p = 0; p < t->header->polyCount; ++p) {
      if (visited[i][p]) {
        ++r.reachable;
      } else {
        t->polys[p].flags |= kIsolatedFlag;
        ++r.isolated;
      }
    }
  }
  return r;
}

NavMeshFile NavMeshAssembly::toFile() const {
  NavMeshFile f;
  const dtNavMeshParams* p = nav_->getParams();
  f.header.magic = kSetMagic;
  f.header.version = kSetVersion;
  f.header.tilesX = tilesX_;
  f.header.tilesZ = tilesZ_;
  std::memcpy(f.header.orig, p->orig, sizeof(f.header.orig));
  f.header.tileWidth = p->tileWidth;
  f.header.tileHeight = p->tileHeight;
  f.header.maxTiles = p->maxTiles;
  f.header.maxPolys = p->maxPolys;
  for (int i = 0; i < nav_->getMaxTiles(); ++i) {
    const dtMeshTile* t = static_cast<const dtNavMesh*>(nav_)->getTile(i);
    if (!t || !t->header) continue;
    const dtTileRef ref = nav_->getTileRef(t);
    Tile tile = parseTile(std::span<const uint8_t>(t->data, size_t(t->dataSize)), ref);
    auto it = loadedObstacles_.find(ref);
    if (it != loadedObstacles_.end() && it->second.size() == tile.obstacles.size()) tile.obstacles = it->second;
    f.tiles.push_back(std::move(tile));
  }
  f.header.numTiles = int32_t(f.tiles.size());
  return f;
}

}  // namespace dznav
