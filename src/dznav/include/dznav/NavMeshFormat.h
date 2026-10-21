// DayZ navmesh file format (.nm), as read by the game.
//
// The container is the RecastDemo "tile mesh set" layout extended with a grid size and a tile
// offset table. Tiles are Detour tile blobs built with DT_POLYREF64 (16-byte links), with a
// DayZ-specific tile version (12), wider off-mesh connections and a trailing "extra" section
// holding door/part polygon lists and cylinder obstacles. See docs/format/nm-format.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dznav {

constexpr uint32_t kSetMagic = 'M' << 24 | 'S' << 16 | 'E' << 8 | 'T';  // bytes "TESM"
constexpr int32_t kSetVersion = 9;     // DayZ 1.30+; the game loads no other version
constexpr int32_t kSetVersionOld = 7;  // DayZ 1.29 and earlier: same layout, still read (and saved as 9)
constexpr uint32_t kTileMagic = 'D' << 24 | 'N' << 16 | 'A' << 8 | 'V';  // bytes "VAND"
constexpr int32_t kTileVersion = 12;
constexpr int kVertsPerPoly = 6;
constexpr uint16_t kExtLink = 0x8000;
constexpr uint16_t kIsolatedFlag = 0x8000;  // poly flag: unreachable from the island filter seed
constexpr uint32_t kNullLink = 0xffffffff;

// 64-bit poly ref layout (Detour DT_POLYREF64 defaults).
constexpr int kSaltBits = 16;
constexpr int kTileBits = 28;
constexpr int kPolyBits = 20;

constexpr uint64_t encodePolyRef(uint32_t salt, uint32_t tileIndex, uint32_t polyIndex) {
  return (uint64_t(salt) << (kPolyBits + kTileBits)) | (uint64_t(tileIndex) << kPolyBits) | polyIndex;
}
constexpr uint32_t refSalt(uint64_t ref) { return uint32_t(ref >> (kPolyBits + kTileBits)) & ((1u << kSaltBits) - 1); }
constexpr uint32_t refTile(uint64_t ref) { return uint32_t(ref >> kPolyBits) & ((1u << kTileBits) - 1); }
constexpr uint32_t refPoly(uint64_t ref) { return uint32_t(ref) & ((1u << kPolyBits) - 1); }

#pragma pack(push, 1)

struct SetHeader {
  uint32_t magic;
  int32_t version;
  int32_t numTiles;
  int32_t tilesX;  // grid width in tiles
  int32_t tilesZ;  // grid height in tiles
  // dtNavMeshParams
  float orig[3];
  float tileWidth;
  float tileHeight;
  int32_t maxTiles;
  int32_t maxPolys;
};
static_assert(sizeof(SetHeader) == 48);

// Per-tile record header. Matches RecastDemo's NavMeshTileHeader { dtTileRef; int dataSize; }
// with 64-bit refs, including the 4 bytes of struct padding (0 in shipped maps, other values in
// some saves; the game ignores it).
struct TileRecordHeader {
  uint64_t tileRef;
  int32_t dataSize;
  uint32_t pad;
};

// Offset table entry for a tile slot that has no tile (partial navmeshes).
constexpr uint32_t kNoTile = 0xffffffff;
static_assert(sizeof(TileRecordHeader) == 16);

struct MeshHeader {
  uint32_t magic;
  int32_t version;
  int32_t x;
  int32_t y;  // grid row; world Z = orig[2] + y * tileHeight
  int32_t layer;
  uint32_t userId;
  int32_t polyCount;
  int32_t vertCount;
  int32_t maxLinkCount;
  int32_t detailMeshCount;
  int32_t detailVertCount;
  int32_t detailTriCount;
  int32_t bvNodeCount;
  int32_t offMeshConCount;
  int32_t offMeshBase;
  float walkableHeight;
  float walkableRadius;
  float walkableClimb;
  float bmin[3];
  float bmax[3];
  float bvQuantFactor;  // 1 / cellSize used for this tile
  int32_t extraDataSize;  // DayZ: byte size of the trailing extra section
};
static_assert(sizeof(MeshHeader) == 104);

struct Poly {
  uint32_t firstLink;
  uint16_t verts[kVertsPerPoly];
  uint16_t neis[kVertsPerPoly];
  uint16_t flags;
  uint8_t vertCount;
  uint8_t areaAndType;  // low 6 bits area, high 2 bits type (0 ground, 1 off-mesh)

  uint8_t area() const { return areaAndType & 0x3f; }
  uint8_t type() const { return areaAndType >> 6; }
};
static_assert(sizeof(Poly) == 32);

struct Link {
  uint64_t ref;
  uint32_t next;
  uint8_t edge;
  uint8_t side;
  uint8_t bmin;
  uint8_t bmax;
};
static_assert(sizeof(Link) == 16);

struct PolyDetail {
  uint32_t vertBase;
  uint32_t triBase;
  uint8_t vertCount;
  uint8_t triCount;
  uint8_t pad[2];
};
static_assert(sizeof(PolyDetail) == 12);

struct BVNode {
  uint16_t bmin[3];
  uint16_t bmax[3];
  int32_t i;  // >= 0 leaf poly index, < 0 escape offset
};
static_assert(sizeof(BVNode) == 16);

// Stock Detour is 36 bytes; DayZ has three floats after pos[6].
struct OffMeshConnection {
  float pos[6];
  float centerHeight;  // measured obstacle height at the link midpoint (can be < 0); not used by Detour
  float reserved;      // 0 when built, set to 1.0 by addTile
  float rad;           // endpoint snap radius
  uint16_t poly;
  uint8_t flags;  // 1 = bidirectional
  uint8_t side;   // 0xff = internal, else neighbour tile side
  uint32_t userId;  // index of the link class in GenParams/Links
};
static_assert(sizeof(OffMeshConnection) == 44);

// Extra section: per-object part (door) polygon lists.
struct TilePart {
  uint64_t objectId;
  uint32_t partIndex;  // 2 * part + state
  int32_t firstPoly;   // index into partPolys, -1 if none
};
static_assert(sizeof(TilePart) == 16);

struct TilePartPoly {
  uint32_t poly;
  int32_t next;  // index into partPolys, -1 terminates
};
static_assert(sizeof(TilePartPoly) == 8);

struct Obstacle {
  float pos[3];
  float radius;
};
static_assert(sizeof(Obstacle) == 16);

#pragma pack(pop)

// Fully decoded tile. Serializing a parsed tile reproduces the original bytes.
struct Tile {
  uint64_t ref = 0;
  uint32_t recordPad = 0;  // record header padding, kept for exact round trips
  MeshHeader header{};
  std::vector<float> verts;  // 3 per vertex
  std::vector<Poly> polys;
  std::vector<Link> links;
  std::vector<PolyDetail> detailMeshes;
  std::vector<float> detailVerts;
  std::vector<uint8_t> detailTris;  // 4 per triangle
  std::vector<BVNode> bvTree;
  std::vector<OffMeshConnection> offMeshCons;
  std::vector<TilePart> parts;
  std::vector<TilePartPoly> partPolys;
  std::vector<Obstacle> obstacles;

  size_t extraDataSize() const;
  size_t dataSize() const;
};

// The offset table has maxTiles entries indexed by tile slot (refTile(ref)); slots without a tile
// hold kNoTile. Records follow the table back to back, numTiles of them; the game reads them
// sequentially and ignores the table.
struct NavMeshFile {
  SetHeader header{};
  std::vector<Tile> tiles;  // in file order
};

struct FormatError {
  std::string message;
};

// Parses one tile blob (the bytes after the record header). Throws FormatError.
Tile parseTile(std::span<const uint8_t> data, uint64_t ref);
// Serializes a tile blob. Updates no header fields: callers keep counts consistent.
std::vector<uint8_t> serializeTile(const Tile& tile);
// Recomputes header counts and extraDataSize from the vectors.
void syncTileHeader(Tile& tile);

NavMeshFile parseNavMesh(std::span<const uint8_t> bytes);
std::vector<uint8_t> serializeNavMesh(const NavMeshFile& file);

NavMeshFile readNavMeshFile(const std::string& path);
void writeNavMeshFile(const std::string& path, const NavMeshFile& file);

std::vector<uint8_t> readFileBytes(const std::string& path);
void writeFileBytes(const std::string& path, std::span<const uint8_t> bytes);

}  // namespace dznav
