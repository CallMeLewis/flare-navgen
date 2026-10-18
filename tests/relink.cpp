// Loads every tile of a shipped .nm into the patched dtNavMesh with the link section zeroed, lets
// Detour rebuild all links, and compares the resulting tile bytes with the file. Identical bytes
// mean our Detour patch links tiles exactly like DayZ's.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "DetourAlloc.h"
#include "DetourNavMesh.h"
#include "dznav/NavMeshFormat.h"

using namespace dznav;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: relink <file.nm>\n");
    return 2;
  }
  std::vector<uint8_t> bytes = readFileBytes(argv[1]);
  SetHeader h;
  std::memcpy(&h, bytes.data(), sizeof(h));

  dtNavMeshParams params;
  std::memcpy(params.orig, h.orig, sizeof(params.orig));
  params.tileWidth = h.tileWidth;
  params.tileHeight = h.tileHeight;
  params.maxTiles = h.maxTiles;
  params.maxPolys = h.maxPolys;
  dtNavMesh* nav = dtAllocNavMesh();
  if (dtStatusFailed(nav->init(&params))) {
    std::fprintf(stderr, "init failed\n");
    return 1;
  }

  struct Rec {
    size_t pos;
    uint64_t ref;
    int size;
  };
  std::vector<Rec> recs;
  size_t pos = sizeof(SetHeader) + size_t(h.maxTiles) * 4;
  for (int i = 0; i < h.numTiles; ++i) {
    TileRecordHeader r;
    std::memcpy(&r, bytes.data() + pos, sizeof(r));
    recs.push_back({pos + sizeof(r), r.tileRef, r.dataSize});
    pos += sizeof(r) + r.dataSize;
  }

  size_t added = 0;
  for (const Rec& r : recs) {
    if (added++ % 20000 == 0) {
      std::fprintf(stderr, "adding %zu\n", added - 1);
      std::fflush(stderr);
    }
    auto* data = static_cast<unsigned char*>(dtAlloc(r.size, DT_ALLOC_PERM));
    std::memcpy(data, bytes.data() + r.pos, r.size);
    const auto* mh = reinterpret_cast<const dtMeshHeader*>(data);
    const size_t linksOff = sizeof(dtMeshHeader) + size_t(mh->vertCount) * 12 + size_t(mh->polyCount) * sizeof(dtPoly);
    std::memset(data + linksOff, 0, size_t(mh->maxLinkCount) * sizeof(dtLink));
    dtTileRef result = 0;
    const dtStatus st = nav->addTile(data, r.size, DT_TILE_FREE_DATA, r.ref, &result);
    if (dtStatusFailed(st) || result != r.ref) {
      std::fprintf(stderr, "addTile failed (status 0x%x)\n", st);
      return 1;
    }
  }

  size_t mismatched = 0, shown = 0, obstacleYOnly = 0;
  std::map<std::string, size_t> bySection;
  for (size_t i = 0; i < recs.size(); ++i) {
    const dtMeshTile* t = nav->getTileByRef(recs[i].ref);
    const uint8_t* orig = bytes.data() + recs[i].pos;
    if (std::memcmp(t->data, orig, recs[i].size) == 0) continue;
    ++mismatched;
    // Name every section that differs.
    const dtMeshHeader* mh = t->header;
    const std::pair<const char*, size_t> sections[] = {
        {"header", sizeof(dtMeshHeader)},
        {"verts", size_t(mh->vertCount) * 12},
        {"polys", size_t(mh->polyCount) * sizeof(dtPoly)},
        {"links", size_t(mh->maxLinkCount) * sizeof(dtLink)},
        {"detail", size_t(mh->detailMeshCount) * 12 + size_t(mh->detailVertCount) * 12 + size_t(mh->detailTriCount) * 4},
        {"bv", size_t(mh->bvNodeCount) * sizeof(dtBVNode)},
        {"offmesh", size_t(mh->offMeshConCount) * sizeof(dtOffMeshConnection)},
        {"extra", size_t(mh->extraDataSize)},
    };
    std::string key;
    size_t off = 0;
    for (const auto& [name, size] : sections) {
      if (size && std::memcmp(t->data + off, orig + off, size) != 0) key += std::string(key.empty() ? "" : "+") + name;
      off += size;
    }
    // addTile snaps obstacle Y to the mesh; re-snapping an already snapped Y isn't always
    // idempotent, so accept tiles whose only difference is obstacle Y.
    if (key == "extra") {
      Tile a = parseTile(std::span<const uint8_t>(orig, recs[i].size), recs[i].ref);
      Tile b = parseTile(std::span<const uint8_t>(t->data, recs[i].size), recs[i].ref);
      for (auto& o : a.obstacles) o.pos[1] = 0;
      for (auto& o : b.obstacles) o.pos[1] = 0;
      if (serializeTile(a) == serializeTile(b)) {
        ++obstacleYOnly;
        --mismatched;
        continue;
      }
    }
    bySection[key]++;
    if (shown++ < 5) {
      size_t k = 0;
      while (t->data[k] == orig[k]) ++k;
      std::printf("tile %zu (%d,%d): first diff at byte %zu of %d [%s]\n", i, mh->x, mh->y, k, recs[i].size,
                  key.c_str());
    }
  }
  for (const auto& [key, n] : bySection) std::printf("  differs in %s: %zu tiles\n", key.c_str(), n);
  std::printf("%zu / %zu tiles differ after relinking (%zu more differ only in re-snapped obstacle Y)\n", mismatched,
              recs.size(), obstacleYOnly);
  // Optional: relink <file.nm> <tileIndex> <out.bin> dumps the relinked bytes of one tile.
  if (argc >= 4) {
    const size_t i = size_t(std::atoi(argv[2]));
    const dtMeshTile* t = nav->getTileByRef(recs[i].ref);
    writeFileBytes(argv[3], std::span<const uint8_t>(t->data, size_t(recs[i].size)));
  }
  dtFreeNavMesh(nav);
  return mismatched ? 1 : 0;
}
