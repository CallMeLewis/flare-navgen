// The C interface in dznav_core.h, over TileBuilder and the patched Detour.
#include "dznav_core.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "DetourAlloc.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "RecastAlloc.h"
#include "dznav/NavMeshFormat.h"
#include "dznav/TileBuilder.h"
#include "dznav/WorldInput.h"

struct DznNavMesh {
  dtNavMesh* nav = nullptr;
};

namespace {

DznBytes copyBytes(const void* data, size_t len) {
  DznBytes b{nullptr, 0};
  if (!len) return b;
  b.data = static_cast<uint8_t*>(std::malloc(len));
  if (!b.data) throw std::bad_alloc();
  std::memcpy(b.data, data, len);
  b.len = len;
  return b;
}

void setError(DznBytes* error, const std::string& message) {
  if (!error) return;
  try {
    *error = copyBytes(message.data(), message.size());
  } catch (...) {
    *error = DznBytes{nullptr, 0};
  }
}

// Runs fn, turning any exception into a 0 return and an error message.
template <typename Fn>
int guarded(DznBytes* error, Fn&& fn) {
  try {
    fn();
    return 1;
  } catch (const std::exception& e) {
    setError(error, e.what());
  } catch (const dznav::FormatError& e) {
    setError(error, e.message);
  } catch (const dznav::WireError& e) {
    setError(error, e.message);
  } catch (...) {
    setError(error, "unknown error in the navmesh core");
  }
  return 0;
}

dznav::WorldParams toWorldParams(const DznWorldParams& p) {
  dznav::WorldParams wp;
  std::memcpy(&wp.block, p.block, sizeof(wp.block));
  wp.cellClasses.resize(p.cell_class_count);
  if (p.cell_class_count)
    std::memcpy(wp.cellClasses.data(), p.cell_classes, p.cell_class_count * sizeof(dznav::CellClass));
  wp.links.resize(p.link_count);
  if (p.link_count) std::memcpy(wp.links.data(), p.links, p.link_count * sizeof(dznav::LinkClass));
  return wp;
}

dznav::TileInput toTileInput(const DznTileInput& in) {
  dznav::TileInput t;
  t.x = in.x;
  t.z = in.z;
  t.cellClass = in.cell_class;
  std::memcpy(t.bmin, in.bmin, sizeof(t.bmin));
  std::memcpy(t.bmax, in.bmax, sizeof(t.bmax));
  t.groups.resize(in.group_count);
  for (uint32_t i = 0; i < in.group_count; ++i) {
    const DznInputGroup& src = in.groups[i];
    dznav::InputGroup& g = t.groups[i];
    g.area = src.area;
    g.parts.resize(src.part_count);
    for (uint32_t k = 0; k < src.part_count; ++k) {
      g.parts[k].partIndex = src.parts[k].part_index;
      g.parts[k].objectId = src.parts[k].object_id;
      g.parts[k].firstVertex = src.parts[k].first_vertex;
    }
    g.verts.assign(src.verts, src.verts + src.vert_count * 3);
  }
  return t;
}

unsigned char* copyToDetour(const uint8_t* data, size_t len) {
  auto* p = static_cast<unsigned char*>(dtAlloc(int(len), DT_ALLOC_PERM));
  if (!p) throw std::bad_alloc();
  std::memcpy(p, data, len);
  return p;
}

void addTile(dtNavMesh& nav, const uint8_t* data, size_t len, dtTileRef lastRef, dtTileRef* result) {
  if (len < sizeof(dtMeshHeader) || len > size_t(INT32_MAX)) throw std::runtime_error("invalid tile data size");
  unsigned char* copy = copyToDetour(data, len);
  const dtStatus st = nav.addTile(copy, int(len), DT_TILE_FREE_DATA, lastRef, result);
  if (dtStatusFailed(st)) {
    dtFree(copy);
    char message[48];
    std::snprintf(message, sizeof message, "addTile failed (status 0x%x)", unsigned(st));
    throw std::runtime_error(message);
  }
}

}  // namespace

extern "C" {

void dzn_bytes_free(DznBytes bytes) { std::free(bytes.data); }

int dzn_build_tile(const DznWorldParams* params, const DznTileInput* input, const DznTileBuildOptions* options,
                   DznBytes* tile, DznTileBuildStats* stats, DznBytes* error) {
  *tile = DznBytes{nullptr, 0};
  return guarded(error, [&] {
    const dznav::WorldParams wp = toWorldParams(*params);
    const dznav::TileInput in = toTileInput(*input);
    dznav::TileBuildOptions bo;
    bo.jumpLinks = options->jump_links != 0;
    bo.doors = options->doors == DZN_DOORS_OPEN      ? dznav::DoorStatePolicy::Open
               : options->doors == DZN_DOORS_ENABLED ? dznav::DoorStatePolicy::Enabled
                                                     : dznav::DoorStatePolicy::Closed;
    bo.seaLevel = options->sea_level;
    dznav::TileBuildStats s;
    s.x = in.x;
    s.z = in.z;
    const std::vector<uint8_t> blob = dznav::buildTile(wp, in, s, bo);
    *tile = copyBytes(blob.data(), blob.size());
    stats->data_size = s.dataSize;
    stats->polys = s.polys;
    stats->verts = s.verts;
    stats->off_mesh = s.offMesh;
    stats->obstacles = s.obstacles;
    stats->stray_part_ids = s.strayPartIds;
  });
}

DznNavMesh* dzn_navmesh_new(const DznNavMeshParams* params, DznBytes* error) {
  DznNavMesh* mesh = nullptr;
  guarded(error, [&] {
    dtNavMeshParams p{};
    std::memcpy(p.orig, params->orig, sizeof(p.orig));
    p.tileWidth = params->tile_width;
    p.tileHeight = params->tile_height;
    p.maxTiles = params->max_tiles;
    p.maxPolys = params->max_polys;
    dtNavMesh* nav = dtAllocNavMesh();
    if (!nav) throw std::bad_alloc();
    if (dtStatusFailed(nav->init(&p))) {
      dtFreeNavMesh(nav);
      throw std::runtime_error("dtNavMesh::init failed");
    }
    mesh = new DznNavMesh{nav};
  });
  return mesh;
}

void dzn_navmesh_free(DznNavMesh* mesh) {
  if (!mesh) return;
  dtFreeNavMesh(mesh->nav);
  delete mesh;
}

void dzn_navmesh_params(const DznNavMesh* mesh, DznNavMeshParams* out) {
  const dtNavMeshParams* p = mesh->nav->getParams();
  std::memcpy(out->orig, p->orig, sizeof(out->orig));
  out->tile_width = p->tileWidth;
  out->tile_height = p->tileHeight;
  out->max_tiles = p->maxTiles;
  out->max_polys = p->maxPolys;
}

int dzn_navmesh_add_tile(DznNavMesh* mesh, const uint8_t* data, size_t len, uint64_t ref, uint64_t* out_ref,
                         DznBytes* error) {
  return guarded(error, [&] {
    dtTileRef result = 0;
    addTile(*mesh->nav, data, len, ref, &result);
    *out_ref = result;
  });
}

int dzn_navmesh_set_tile(DznNavMesh* mesh, const uint8_t* data, size_t len, uint64_t* out_ref, uint64_t* replaced_ref,
                         DznBytes* error) {
  *replaced_ref = 0;
  return guarded(error, [&] {
    dtNavMesh& nav = *mesh->nav;
    dtMeshHeader mh;
    if (len < sizeof(mh)) throw std::runtime_error("tile data too small");
    std::memcpy(&mh, data, sizeof(mh));

    // Reuse the slot of the tile being replaced, with a new salt, so tile order is stable.
    dtTileRef lastRef = 0;
    if (const dtMeshTile* old = nav.getTileAt(mh.x, mh.y, mh.layer)) {
      const dtTileRef oldRef = nav.getTileRef(old);
      if (dtStatusFailed(nav.removeTile(oldRef, nullptr, nullptr))) throw std::runtime_error("removeTile failed");
      *replaced_ref = oldRef;
      const unsigned int index = nav.decodePolyIdTile(oldRef);
      lastRef = nav.encodePolyId(static_cast<const dtNavMesh&>(nav).getTile(int(index))->salt, index, 0);
    }
    dtTileRef result = 0;
    addTile(nav, data, len, lastRef, &result);
    *out_ref = result;
  });
}

int dzn_navmesh_remove_tile(DznNavMesh* mesh, uint64_t ref) {
  return dtStatusSucceed(mesh->nav->removeTile(ref, nullptr, nullptr)) ? 1 : 0;
}

uint64_t dzn_navmesh_tile_ref_at(const DznNavMesh* mesh, int32_t x, int32_t z) {
  const dtNavMesh& nav = *mesh->nav;
  const dtMeshTile* t = nav.getTileAt(x, z, 0);
  return t ? nav.getTileRef(t) : 0;
}

int32_t dzn_navmesh_max_tiles(const DznNavMesh* mesh) { return mesh->nav->getMaxTiles(); }

int dzn_navmesh_tile(const DznNavMesh* mesh, int32_t index, uint64_t* ref, const uint8_t** data, size_t* len) {
  const dtNavMesh& nav = *mesh->nav;
  if (index < 0 || index >= nav.getMaxTiles()) return 0;
  const dtMeshTile* t = nav.getTile(index);
  if (!t || !t->header) return 0;
  *ref = nav.getTileRef(t);
  *data = t->data;
  *len = size_t(t->dataSize);
  return 1;
}

int dzn_navmesh_filter_islands(DznNavMesh* mesh, const float seed[3], int search_all_heights, DznIslandResult* out,
                               DznBytes* error) {
  *out = DznIslandResult{};
  return guarded(error, [&] {
    const dtNavMesh& nav = *mesh->nav;
    // Clear previous results.
    for (int i = 0; i < nav.getMaxTiles(); ++i) {
      const dtMeshTile* t = nav.getTile(i);
      if (!t || !t->header) continue;
      for (int p = 0; p < t->header->polyCount; ++p) t->polys[p].flags &= ~dznav::kIsolatedFlag;
    }

    dtNavMeshQuery* query = dtAllocNavMeshQuery();
    if (!query || dtStatusFailed(query->init(mesh->nav, 2048))) {
      dtFreeNavMeshQuery(query);
      throw std::runtime_error("dtNavMeshQuery::init failed");
    }
    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    filter.setExcludeFlags(0x8002);
    const float extents[3] = {30.0f, search_all_heights ? 10000.0f : 30.0f, 3.0f};
    dtPolyRef start = 0;
    query->findNearestPoly(seed, extents, &filter, &start, out->seed_point);
    dtFreeNavMeshQuery(query);
    if (!start) return;
    out->seed_found = 1;

    // Depth-first walk over every link, regardless of flags (closed doors are traversed).
    std::vector<std::vector<uint8_t>> visited(size_t(nav.getMaxTiles()));
    for (int i = 0; i < nav.getMaxTiles(); ++i) {
      const dtMeshTile* t = nav.getTile(i);
      if (t && t->header) visited[i].assign(size_t(t->header->polyCount), 0);
    }
    std::vector<dtPolyRef> stack{start};
    visited[nav.decodePolyIdTile(start)][nav.decodePolyIdPoly(start)] = 1;
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
          ++out->reachable;
        } else {
          t->polys[p].flags |= dznav::kIsolatedFlag;
          ++out->isolated;
        }
      }
    }
  });
}

}  // extern "C"

// Every allocation of the core goes through mimalloc (linked in by the libmimalloc-sys crate): Recast and Detour
// through their allocator hooks, and the rest through the global operator new. Builder threads allocate and free a
// lot, and the default heap made them wait on each other: dense tiles build about 10% faster.
extern "C" void* mi_malloc(size_t size);
extern "C" void mi_free(void* p);

namespace {

void* recastAlloc(size_t size, rcAllocHint) { return mi_malloc(size); }
void* detourAlloc(size_t size, dtAllocHint) { return mi_malloc(size); }
// Set before main runs, so nothing is allocated with one allocator and freed with the other.
const bool kAllocatorSet = (rcAllocSetCustom(recastAlloc, mi_free), dtAllocSetCustom(detourAlloc, mi_free), true);

}  // namespace

void* operator new(size_t size) {
  if (void* p = mi_malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { mi_free(p); }
void operator delete[](void* p) noexcept { mi_free(p); }
void operator delete(void* p, size_t) noexcept { mi_free(p); }
void operator delete[](void* p, size_t) noexcept { mi_free(p); }
