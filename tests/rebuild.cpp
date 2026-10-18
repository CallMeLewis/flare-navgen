// Reconstructs each shipped tile's Recast poly mesh (voxel coordinates, neighbour codes, areas,
// flags, off-mesh connections, extra section), rebuilds the tile with our patched
// dtCreateNavMeshData, links all tiles and compares with the file. Identical bytes mean our
// builder writes tiles exactly like DayZ's, and that the cell height hypothesis holds.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "DetourAlloc.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "dznav/NavMeshFormat.h"

using namespace dznav;

namespace {

constexpr float kCellHeight = 0.1f;
constexpr unsigned short kNull = 0xffff;

struct Rebuilt {
  std::vector<uint8_t> data;
  std::string error;
};

Rebuilt rebuildTile(const Tile& t, const std::vector<const OffMeshConnection*>& incoming,
                    const std::vector<const Tile*>& incomingTiles, const std::vector<int>& incomingPoly) {
  const MeshHeader& h = t.header;
  const float cs = 1.0f / h.bvQuantFactor;
  const int ground = h.offMeshBase;
  const int groundVerts = h.vertCount - 2 * h.offMeshConCount;
  Rebuilt out;

  std::vector<unsigned short> verts(size_t(groundVerts) * 3);
  for (int i = 0; i < groundVerts; ++i) {
    const float* v = &t.verts[size_t(i) * 3];
    const long vx = std::lround((v[0] - h.bmin[0]) / cs);
    const long vy = std::lround((v[1] - h.bmin[1]) / kCellHeight);
    const long vz = std::lround((v[2] - h.bmin[2]) / cs);
    verts[i * 3 + 0] = (unsigned short)vx;
    verts[i * 3 + 1] = (unsigned short)vy;
    verts[i * 3 + 2] = (unsigned short)vz;
    if (h.bmin[0] + vx * cs != v[0] || h.bmin[1] + vy * kCellHeight != v[1] || h.bmin[2] + vz * cs != v[2]) {
      out.error = "vertex not on the cs/ch grid";
      return out;
    }
  }

  const int nvp = kVertsPerPoly;
  std::vector<unsigned short> polys(size_t(ground) * nvp * 2, kNull);
  std::vector<unsigned short> flags(ground);
  std::vector<unsigned char> areas(ground);
  for (int i = 0; i < ground; ++i) {
    const Poly& p = t.polys[i];
    unsigned short* dst = &polys[size_t(i) * nvp * 2];
    for (int j = 0; j < p.vertCount; ++j) {
      dst[j] = p.verts[j];
      const uint16_t n = p.neis[j];
      unsigned short src;
      if (n == 0) src = kNull;
      else if (n & kExtLink) {
        switch (n & 0xff) {
          case 4: src = 0x8000 | 0; break;
          case 2: src = 0x8000 | 1; break;
          case 0: src = 0x8000 | 2; break;
          case 6: src = 0x8000 | 3; break;
          default: out.error = "unexpected portal side"; return out;
        }
      } else src = n - 1;
      dst[nvp + j] = src;
    }
    flags[i] = p.flags;
    areas[i] = p.area();
  }

  // Off-mesh connections: stored ones first (they start in this tile), then those arriving from
  // neighbours, which only reserve links.
  std::vector<float> omVerts, omRad, omCentre;
  std::vector<unsigned char> omDir, omArea;
  std::vector<unsigned short> omFlags;
  std::vector<unsigned int> omUser;
  auto addCon = [&](const OffMeshConnection& c, const Poly& poly) {
    omVerts.insert(omVerts.end(), c.pos, c.pos + 6);
    omRad.push_back(c.rad);
    omCentre.push_back(c.centerHeight);
    omDir.push_back(c.flags & 1);
    omArea.push_back(poly.area());
    omFlags.push_back(poly.flags);
    omUser.push_back(c.userId);
  };
  for (const OffMeshConnection& c : t.offMeshCons) addCon(c, t.polys[c.poly]);
  for (size_t i = 0; i < incoming.size(); ++i) addCon(*incoming[i], incomingTiles[i]->polys[incomingPoly[i]]);

  dtNavMeshCreateParams p{};
  p.verts = verts.data();
  p.vertCount = groundVerts;
  p.polys = polys.data();
  p.polyFlags = flags.data();
  p.polyAreas = areas.data();
  p.polyCount = ground;
  p.nvp = nvp;
  p.offMeshConVerts = omVerts.empty() ? nullptr : omVerts.data();
  p.offMeshConRad = omRad.empty() ? nullptr : omRad.data();
  p.offMeshConCenterHeight = omCentre.empty() ? nullptr : omCentre.data();
  p.offMeshConDir = omDir.empty() ? nullptr : omDir.data();
  p.offMeshConAreas = omArea.empty() ? nullptr : omArea.data();
  p.offMeshConFlags = omFlags.empty() ? nullptr : omFlags.data();
  p.offMeshConUserID = omUser.empty() ? nullptr : omUser.data();
  p.offMeshConCount = int(omRad.size());
  std::vector<uint8_t> extra;
  {
    Tile tmp;
    tmp.parts = t.parts;
    tmp.partPolys = t.partPolys;
    tmp.obstacles = t.obstacles;
    syncTileHeader(tmp);
    const std::vector<uint8_t> all = serializeTile(tmp);
    extra.assign(all.end() - tmp.extraDataSize(), all.end());
  }
  p.extraData = extra.data();
  p.extraDataSize = int(extra.size());
  p.userId = h.userId;
  p.tileX = h.x;
  p.tileY = h.y;
  p.tileLayer = h.layer;
  std::memcpy(p.bmin, h.bmin, sizeof(p.bmin));
  std::memcpy(p.bmax, h.bmax, sizeof(p.bmax));
  p.walkableHeight = h.walkableHeight;
  p.walkableRadius = h.walkableRadius;
  p.walkableClimb = h.walkableClimb;
  p.cs = cs;
  p.ch = kCellHeight;
  p.buildBvTree = h.bvNodeCount > 0;

  unsigned char* data = nullptr;
  int size = 0;
  if (!dtCreateNavMeshData(&p, &data, &size)) {
    out.error = "dtCreateNavMeshData failed";
    return out;
  }
  out.data.assign(data, data + size);
  dtFree(data);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: rebuild <file.nm>\n");
    return 2;
  }
  NavMeshFile f = readNavMeshFile(argv[1]);
  std::map<std::pair<int, int>, const Tile*> byPos;
  for (const Tile& t : f.tiles) byPos[{t.header.x, t.header.y}] = &t;

  // Off-mesh connections ending in each tile but starting in a neighbour.
  std::map<std::pair<int, int>, std::vector<std::pair<const Tile*, int>>> incoming;
  for (const Tile& t : f.tiles)
    for (size_t i = 0; i < t.offMeshCons.size(); ++i) {
      const OffMeshConnection& c = t.offMeshCons[i];
      if (c.side == 0xff) continue;
      const int ex = int(std::floor((c.pos[3] - f.header.orig[0]) / f.header.tileWidth));
      const int ez = int(std::floor((c.pos[5] - f.header.orig[2]) / f.header.tileHeight));
      incoming[{ex, ez}].push_back({&t, int(i)});
    }

  dtNavMeshParams np{};
  std::memcpy(np.orig, f.header.orig, sizeof(np.orig));
  np.tileWidth = f.header.tileWidth;
  np.tileHeight = f.header.tileHeight;
  np.maxTiles = f.header.maxTiles;
  np.maxPolys = f.header.maxPolys;
  dtNavMesh* nav = dtAllocNavMesh();
  nav->init(&np);

  std::map<std::string, int> errors;
  std::vector<std::vector<uint8_t>> original;
  for (const Tile& t : f.tiles) {
    std::vector<const OffMeshConnection*> in;
    std::vector<const Tile*> inTiles;
    std::vector<int> inPoly;
    for (auto [src, idx] : incoming[{t.header.x, t.header.y}]) {
      in.push_back(&src->offMeshCons[idx]);
      inTiles.push_back(src);
      inPoly.push_back(src->offMeshCons[idx].poly);
    }
    Rebuilt r = rebuildTile(t, in, inTiles, inPoly);
    if (!r.error.empty()) {
      errors[r.error]++;
      r.data = serializeTile(t);  // keep the mesh complete
    }
    auto* data = static_cast<unsigned char*>(dtAlloc(int(r.data.size()), DT_ALLOC_PERM));
    std::memcpy(data, r.data.data(), r.data.size());
    if (dtStatusFailed(nav->addTile(data, int(r.data.size()), DT_TILE_FREE_DATA, t.ref, nullptr))) {
      std::fprintf(stderr, "addTile failed\n");
      return 1;
    }
  }
  for (auto& [e, n] : errors) std::printf("  reconstruction error '%s': %d tiles\n", e.c_str(), n);

  int same = 0, sameIgnoringObstacleY = 0, sizeDiff = 0, other = 0, shown = 0;
  for (const Tile& t : f.tiles) {
    const dtMeshTile* mt = nav->getTileByRef(t.ref);
    std::vector<uint8_t> orig = serializeTile(t);
    if (size_t(mt->dataSize) != orig.size()) {
      ++sizeDiff;
      if (shown++ < 5)
        std::printf("tile (%d,%d): size %d vs %zu\n", t.header.x, t.header.y, mt->dataSize, orig.size());
      continue;
    }
    if (std::memcmp(mt->data, orig.data(), orig.size()) == 0) {
      ++same;
      continue;
    }
    Tile a = t, b = parseTile(std::span<const uint8_t>(mt->data, size_t(mt->dataSize)), t.ref);
    for (auto& o : a.obstacles) o.pos[1] = 0;
    for (auto& o : b.obstacles) o.pos[1] = 0;
    if (serializeTile(a) == serializeTile(b)) {
      ++sameIgnoringObstacleY;
      continue;
    }
    ++other;
    if (shown++ < 5) {
      size_t k = 0;
      while (mt->data[k] == orig[k]) ++k;
      std::printf("tile (%d,%d): first diff at byte %zu of %zu\n", t.header.x, t.header.y, k, orig.size());
    }
  }
  std::printf("%d identical, %d identical except obstacle Y, %d size differs, %d other differences (of %zu)\n", same,
              sameIgnoringObstacleY, sizeDiff, other, f.tiles.size());
  dtFreeNavMesh(nav);
  return (sizeDiff || other) ? 1 : 0;
}
