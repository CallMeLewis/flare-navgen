// Self-contained format tests (no game data required).
#include <cstdio>
#include <cstring>

#include "dznav/NavMeshFormat.h"

using namespace dznav;

static int g_failures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

static Tile makeTile(int x, int y) {
  Tile t;
  t.header.x = x;
  t.header.y = y;
  t.header.walkableHeight = 1.5f;
  t.header.walkableRadius = 0.3f;
  t.header.walkableClimb = 0.45f;
  t.header.bvQuantFactor = 4.0f;
  t.header.offMeshBase = 1;
  t.ref = encodePolyRef(1, uint32_t(y * 2 + x), 0);
  t.verts = {0, 0, 0, 50, 0, 0, 50, 0, 50};
  Poly p{};
  p.verts[0] = 0;
  p.verts[1] = 1;
  p.verts[2] = 2;
  p.vertCount = 3;
  p.flags = 1;
  p.areaAndType = 1;
  t.polys.push_back(p);
  t.links.resize(3);
  t.bvTree.resize(2);
  t.parts.push_back({0x8000f0000501f61full, 0, 0});
  t.partPolys.push_back({0, -1});
  t.obstacles.push_back({{10, 0, 10}, 0.3f});
  syncTileHeader(t);
  return t;
}

int main() {
  CHECK(refSalt(encodePolyRef(1, 5, 7)) == 1);
  CHECK(refTile(encodePolyRef(1, 5, 7)) == 5);
  CHECK(refPoly(encodePolyRef(1, 5, 7)) == 7);
  CHECK(encodePolyRef(1, 1, 0) == 0x0001000000100000ull);

  NavMeshFile f;
  f.header.tilesX = 2;
  f.header.tilesZ = 1;
  f.header.tileWidth = f.header.tileHeight = 50.0f;
  f.header.maxTiles = 2;
  f.header.maxPolys = 1 << kPolyBits;
  f.tiles.push_back(makeTile(0, 0));
  f.tiles.push_back(makeTile(1, 0));

  std::vector<uint8_t> bytes = serializeNavMesh(f);
  CHECK(std::memcmp(bytes.data(), "TESM", 4) == 0);
  NavMeshFile g = parseNavMesh(bytes);
  CHECK(g.tiles.size() == 2);
  CHECK(g.tiles[1].header.x == 1);
  CHECK(g.tiles[0].obstacles.size() == 1);
  CHECK(g.tiles[0].parts.at(0).objectId == 0x8000f0000501f61full);
  CHECK(serializeNavMesh(g) == bytes);
  CHECK(int(g.tiles[0].header.extraDataSize) == 12 + 16 + 8 + 16);

  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("all format tests passed\n");
  return 0;
}
