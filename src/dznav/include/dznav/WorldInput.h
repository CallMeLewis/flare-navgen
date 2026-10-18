// Generation input as served by DayZDiag's navmesh data server (`-startNavmeshDataServer`):
// world parameters and per-tile triangle soup. See docs/protocol/data-server.md.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace dznav {

#pragma pack(push, 1)
// Tile cell resolution class. cellSize = tileWidth / cells.
struct CellClass {
  int32_t cells;
  uint32_t unk04;
  float tileWidth;
  uint32_t unk0C;
};
static_assert(sizeof(CellClass) == 16);

// One GenParams/Links class (off-mesh jump/climb link type), in config order.
struct LinkClass {
  uint32_t index;  // -> dtOffMeshConnection::userId
  uint32_t nameHash;
  int32_t typeId;  // -1 when not set
  float jumpHeight;
  float jumpLength;
  float jumpDropdownMin;
  float jumpDropdownMax;
  float minCenterHeight;
  float agentHeight;
  float maxStepHeight;
  float agentRadius;
  uint16_t flags;
  uint8_t area;
  uint8_t pad;
  int32_t color;
};
static_assert(sizeof(LinkClass) == 0x34);

// The fixed 0x54-byte block of world parameters (wire order).
struct WorldParamsBlock {
  float worldSizeX;
  float worldSizeZ;
  float tileWidth;
  float tileHeight;
  int32_t tilesX;
  int32_t tilesZ;
  float deepSeaHeight;    // top of the waterdeepsea box (area 5)
  float deepWaterDepth;   // depth at which water becomes waterdeep (area 3)
  float contourMaxError;  // metres
  uint8_t obstacles;      // extract area-10 parts as cylinder obstacles
  uint8_t pad25[3];
  int32_t blockSize;      // scheduler block size in tiles
  uint8_t filterIsolatedIslands;
  uint8_t pad2D[3];
  float seedPosition[3];
  // GenParams/Agent
  float standHeight;
  float crouchHeight;
  float proneHeight;
  float diameter;
  float maxStepHeight;
  float maxSlope;  // degrees
};
static_assert(sizeof(WorldParamsBlock) == 0x54);
#pragma pack(pop)

struct WorldParams {
  WorldParamsBlock block{};
  std::vector<CellClass> cellClasses;
  std::vector<LinkClass> links;
};

// Area ids used by the data server for input geometry groups (superset of navmesh areas).
enum InputArea : uint32_t {
  kAreaTerrain = 1,
  kAreaWater = 2,
  kAreaWaterDeep = 3,
  kAreaWaterSea = 4,
  kAreaWaterDeepSea = 5,
  kAreaBuilding = 8,
  kAreaObstacle = 10,  // tree trunks etc.; per part
  kAreaDoorOpened = 12,
  kAreaDoorClosed = 13,
  kAreaLadder = 15,    // vertex pairs, not triangles
};

struct InputPart {
  uint32_t partIndex = 0;  // 2 * k + state in the .nm
  uint64_t objectId = 0;
  uint32_t firstVertex = 0;
};

struct InputGroup {
  uint32_t area = 0;
  std::vector<InputPart> parts;  // partition the vertex array
  std::vector<float> verts;      // xyz triangle soup (or segment pairs for ladders)

  size_t vertexCount() const { return verts.size() / 3; }
  // Vertex range [first, end) of part i.
  std::pair<uint32_t, uint32_t> partRange(size_t i) const;
};

struct TileInput {
  int32_t x = 0, z = 0;
  uint32_t cellClass = 0;
  float bmin[3]{}, bmax[3]{};
  std::vector<InputGroup> groups;
};

struct WireError {
  std::string message;
};

// Wire (de)serialization of message bodies, without the length/id frame.
WorldParams parseWorldParams(std::span<const uint8_t> body);  // id 4 payload after the version
std::vector<uint8_t> serializeWorldParams(const WorldParams& p);
TileInput parseTileInput(std::span<const uint8_t> body, size_t* consumed = nullptr);  // id 5 payload
std::vector<uint8_t> serializeTileInput(const TileInput& t);

}  // namespace dznav
