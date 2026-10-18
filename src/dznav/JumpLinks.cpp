// Jump/climb link generation, as in DayZ's navmeshes: centrelines, facing-edge refinement,
// trajectory, sampling, reverse merge and emission.
#include "dznav/JumpLinks.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <memory>

#include "Recast.h"

namespace dznav {

void OffMeshLinks::add(const float* start, const float* end, float centre, float radius, uint16_t flag, uint8_t area,
                       uint8_t dir, uint32_t userId) {
  verts.insert(verts.end(), start, start + 3);
  verts.insert(verts.end(), end, end + 3);
  centerHeight.push_back(centre);
  rad.push_back(radius);
  flags.push_back(flag);
  areas.push_back(area);
  dirs.push_back(dir);
  userIds.push_back(userId);
}

namespace {

struct V3 {
  float x = 0, y = 0, z = 0;
};
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float lenSq(V3 a) { return a.x * a.x + a.y * a.y + a.z * a.z; }

struct Seg {
  V3 p0, p1;
};

// Link class record (0x28 bytes).
struct ClassRec {
  float jumpHeight, jumpLength, dropMin, dropMax, agentHeight, maxStep, agentRadius, minCenterHeight;
  uint32_t index;
  uint16_t flags;
  uint8_t area;
};

struct Span {
  V3 s0, s1, e0, e1;
  float centerH = 0;
  int valid = 1;  // 0 dropped, 1 one-way, 2 bidirectional
};

struct TrajSample {
  float x, y, ymin, ymax;
};

struct Gen {
  const rcHeightfield* hf;
  const rcCompactHeightfield* chf;
  const rcContourSet* cset;
  int waterMin = 2, waterMax = 6;
  std::vector<Seg> edges;
  std::vector<Span> spans;
};

// Finds the edges facing p0->p1 across an obstacle (within `depth`) and returns the
// midlines between them.
int refineEdge(const Seg& s, float depth, float width, const std::vector<Seg>& segs, float cs, Seg* out, int maxOut) {
  const V3 p0 = s.p0;
  const V3 d = s.p1 - s.p0;
  const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  V3 u;
  if (len >= FLT_EPSILON)
    u = d * (1.0f / len);
  else
    u = {d.x, 1.0f, d.z};
  V3 n = {u.z, 0.0f, -u.x};
  {
    const float nl = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (nl >= FLT_EPSILON)
      n = n * (1.0f / nl);
    else
      n = {u.z, 1.0f, -u.x};
  }
  const float cos135 = std::cos(2.3561945f);
  auto local = [&](V3 q) {
    const V3 dq = q - p0;
    return V3{u.x * dq.x + n.x * dq.z, u.y * dq.x + dq.y + n.y * dq.z, u.z * dq.x + n.z * dq.z};
  };
  const float bmin[3] = {0.0f, -width * 0.5f, 0.0f};
  const float bmax[3] = {len, width * 0.5f, depth};

  struct Rec {
    int group;
    int seg;
    float t0, t1, zmin, zmax;
  };
  Rec recs[64];
  int nrecs = 0;
  for (int k = 0; k < (int)segs.size(); ++k) {
    const V3 a = local(segs[k].p0);
    const V3 b = local(segs[k].p1);
    const V3 e = b - a;
    float tmin = 0.0f, tmax = FLT_MAX;
    const float av[3] = {a.x, a.y, a.z};
    const float ev[3] = {e.x, e.y, e.z};
    bool reject = false;
    for (int ax = 0; ax < 3 && !reject; ++ax) {
      if (std::fabs(ev[ax]) < 1e-6f) {
        if (av[ax] < bmin[ax] || av[ax] > bmax[ax]) reject = true;
        continue;
      }
      const float inv = 1.0f / ev[ax];
      float t1 = (bmin[ax] - av[ax]) * inv;
      float t2 = (bmax[ax] - av[ax]) * inv;
      if (t1 > t2) std::swap(t1, t2);
      tmin = std::max(tmin, t1);
      tmax = std::min(tmax, t2);
      if (tmax < tmin) reject = true;
    }
    if (reject || !(tmin <= 1.0f && tmax >= 0.0f)) continue;
    V3 v = {segs[k].p1.x - segs[k].p0.x, 0.0f, segs[k].p1.z - segs[k].p0.z};
    const float vl = std::sqrt(v.x * v.x + v.z * v.z);
    if (vl >= FLT_EPSILON)
      v = v * (1.0f / vl);
    else
      v.y = 1.0f;
    if (v.x * u.x + v.y * u.y + v.z * u.z <= cos135 && nrecs < 64)
      recs[nrecs++] = {0, k, std::clamp(tmin, 0.0f, 1.0f), std::clamp(tmax, 0.0f, 1.0f), std::min(a.z, b.z),
                       std::max(a.z, b.z)};
  }
  int g = 1;
  for (int i = 0; i < nrecs; ++i) {
    if (recs[i].group != 0) continue;
    recs[i].group = g;
    for (int j = i + 1; j < nrecs; ++j)
      if (recs[i].zmin - cs <= cs + recs[j].zmax && recs[j].zmin - cs <= cs + recs[i].zmax) recs[j].group = g;
    ++g;
  }
  const float dd = d.x * d.x + d.y * d.y + d.z * d.z;
  int nout = 0;
  for (int grp = 1; grp < g; ++grp) {
    float smin = 10.0f, smax = -10.0f;
    V3 minPt, maxPt;
    for (int i = 0; i < nrecs; ++i) {
      if (recs[i].group != grp) continue;
      const Seg& q = segs[recs[i].seg];
      const V3 qd = q.p1 - q.p0;
      const V3 cs2[2] = {q.p0 + qd * recs[i].t0, q.p0 + qd * recs[i].t1};
      for (const V3& c : cs2) {
        float t = (c.y - p0.y) * d.y + (c.x - p0.x) * d.x + (c.z - p0.z) * d.z;
        t = t <= 0.0f ? 0.0f : (t < dd ? t / dd : 1.0f);
        if (t < smin) {
          smin = t;
          minPt = c;
        }
        if (smax < t) {
          smax = t;
          maxPt = c;
        }
      }
    }
    if (smin <= smax && nout < maxOut) {
      const V3 e0 = p0 + d * smin;
      const V3 e1 = p0 + d * smax;
      out[nout++] = {e0 + (minPt - e0) * 0.5f, e1 + (maxPt - e1) * 0.5f};
    }
  }
  return nout;
}

void buildCentrelines(Gen& gen, float depth, float minLenSq) {
  const rcContourSet& cset = *gen.cset;
  std::vector<Seg> segs;
  for (int ci = 0; ci < cset.nconts; ++ci) {
    const rcContour& c = cset.conts[ci];
    for (int i = 0, j = c.nverts - 1; i < c.nverts; j = i++) {
      const int* vi = &c.verts[i * 4];
      const int* vj = &c.verts[j * 4];
      if ((vj[3] & RC_AREA_BORDER) == 0) continue;
      auto P = [&](const int* v) {
        return V3{(float)v[0] * cset.cs + cset.bmin[0], (float)(v[1] + 2) * cset.ch + cset.bmin[1],
                  (float)v[2] * cset.cs + cset.bmin[2]};
      };
      const Seg s{P(vi), P(vj)};
      const V3 d = s.p1 - s.p0;
      if (d.y * d.y + d.x * d.x + d.z * d.z >= minLenSq) segs.push_back(s);
    }
  }
  Seg outs[32];
  for (const Seg& s : segs) {
    const int n = refineEdge(s, depth, 1.0f, segs, gen.chf->cs, outs, 32);
    float best = 0.0f;
    int bi = -1;
    for (int i = 0; i < n; ++i) {
      const float l = lenSq(outs[i].p1 - outs[i].p0);
      if (l > best) {
        best = l;
        bi = i;
      }
    }
    if (bi >= 0) gen.edges.push_back(outs[bi]);
  }
}

std::vector<TrajSample> buildTrajectory(const ClassRec& c, float cs, float& total) {
  const float r = c.agentRadius;
  total = (r + r) + c.jumpLength;
  const int nT = (int)std::ceil(total / cs);
  std::vector<TrajSample> t(size_t(std::max(nT, 0)));
  for (int i = 0; i < nT; ++i) {
    const float x = (float)i * cs;
    t[i].x = x;
    t[i].y = (x < r) ? c.maxStep : (x >= total - r ? c.dropMin : c.jumpHeight);
  }
  for (int i = 0; i < nT; ++i) {
    const float lo = std::max(t[i].x - r, t[0].x);
    const float hi = std::min(t[i].x + r, t[nT - 1].x);
    float ymin = t[i].y, ymax = t[i].y;
    for (int j = 0; j < nT; ++j)
      if (lo <= t[j].x && t[j].x < hi) {
        ymin = std::min(ymin, t[j].y);
        ymax = std::max(ymax, t[j].y);
      }
    t[i].ymin = ymin;
    t[i].ymax = ymax + c.agentHeight;
  }
  return t;
}

// Height of the walkable span in the cell under p closest to p.y.
bool chfNearestFloor(const rcCompactHeightfield& chf, V3 p, float& y) {
  const int x = (int)std::floor((p.x - chf.bmin[0]) / chf.cs);
  const int z = (int)std::floor((p.z - chf.bmin[2]) / chf.cs);
  if (x < 0 || z < 0 || x >= chf.width || z >= chf.height) return false;
  const rcCompactCell& c = chf.cells[x + z * chf.width];
  float best = FLT_MAX;
  bool found = false;
  for (unsigned i = c.index, ni = c.index + c.count; i < ni; ++i) {
    if (chf.areas[i] == RC_NULL_AREA) continue;
    const float sy = (float)chf.spans[i].y * chf.ch + chf.bmin[1];
    const float dist = std::fabs(p.y - sy);
    if (dist < best) {
      best = dist;
      y = sy;
      found = true;
    }
  }
  return found;
}

// Top of the solid span in p's column closest to p.y (any area).
bool solidNearestTop(const rcHeightfield& hf, V3 p, float& y) {
  const int x = (int)((p.x - hf.bmin[0]) / hf.cs);
  const int z = (int)((p.z - hf.bmin[2]) / hf.cs);
  if (x < 0 || z < 0 || x >= hf.width || z >= hf.height) return false;
  float best = FLT_MAX;
  bool found = false;
  for (const rcSpan* s = hf.spans[x + z * hf.width]; s; s = s->next) {
    const float sy = (float)s->smax * hf.ch + hf.bmin[1];
    const float dist = std::fabs(p.y - sy);
    if (dist < best) {
      best = dist;
      y = sy;
      found = true;
    }
  }
  return found;
}

V3 normalizeOr(V3 v, V3 fallback) {
  const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  return l >= FLT_EPSILON ? v * (1.0f / l) : fallback;
}

void sampleEdges(Gen& gen, const std::vector<TrajSample>& traj, float total, const ClassRec& c) {
  const rcCompactHeightfield& chf = *gen.chf;
  const rcHeightfield& hf = *gen.hf;
  const int nT = (int)traj.size();
  if (nT == 0) return;
  const float h = total * 0.5f;
  const float L2 = c.jumpLength * 0.5f;
  const float mch = c.minCenterHeight;
  const float invNT = 1.0f / (float)nT;
  const int minRun = (int)std::ceil((c.agentRadius + c.agentRadius) / chf.cs);

  std::vector<int> state;
  std::vector<float> value;
  for (const Seg& edge : gen.edges) {
    const V3 A = edge.p0, B = edge.p1;
    const V3 dn = normalizeOr(B - A, V3{(B - A).x, 1.0f, (B - A).z});
    const V3 n = normalizeOr(V3{dn.z, 0.0f, -dn.x}, V3{dn.z, 1.0f, -dn.x});
    const V3 S0 = n * (-h) + A;
    const V3 dir = (n * (-h) + B) - S0;
    const V3 P0 = n * (-L2) + A;
    const V3 P1 = n * L2 + A;
    // The far end is dir * t + (n * h + A), with n * h + A computed once per edge. The
    // association matters: (dir * t + n * h) + A lands samples one float step apart, which can
    // flip a cell lookup.
    const V3 Ebase = n * h + A;
    const float dirLen = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    const int nS = (int)std::ceil(dirLen / chf.cs);
    if (nS <= 0) continue;
    const float step = nS == 1 ? 0.0f : 1.0f / (float)(nS - 1);
    state.assign(size_t(nS), 1);
    value.assign(size_t(nS), 0.0f);

    for (int k = 0; k < nS; ++k) {
      const float t = (float)k * step;
      const V3 S = dir * t + S0;
      const V3 E = dir * t + Ebase;
      const float cy = dir.y * t + A.y;
      float yS, yE;
      if (!chfNearestFloor(chf, S, yS)) continue;
      if (!(yS <= c.dropMin + c.maxStep + S.y && S.y - (c.maxStep - c.dropMax) <= yS)) continue;
      const float ys = yS + chf.ch;
      const float yTop = ys + traj[nT - 1].ymin;
      if (!chfNearestFloor(chf, E, yE)) continue;
      if (!(yE <= c.maxStep + yTop && (yTop - c.dropMin + c.dropMax) - c.maxStep <= yE)) continue;
      const float ye = yE + chf.ch;
      unsigned short hiU = 0, loU = 10000;
      unsigned int last = 0;
      int st = 0;
      for (int j = 0; j < nT && st == 0; ++j) {
        const float u = (float)j * invNT;
        const float y = ys + (ye - ys) * u;
        const float x = S.x + (E.x - S.x) * u;
        const float z = S.z + (E.z - S.z) * u;
        const int cx = (int)((x - hf.bmin[0]) / hf.cs);
        const int cz = (int)((z - hf.bmin[2]) / hf.cs);
        const bool inRange = cx >= 0 && cz >= 0 && cx < hf.width && cz < hf.height;
        if ((j == 0 || j == nT - 1) && inRange) {
          const rcCompactCell& cell = chf.cells[cx + cz * chf.width];
          if (cell.count && gen.waterMin <= chf.areas[cell.index] && chf.areas[cell.index] <= gen.waterMax) {
            st = 2;
            break;
          }
        }
        if (inRange) {
          if (const rcSpan* s = hf.spans[cx + cz * hf.width]) {
            last = 0;
            const rcSpan* prev = nullptr;
            const unsigned top = (unsigned)(int64_t)((y + traj[j].ymax - hf.bmin[1]) / hf.ch);
            const unsigned bot = (unsigned)(int64_t)((y + traj[j].ymin - hf.bmin[1]) / hf.ch);
            for (; s; prev = s, s = s->next) {
              if (top < s->smin) break;
              last = s->smax;
              if (bot <= s->smax && s->smin <= top) {
                st = 1;
                break;
              }
            }
            if (st) break;
            if (prev && gen.waterMin <= (int)prev->area && (int)prev->area <= gen.waterMax) {
              st = 2;
              break;
            }
          }
        }
        hiU = std::max<unsigned short>(hiU, (unsigned short)last);
        loU = std::min<unsigned short>(loU, (unsigned short)last);
      }
      if (st != 0) {
        state[k] = st;
        continue;
      }
      const unsigned short hsel = mch >= 0.0f ? hiU : loU;
      const float centerH = (float)(hsel + 1) * chf.ch + chf.bmin[1] - cy;
      value[k] = centerH;
      state[k] = ((mch >= 0.0f && centerH < mch) || (mch < 0.0f && centerH > mch)) ? 1 : 0;
    }

    int start = -1;
    for (int k = 0; k <= nS; ++k) {
      const bool valid = k < nS && state[k] == 0;
      if (start == -1) {
        if (valid) start = k;
      } else if (!valid) {
        if (k - start > minRun) {
          const float t0 = (float)start * step;
          const float t1 = (float)(k - 1) * step;
          Span sp;
          sp.s0 = P0 + dir * t0;
          sp.s1 = P0 + dir * t1;
          sp.e0 = P1 + dir * t0;
          sp.e1 = P1 + dir * t1;
          sp.centerH = value[k - 1];
          for (V3* pt : {&sp.s0, &sp.s1, &sp.e0, &sp.e1}) {
            float y;
            if (solidNearestTop(hf, *pt, y)) pt->y = y;
          }
          gen.spans.push_back(sp);
        }
        start = -1;
      }
    }
  }
}

float clamp01(float x) { return (0.0f <= x) ? (x <= 1.0f ? x : 1.0f) : 0.0f; }

// Squared distance between segments p1-q1 and p2-q2 (closest points
// as in Ericson, Real-Time Collision Detection 5.1.9, with DayZ's epsilon and operation order).
float segSegDist2(V3 p1, V3 q1, V3 p2, V3 q2) {
  const V3 d2 = q2 - p2, d1 = q1 - p1, r = p1 - p2;
  const float a = d1.y * d1.y + d1.x * d1.x + d1.z * d1.z;
  const float e = d2.y * d2.y + d2.x * d2.x + d2.z * d2.z;
  const float f = r.y * d2.y + r.x * d2.x + r.z * d2.z;
  float s, t;
  if (a <= 1e-6f) {
    s = 0.0f;
    t = 1e-6f < e ? clamp01(f / e) : 0.0f;
  } else {
    const float c = r.y * d1.y + r.x * d1.x + r.z * d1.z;
    if (e <= 1e-6f) {
      t = 0.0f;
      s = clamp01(-(c / a));
    } else {
      const float b = d2.y * d1.y + d2.x * d1.x + d2.z * d1.z;
      const float denom = e * a - b * b;
      s = denom == 0.0f ? 0.0f : clamp01((b * f - c * e) / denom);
      t = (s * b + f) / e;
      if (0.0f <= t) {
        if (t > 1.0f) {
          t = 1.0f;
          s = clamp01((b - c) / a);
        }
      } else {
        t = 0.0f;
        s = clamp01(-(c / a));
      }
    }
  }
  const float dx = ((q2.x - p2.x) * t + p2.x) - ((q1.x - p1.x) * s + p1.x);
  const float dz = ((q2.z - p2.z) * t + p2.z) - ((q1.z - p1.z) * s + p1.z);
  const float dy = ((q2.y - p2.y) * t + p2.y) - ((q1.y - p1.y) * s + p1.y);
  return dy * dy + dx * dx + dz * dz;
}

// The two sides of an obstacle produce reversed copies of the same span; keep the
// longer one as a bidirectional link.
void mergeReverseDuplicates(Gen& gen) {
  const float thr = (gen.chf->cs * 5.0f) * (gen.chf->cs * 5.0f);
  for (size_t i = 0; i < gen.spans.size(); ++i) {
    Span& a = gen.spans[i];
    if (!a.valid) continue;
    for (size_t j = i + 1; j < gen.spans.size(); ++j) {
      Span& b = gen.spans[j];
      if (!b.valid) continue;
      const float dA = segSegDist2(a.e0, a.e1, b.s0, b.s1);
      const float dB = segSegDist2(a.s0, a.s1, b.e0, b.e1);
      if (dB < thr && dA < thr) {
        const V3 da = a.s1 - a.s0, db = b.s1 - b.s0;
        if (da.y * da.y + da.x * da.x + da.z * da.z <= db.y * db.y + db.x * db.x + db.z * db.z) {
          a.valid = 0;
          b.valid = 2;
          break;
        }
        // The kept span goes on comparing with later spans.
        a.valid = 2;
        b.valid = 0;
        a.centerH = b.centerH;
      }
    }
  }
}

// Links every 2 m along each span.
void emitLinks(const Gen& gen, const ClassRec& c, OffMeshLinks& out) {
  const float d2 = c.agentRadius * 2.0f;
  for (const Span& sp : gen.spans) {
    if (!sp.valid) continue;
    const V3 d = sp.s1 - sp.s0;
    const float len = std::sqrt(d.y * d.y + d.x * d.x + d.z * d.z);
    float rem, off;
    if (len >= 2.0f * d2) {
      rem = len - 2.0f * d2;
      const float f = std::floor(rem * 0.5f);
      off = (rem - (f + f)) * 0.5f + d2;
    } else {
      rem = 0.0f;
      off = len * 0.5f;
    }
    const int count = 1 - (int)(rem * -0.5f);
    for (int i = 0; i < count; ++i) {
      const float w = len > 0.0f ? off / len : 0.0f;
      const V3 start = sp.s0 + d * w;
      const V3 end = sp.e0 + d * w;
      const float a[3] = {start.x, start.y, start.z};
      const float b[3] = {end.x, end.y, end.z};
      out.add(a, b, sp.centerH, count == 1 ? off : 1.0f, c.flags, c.area, sp.valid != 1 ? 1 : 0, c.index);
      off += 2.0f;
    }
  }
}

}  // namespace

void generateJumpLinks(rcContext* ctx, const JumpLinkInput& in, OffMeshLinks& out) {
  // Ladders: vertex pairs become one-way ladder links (area 15, flag 0x40). Unused in shipped maps.
  for (const InputGroup* g : in.ladders)
    for (size_t i = 0; i + 1 < g->vertexCount(); i += 2)
      out.add(&g->verts[i * 3], &g->verts[(i + 1) * 3], 0.0f, 1.0f, 0x40, 15, 1, 0);

  const WorldParams& wp = *in.params;
  if (wp.links.empty()) return;

  // Contours over the final (eroded) compact heightfield with monotone regions.
  if (!rcBuildRegionsMonotone(ctx, *in.chf, 1, in.minRegionAreaByArea, in.mergeRegionArea)) return;
  std::unique_ptr<rcContourSet, void (*)(rcContourSet*)> cset(rcAllocContourSet(), rcFreeContourSet);
  if (!rcBuildContours(ctx, *in.chf, in.maxSimplificationError, in.maxVerticalError, in.maxEdgeLen, *cset,
                       RC_CONTOUR_TESS_WALL_EDGES))
    return;

  Gen gen;
  gen.hf = in.solid;
  gen.chf = in.chf;
  gen.cset = cset.get();

  std::vector<ClassRec> classes;
  for (const LinkClass& l : wp.links)
    classes.push_back({l.jumpHeight, l.jumpLength, l.jumpDropdownMin, l.jumpDropdownMax, l.agentHeight,
                       l.maxStepHeight, l.agentRadius, l.minCenterHeight, l.index, l.flags, l.area});

  float depth = 3.0f;
  for (const ClassRec& c : classes) depth = std::min(depth, std::max(wp.block.diameter * 3.0f, c.jumpLength));
  buildCentrelines(gen, depth, 1.5f);

  for (const ClassRec& c : classes) {
    gen.spans.clear();
    float total = 0;
    const std::vector<TrajSample> traj = buildTrajectory(c, in.chf->cs, total);
    sampleEdges(gen, traj, total, c);
    mergeReverseDuplicates(gen);
    emitLinks(gen, c, out);
  }
}

}  // namespace dznav
