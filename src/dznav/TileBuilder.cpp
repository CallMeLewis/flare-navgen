#include "dznav/TileBuilder.h"

#include <xmmintrin.h>

#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include "DetourAlloc.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "Recast.h"
#include "dznav/JumpLinks.h"
#include "dznav/NavMeshFormat.h"

namespace dznav {
namespace {

// Collects Recast errors so a failed build can say why.
class Context : public rcContext {
 public:
  Context() : rcContext(true) { enableTimer(false); }
  std::string errors;

 protected:
  void doLog(const rcLogCategory category, const char* msg, const int len) override {
    if (category == RC_LOG_ERROR) errors.append(msg, size_t(len)).append("; ");
  }
};

struct RcDeleter {
  void operator()(rcHeightfield* p) const { rcFreeHeightField(p); }
  void operator()(rcCompactHeightfield* p) const { rcFreeCompactHeightfield(p); }
  void operator()(rcContourSet* p) const { rcFreeContourSet(p); }
  void operator()(rcPolyMesh* p) const { rcFreePolyMesh(p); }
};
template <typename T>
using RcPtr = std::unique_ptr<T, RcDeleter>;

// Per-tile configuration. All arithmetic is in single precision, which the output depends on.
struct Config {
  int width = 0, tileSize = 0, borderSize = 0;
  float cs = 0, ch = 0.1f;
  float bmin[3]{}, bmax[3]{};
  float walkableSlopeAngle = 0;
  int walkableHeight = 0, crouchHeight = 0, proneHeight = 0, walkableClimb = 0, walkableRadius = 0;
  int deepWaterDepth = 0, seaLevel = 0;
  int maxEdgeLen = 120;
  float maxSimplificationError = 0, maxVerticalError = 8.0f;
  int minRegionArea = 0, mergeRegionArea = 400;
  int minRegionAreaByArea[64]{};
  float agentHeight = 0, agentRadius = 0, agentClimb = 0;  // metres, for the Detour header
  bool obstacles = true;
};

Config makeConfig(const WorldParams& wp, const TileInput& in, const TileBuildOptions& opt) {
  const WorldParamsBlock& b = wp.block;
  const CellClass& cc = wp.cellClasses.at(in.cellClass);
  Config c;
  c.tileSize = cc.cells;
  c.cs = cc.tileWidth / (float)cc.cells;
  c.ch = 0.1f;
  c.walkableSlopeAngle = b.maxSlope;
  c.walkableHeight = (int)std::ceil(b.standHeight / 0.1f);
  c.crouchHeight = (int)std::ceil(b.crouchHeight / 0.1f) - 2;
  c.proneHeight = (int)std::ceil(b.proneHeight / 0.1f) - 2;
  c.walkableClimb = (int)std::ceil(b.maxStepHeight / 0.1f);
  c.walkableRadius = (int)std::ceil((b.diameter * 0.5f) / c.cs);
  c.deepWaterDepth = (int)std::ceil(b.deepWaterDepth / 0.1f);
  const float sea = std::isnan(opt.seaLevel) ? b.deepSeaHeight : opt.seaLevel;
  c.seaLevel = (int)std::ceil(sea / 0.1f);
  c.borderSize = std::max(10, c.walkableRadius - (int)(-3.0f / c.cs));
  c.width = cc.cells + 2 * c.borderSize;
  const float pad = (float)c.borderSize * c.cs;
  c.bmin[0] = in.bmin[0] - pad;
  c.bmin[1] = in.bmin[1];
  c.bmin[2] = in.bmin[2] - pad;
  c.bmax[0] = in.bmax[0] + pad;
  c.bmax[1] = in.bmax[1];
  c.bmax[2] = in.bmax[2] + pad;
  c.maxSimplificationError = b.contourMaxError / c.cs;
  c.minRegionArea = (int)((1.0f / c.cs) * (1.0f / c.cs));
  for (int& a : c.minRegionAreaByArea) a = c.minRegionArea;
  c.minRegionAreaByArea[10] = c.minRegionAreaByArea[12] = c.minRegionAreaByArea[13] = c.minRegionAreaByArea[14] =
      INT_MAX;
  c.agentHeight = b.standHeight;
  c.agentRadius = b.diameter * 0.5f;
  c.agentClimb = b.maxStepHeight;
  c.obstacles = b.obstacles != 0;
  return c;
}

// Walkability of a DayZ triangle (v0, v1, v2) as rasterised (v0, v2, v1): Recast's normal with
// Enfusion's approximate normalisation (RSQRTSS plus one Newton step).
bool walkable(const float* v0, const float* v1, const float* v2, float threshold) {
  const float e1[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]};
  const float e2[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
  float n[3] = {e2[1] * e1[2] - e2[2] * e1[1], e2[2] * e1[0] - e2[0] * e1[2], e2[0] * e1[1] - e2[1] * e1[0]};
  const float d = (n[0] * n[0] + n[1] * n[1]) + n[2] * n[2];
  if (d > FLT_MIN) {
    const float r = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(d)));
    const float inv = (0.5f * r) * (3.0f - (d * r) * r);
    n[1] *= inv;
  }
  return threshold < n[1];
}

struct Part {
  uint64_t objectId = 0;
  uint32_t partIndex = 0;
  uint8_t state = 1;
  uint8_t area = 0;
  std::vector<int> polys;
};

// Door/hide parts of every group except trees; trees only reserve obstacle slots.
struct TileExtra {
  std::vector<Part> parts;
  int treeItems = 0;
  std::vector<float> obstacles;  // 4 floats each
};

TileExtra buildExtra(const TileInput& in, DoorStatePolicy policy) {
  TileExtra e;
  for (const InputGroup& g : in.groups) {
    const uint8_t area = uint8_t(g.area);
    if (area == kAreaObstacle) {
      e.treeItems = int(g.parts.size());
      continue;
    }
    for (const InputPart& p : g.parts) {
      Part part;
      part.objectId = p.objectId;
      part.partIndex = p.partIndex;
      part.area = area;
      // The server doesn't send the door state, so the policy decides it.
      switch (policy) {
        case DoorStatePolicy::Closed: part.state = area == kAreaDoorOpened ? 0 : 1; break;
        case DoorStatePolicy::Open: part.state = area == kAreaDoorClosed ? 0 : 1; break;
        case DoorStatePolicy::Enabled: part.state = 1; break;
      }
      e.parts.push_back(std::move(part));
    }
  }
  return e;
}

void rasterizeTris(Context& ctx, const float* verts, size_t nverts, uint8_t area, uint16_t subArea, rcHeightfield& hf,
                   const dzRasterParams& rp, int mode, bool slopeTest, float slopeThreshold) {
  for (size_t t = 0; t + 2 < nverts; t += 3) {
    const float* v0 = &verts[t * 3];
    const float* v1 = &verts[(t + 1) * 3];
    const float* v2 = &verts[(t + 2) * 3];
    const uint8_t a = (!slopeTest || walkable(v0, v1, v2, slopeThreshold)) ? area : RC_NULL_AREA;
    // DayZ geometry is wound opposite to Recast.
    if (!dzRasterizeTriangle(&ctx, v0, v2, v1, a, subArea, hf, rp, mode, 1))
      throw std::runtime_error("rasterisation failed: " + ctx.errors);
  }
}

void rasterizeGroup(Context& ctx, const InputGroup& g, rcHeightfield& hf, const Config& c, float slopeThreshold,
                    int& partCounter) {
  const uint8_t type = uint8_t(g.area);
  dzRasterParams rp{c.walkableClimb, c.walkableHeight, c.walkableClimb};
  const float* v = g.verts.data();
  const size_t nv = g.vertexCount();
  auto forParts = [&](auto&& fn) {
    for (size_t k = 0; k < g.parts.size(); ++k) {
      const auto [first, end] = g.partRange(k);
      fn(k, &v[size_t(first) * 3], size_t(end - first));
    }
  };
  switch (type) {
    case 2:  // water: shallow then deep
      rasterizeTris(ctx, v, nv, 2, 0, hf, rp, DZ_RASTER_WATER, false, slopeThreshold);
      rp.waterDepth = c.deepWaterDepth;
      rasterizeTris(ctx, v, nv, 3, 0, hf, rp, DZ_RASTER_WATER, false, slopeThreshold);
      break;
    case 6:
    case 7:
    case 8:
    case 9:
    case 11:
      rasterizeTris(ctx, v, nv, type, 0, hf, rp, DZ_RASTER_ADD, true, slopeThreshold);
      break;
    case 10:  // trees
      if (g.parts.empty()) {
        rasterizeTris(ctx, v, nv, 10, 0, hf, rp, DZ_RASTER_TREE, false, slopeThreshold);
      } else {
        forParts([&](size_t k, const float* pv, size_t pn) {
          if (c.obstacles)
            rasterizeTris(ctx, pv, pn, 10, uint16_t(k), hf, rp, DZ_RASTER_TREE, false, slopeThreshold);
          else
            rasterizeTris(ctx, pv, pn, RC_NULL_AREA, 0, hf, rp, DZ_RASTER_ADD, false, slopeThreshold);
        });
      }
      break;
    case 12:
    case 13:  // doors
      if (g.parts.empty()) {
        rasterizeTris(ctx, v, nv, type, 0, hf, rp, DZ_RASTER_PAINT_BELOW, false, slopeThreshold);
      } else {
        forParts([&](size_t, const float* pv, size_t pn) {
          if (!dzRasterizePart(&ctx, pv, int(pn), type, uint16_t(partCounter++), hf, DZ_RASTER_PAINT_BELOW,
                               c.walkableHeight))
            throw std::runtime_error("door rasterisation failed: " + ctx.errors);
        });
      }
      break;
    case 14:  // hide parts
      if (g.parts.empty()) {
        rasterizeTris(ctx, v, nv, 14, 0, hf, rp, DZ_RASTER_PAINT_OR_ADD, false, slopeThreshold);
      } else {
        forParts([&](size_t, const float* pv, size_t pn) {
          if (!dzRasterizePart(&ctx, pv, int(pn), 14, uint16_t(partCounter++), hf, DZ_RASTER_PAINT_OR_ADD, 1))
            throw std::runtime_error("part rasterisation failed: " + ctx.errors);
        });
      }
      break;
    case 18:  // WallWithHole ceilings
      rasterizeTris(ctx, v, nv, 18, 0, hf, rp, DZ_RASTER_ADD, false, slopeThreshold);
      break;
    default:
      break;
  }
}

void postCompact(Context& ctx, rcCompactHeightfield& chf, const Config& c, TileExtra& extra) {
  const float boxMin[3] = {c.bmin[0], -500.0f, c.bmin[2]};
  const float boxMax[3] = {c.bmax[0], (float)c.seaLevel * c.ch, c.bmax[2]};
  rcMarkBoxArea(&ctx, boxMin, boxMax, 5, chf);

  unsigned char mask[64];
  std::memset(mask, 1, sizeof(mask));
  mask[12] = mask[13] = mask[14] = 0;
  const int doorRadius = (int)((float)c.walkableRadius * 0.8f);
  if (!dzDilateArea(&ctx, doorRadius, chf, 13, mask) || !dzDilateArea(&ctx, doorRadius, chf, 12, mask) ||
      !dzDilateArea(&ctx, c.walkableRadius, chf, 14, mask) || !dzDilateArea(&ctx, c.walkableRadius, chf, 16, mask))
    throw std::runtime_error("dilate failed: " + ctx.errors);

  if (c.obstacles && extra.treeItems > 0) {
    unsigned char seeds[64]{};
    seeds[0] = seeds[10] = seeds[12] = seeds[13] = seeds[14] = 1;
    extra.obstacles.resize(size_t(extra.treeItems) * 4);
    const int n = dzExtractTreeObstacles(&ctx, chf, 10, 1, extra.treeItems, seeds, 2.0f, 3.0f, extra.obstacles.data());
    extra.obstacles.resize(size_t(n) * 4);
  }

  if (!rcErodeWalkableArea(&ctx, c.walkableRadius, chf)) throw std::runtime_error("buildNavigation: Could not erode.");

  for (int i = 0; i < chf.spanCount; ++i) {
    if (chf.areas[i] == 7 || chf.areas[i] == 9)
      chf.areas[i] = 1;
    else if (chf.areas[i] == 11)
      chf.areas[i] = 8;
  }
  // Building floors open to the sky are terrain (roofs).
  for (int i = 0; i < chf.width * chf.height; ++i) {
    const rcCompactCell& cell = chf.cells[i];
    if (!cell.count) continue;
    const unsigned int k = cell.index + cell.count - 1;
    if (chf.areas[k] == 8 && chf.spans[k].h == 255) chf.areas[k] = 1;
  }
}

uint16_t flagsForArea(uint8_t area, uint16_t current) {
  switch (area) {
    case 1: case 6: case 7: case 9: case 14: return 0x01;
    case 2: case 4: return 0x11;
    case 3: return 0x10;
    case 5: return 0x20;
    case 8: return 0x09;
    case 12: case 13: return 0x05;
    case 16: return 0x400;
    case 17: return 0x800;
    default: return current;
  }
}

// Tree ids left on area-5 spans by the sea pass can exceed the part count; those polys get no
// part rather than reading past the table.
std::vector<uint8_t> buildExtraBlob(TileExtra& extra, const rcPolyMesh& pm, int& strayPartIds) {
  uint32_t nPartPolys = 0;
  if (!extra.parts.empty()) {
    for (int i = 0; i < pm.npolys; ++i) {
      const unsigned p = pm.subAreas[i];
      if (p >= extra.parts.size()) {
        ++strayPartIds;
        continue;
      }
      if (extra.parts[p].area != pm.areas[i]) continue;
      Part& part = extra.parts[p];
      if (part.state == 0)
        pm.flags[i] |= 2;
      else
        pm.flags[i] &= ~2;
      part.polys.push_back(i);
      ++nPartPolys;
    }
  }
  std::vector<uint8_t> out;
  auto put = [&](const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
  };
  const auto nParts = uint32_t(extra.parts.size());
  put(&nParts, 4);
  put(&nPartPolys, 4);
  int32_t next = 0;
  for (const Part& p : extra.parts) {
    TilePart tp{p.objectId, p.partIndex, p.polys.empty() ? -1 : next};
    put(&tp, sizeof(tp));
    next += int32_t(p.polys.size());
  }
  int32_t idx = 0;
  for (const Part& p : extra.parts)
    for (size_t k = 0; k < p.polys.size(); ++k) {
      ++idx;
      TilePartPoly pp{uint32_t(p.polys[k]), k + 1 < p.polys.size() ? idx : -1};
      put(&pp, sizeof(pp));
    }
  const auto nObs = uint32_t(extra.obstacles.size() / 4);
  put(&nObs, 4);
  if (nObs) put(extra.obstacles.data(), extra.obstacles.size() * sizeof(float));
  return out;
}

}  // namespace

std::vector<uint8_t> buildTile(const WorldParams& wp, const TileInput& in, TileBuildStats& stats,
                               const TileBuildOptions& opt) {
  Context ctx;
  const Config c = makeConfig(wp, in, opt);
  TileExtra extra = buildExtra(in, opt.doors);

  RcPtr<rcHeightfield> solid(rcAllocHeightfield());
  if (!rcCreateHeightfield(&ctx, *solid, c.width, c.width, c.bmin, c.bmax, c.cs, c.ch))
    throw std::runtime_error("buildNavigation: Could not create solid heightfield.");

  const float slopeThreshold = std::cos((c.walkableSlopeAngle / 180.0f) * 3.1415927f);
  const dzRasterParams rp{c.walkableClimb, c.walkableHeight, c.walkableClimb};

  // Terrain (group 0) with the slope test, then the 21x21 mode filter.
  if (!in.groups.empty()) {
    const InputGroup& terrain = in.groups[0];
    rasterizeTris(ctx, terrain.verts.data(), terrain.vertexCount(), uint8_t(terrain.area), 0, *solid, rp,
                  DZ_RASTER_ADD, true, slopeThreshold);
  }
  if (c.borderSize > 10 && !dzModeFilterWalkableAreaFlat(&ctx, *solid, c.borderSize))
    throw std::runtime_error(ctx.errors);

  // Objects, in group order. Ladders (15) become off-mesh links.
  int partCounter = 0;
  std::vector<const InputGroup*> ladders;
  for (const InputGroup& g : in.groups) {
    if (uint8_t(g.area) == kAreaTerrain) continue;
    if (uint8_t(g.area) == kAreaLadder) {
      ladders.push_back(&g);
      continue;
    }
    rasterizeGroup(ctx, g, *solid, c, slopeThreshold, partCounter);
  }

  const int heights[3] = {c.proneHeight, c.crouchHeight, c.walkableHeight};
  const unsigned char lowAreas[3] = {RC_NULL_AREA, 16, 17};
  unsigned char flaggedCeilings[64]{};
  flaggedCeilings[18] = 1;
  dzFilterLowHeightSpansMulti(&ctx, heights, lowAreas, 3, flaggedCeilings, *solid);
  rcFilterLowHangingWalkableObstacles(&ctx, c.walkableClimb, *solid);
  unsigned char ledgeSkip[64]{};
  ledgeSkip[0] = ledgeSkip[1] = 1;
  rcFilterLedgeSpans(&ctx, c.walkableHeight, c.walkableClimb, *solid, ledgeSkip);

  RcPtr<rcCompactHeightfield> chf(rcAllocCompactHeightfield());
  if (!rcBuildCompactHeightfield(&ctx, c.proneHeight, c.walkableClimb, *solid, *chf))
    throw std::runtime_error("buildNavigation: Could not build compact data.");
  postCompact(ctx, *chf, c, extra);

  OffMeshLinks links;
  if (opt.jumpLinks) {
    JumpLinkInput jl{&wp, &c.minRegionAreaByArea[0], c.mergeRegionArea, c.maxSimplificationError,
                     c.maxVerticalError, c.maxEdgeLen, solid.get(), chf.get()};
    for (const InputGroup* g : ladders) jl.ladders.push_back(g);
    generateJumpLinks(&ctx, jl, links);
  }
  solid.reset();

  if (!rcBuildDistanceField(&ctx, *chf)) throw std::runtime_error("buildNavigation: Could not build distance field.");
  if (!rcBuildRegions(&ctx, *chf, c.borderSize, c.minRegionAreaByArea, c.mergeRegionArea))
    throw std::runtime_error("buildNavigation: Could not build regions.");

  RcPtr<rcContourSet> cset(rcAllocContourSet());
  if (!rcBuildContours(&ctx, *chf, c.maxSimplificationError, c.maxVerticalError, c.maxEdgeLen, *cset,
                       RC_CONTOUR_TESS_WALL_EDGES))
    throw std::runtime_error("buildNavigation: Could not create contours.");
  if (cset->nconts == 0) return {};

  RcPtr<rcPolyMesh> pmesh(rcAllocPolyMesh());
  if (!rcBuildPolyMesh(&ctx, *cset, 6, *pmesh))
    throw std::runtime_error("buildNavigation: Could not triangulate contours.");
  chf.reset();
  cset.reset();
  if (pmesh->npolys == 0) return {};
  if (pmesh->nverts >= 0xffff)
    throw std::runtime_error("Too many vertices per tile " + std::to_string(pmesh->nverts) + " (max: 65535).");

  // Finalizer: flags from areas, door parts and obstacles, then Detour.
  for (int i = 0; i < pmesh->npolys; ++i) pmesh->flags[i] = flagsForArea(pmesh->areas[i], pmesh->flags[i]);
  const std::vector<uint8_t> extraBlob = buildExtraBlob(extra, *pmesh, stats.strayPartIds);

  dtNavMeshCreateParams p{};
  p.verts = pmesh->verts;
  p.vertCount = pmesh->nverts;
  p.polys = pmesh->polys;
  p.polyAreas = pmesh->areas;
  p.polyFlags = pmesh->flags;
  p.polyCount = pmesh->npolys;
  p.nvp = pmesh->nvp;
  p.offMeshConCount = int(links.rad.size());
  if (p.offMeshConCount) {
    p.offMeshConVerts = links.verts.data();
    p.offMeshConRad = links.rad.data();
    p.offMeshConCenterHeight = links.centerHeight.data();
    p.offMeshConFlags = links.flags.data();
    p.offMeshConAreas = links.areas.data();
    p.offMeshConDir = links.dirs.data();
    p.offMeshConUserID = links.userIds.data();
  }
  p.extraData = extraBlob.data();
  p.extraDataSize = int(extraBlob.size());
  p.tileX = in.x;
  p.tileY = in.z;
  p.tileLayer = 0;
  std::memcpy(p.bmin, pmesh->bmin, sizeof(p.bmin));
  std::memcpy(p.bmax, pmesh->bmax, sizeof(p.bmax));
  p.walkableHeight = c.agentHeight;
  p.walkableRadius = c.agentRadius;
  p.walkableClimb = c.agentClimb;
  p.cs = c.cs;
  p.ch = c.ch;
  p.buildBvTree = true;

  unsigned char* data = nullptr;
  int size = 0;
  if (!dtCreateNavMeshData(&p, &data, &size)) throw std::runtime_error("Could not build Detour navmesh.");
  // Counts as stored in the tile (off-mesh links outside the tile are dropped; each kept one adds a poly).
  const dtMeshHeader* header = reinterpret_cast<const dtMeshHeader*>(data);
  stats.dataSize = size;
  stats.polys = header->polyCount;
  stats.verts = header->vertCount;
  stats.offMesh = header->offMeshConCount;
  stats.obstacles = int(extra.obstacles.size() / 4);
  std::vector<uint8_t> out(data, data + size);
  dtFree(data);
  return out;
}

}  // namespace dznav
