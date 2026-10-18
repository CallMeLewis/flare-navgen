#include "dznav/Compare.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace dznav {

double TileSummary::walkableArea() const {
  double s = 0;
  for (const auto& [area, a] : areaByType) s += a;
  return s;
}

TileSummary summarizeTile(const Tile& t) {
  TileSummary s;
  s.x = t.header.x;
  s.z = t.header.y;
  s.polys = t.header.offMeshBase;
  s.verts = t.header.vertCount;
  s.offMesh = t.header.offMeshConCount;
  s.obstacles = int(t.obstacles.size());
  s.parts = int(t.parts.size());
  s.cellSize = t.header.bvQuantFactor > 0 ? 1.0f / t.header.bvQuantFactor : 0;
  for (const Poly& p : t.polys) {
    if (p.type() != 0) continue;
    double a = 0;
    for (int k = 0; k < p.vertCount; ++k) {
      const float* u = &t.verts[p.verts[k] * 3];
      const float* v = &t.verts[p.verts[(k + 1) % p.vertCount] * 3];
      a += double(u[0]) * v[2] - double(v[0]) * u[2];
    }
    s.areaByType[p.area()] += std::fabs(a) * 0.5;
  }
  return s;
}

CompareResult compareNavMeshes(const NavMeshFile& a, const NavMeshFile& b) {
  CompareResult r;
  std::map<std::pair<int, int>, const Tile*> byPos;
  for (const Tile& t : b.tiles) byPos[{t.header.x, t.header.y}] = &t;
  std::set<std::pair<int, int>> seen;
  for (const Tile& ta : a.tiles) {
    const std::pair<int, int> key{ta.header.x, ta.header.y};
    auto it = byPos.find(key);
    if (it == byPos.end()) {
      ++r.onlyInA;
      continue;
    }
    seen.insert(key);
    TileDiff d;
    d.a = summarizeTile(ta);
    d.b = summarizeTile(*it->second);
    std::set<int> areas;
    for (const auto& [id, v] : d.a.areaByType) areas.insert(id), r.totalAreaA[id] += v;
    for (const auto& [id, v] : d.b.areaByType) areas.insert(id), r.totalAreaB[id] += v;
    for (int id : areas) {
      const double va = d.a.areaByType.count(id) ? d.a.areaByType.at(id) : 0;
      const double vb = d.b.areaByType.count(id) ? d.b.areaByType.at(id) : 0;
      d.areaDelta += std::fabs(va - vb);
    }
    r.tiles.push_back(std::move(d));
  }
  r.onlyInB = int(b.tiles.size() - seen.size());
  return r;
}

namespace {

// Sorts tree obstacles and door parts (keeping each part's polygon list), so tiles whose objects
// were listed in a different order compare equal.
void sortObjects(Tile& t) {
  std::sort(t.obstacles.begin(), t.obstacles.end(), [](const Obstacle& a, const Obstacle& b) {
    return std::tie(a.pos[0], a.pos[2], a.radius) < std::tie(b.pos[0], b.pos[2], b.radius);
  });
  struct Part {
    uint64_t objectId;
    uint32_t partIndex;
    std::vector<uint32_t> polys;
  };
  std::vector<Part> parts;
  for (const TilePart& p : t.parts) {
    Part q{p.objectId, p.partIndex, {}};
    for (int32_t k = p.firstPoly; k >= 0 && size_t(k) < t.partPolys.size(); k = t.partPolys[k].next)
      q.polys.push_back(t.partPolys[k].poly);
    parts.push_back(std::move(q));
  }
  std::stable_sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) {
    return std::tie(a.objectId, a.partIndex) < std::tie(b.objectId, b.partIndex);
  });
  t.parts.clear();
  t.partPolys.clear();
  for (const Part& p : parts) {
    t.parts.push_back({p.objectId, p.partIndex, p.polys.empty() ? -1 : int32_t(t.partPolys.size())});
    for (size_t i = 0; i < p.polys.size(); ++i)
      t.partPolys.push_back({p.polys[i], i + 1 < p.polys.size() ? int32_t(t.partPolys.size() + 1) : -1});
  }
}

}  // namespace

Tile canonicalTile(const Tile& tile, bool ignoreIsolated, bool ignoreOrder) {
  Tile t = tile;
  t.ref = 0;
  t.recordPad = 0;
  for (Link& l : t.links) l = Link{};
  for (Poly& p : t.polys) {
    p.firstLink = 0;
    if (p.area() == 12 || p.area() == 13) p.flags &= ~2;
    if (ignoreIsolated) p.flags &= ~kIsolatedFlag;
  }
  for (OffMeshConnection& c : t.offMeshCons) {
    const Poly& p = t.polys[c.poly];
    std::copy(c.pos, c.pos + 3, &t.verts[size_t(p.verts[0]) * 3]);
    std::copy(c.pos + 3, c.pos + 6, &t.verts[size_t(p.verts[1]) * 3]);
    c.reserved = 0;
  }
  for (Obstacle& o : t.obstacles) o.pos[1] = 0;
  if (ignoreOrder) sortObjects(t);
  return t;
}

VerifyResult verifyTiles(const NavMeshFile& reference, const NavMeshFile& candidate, bool ignoreOrder) {
  VerifyResult r;
  r.ignoredOrder = ignoreOrder;
  std::map<std::pair<int, int>, const Tile*> ref;
  for (const Tile& t : reference.tiles) ref[{t.header.x, t.header.y}] = &t;
  // The island filter runs only after the last tile, so a partial save has no isolated polygons;
  // compare those without the flag.
  r.ignoredIsolated = std::none_of(reference.tiles.begin(), reference.tiles.end(), [](const Tile& t) {
    return std::any_of(t.polys.begin(), t.polys.end(), [](const Poly& p) { return p.flags & kIsolatedFlag; });
  });
  for (const Tile& t : candidate.tiles) {
    auto it = ref.find({t.header.x, t.header.y});
    if (it == ref.end()) continue;
    ++r.compared;
    if (serializeTile(canonicalTile(*it->second, r.ignoredIsolated, ignoreOrder)) ==
        serializeTile(canonicalTile(t, r.ignoredIsolated, ignoreOrder)))
      ++r.identical;
    else
      r.different.push_back({t.header.x, t.header.y});
  }
  return r;
}

}  // namespace dznav
