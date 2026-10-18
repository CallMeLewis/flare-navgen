// Jump/climb off-mesh link generation, as in DayZ's navmeshes.
#pragma once

#include <cstdint>
#include <vector>

#include "dznav/WorldInput.h"

class rcContext;
struct rcHeightfield;
struct rcCompactHeightfield;

namespace dznav {

// Off-mesh connections in dtNavMeshCreateParams layout.
struct OffMeshLinks {
  std::vector<float> verts;         // 6 per link: start, end
  std::vector<float> centerHeight;  // stored at dtOffMeshConnection+24
  std::vector<float> rad;           // endpoint snap radius
  std::vector<uint16_t> flags;
  std::vector<uint8_t> areas;
  std::vector<uint8_t> dirs;  // 1 = bidirectional
  std::vector<uint32_t> userIds;

  void add(const float* start, const float* end, float centre, float radius, uint16_t flag, uint8_t area,
           uint8_t dir, uint32_t userId);
};

struct JumpLinkInput {
  const WorldParams* params = nullptr;
  const int* minRegionAreaByArea = nullptr;  // [64]
  int mergeRegionArea = 400;
  float maxSimplificationError = 0;
  float maxVerticalError = 8.0f;
  int maxEdgeLen = 120;
  const rcHeightfield* solid = nullptr;
  rcCompactHeightfield* chf = nullptr;  // regions are overwritten (rebuilt by the caller)
  std::vector<const InputGroup*> ladders;
};

void generateJumpLinks(rcContext* ctx, const JumpLinkInput& in, OffMeshLinks& out);

}  // namespace dznav
