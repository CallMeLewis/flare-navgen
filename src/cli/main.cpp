#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "Json.h"

#include "dznav/Capture.h"
#include "dznav/Compare.h"
#include "dznav/DataServerClient.h"
#include "dznav/Generator.h"
#include "dznav/TileBuilder.h"
#include "dznav/NavMeshAssembly.h"
#include "dznav/NavMeshFormat.h"
#include "dznav/Preview.h"
#include "dznav/ReplayServer.h"

using namespace dznav;
using dznav::cli::Json;
using dznav::cli::emitLine;

namespace {

// --json: print results and progress as JSON lines (see docs/cli-json.md).
bool gJson = false;

// Set by Ctrl+C, or by a "stop" line on stdin in --json mode: generation stops and saves what it has.
std::atomic<bool> gCancel{false};

void onInterrupt(int) {
  if (gCancel.exchange(true)) {
    std::signal(SIGINT, SIG_DFL);  // a second Ctrl+C quits without saving
    std::raise(SIGINT);
  }
  std::signal(SIGINT, onInterrupt);
}

void watchStdinForStop() {
  std::thread([] {
    std::string line;
    while (std::getline(std::cin, line))
      if (line == "stop" || line == "stop\r") gCancel = true;
  }).detach();
}

int usage() {
  std::fprintf(stderr,
               "dznav - DayZ navmesh toolkit\n\n"
               "Usage:\n"
               "  dznav info <file.nm> [--tiles]             Summary of a navmesh file (--tiles: per-tile list, JSON only)\n"
               "  dznav roundtrip <file.nm>                  Parse and re-serialize, verify byte equality\n"
               "  dznav tile <file.nm> <x> <z>               Dump one tile\n"
               "  dznav export-obj <file.nm> <out.obj> [x0 z0 x1 z1]\n"
               "                                             Export polygons (optionally a tile range) as OBJ\n"
               "  dznav render <file.nm> <out.png> <minX> <minZ> <maxX> <maxZ> [px/m]\n"
               "                                             Top-down preview of a world-space rectangle\n"
               "  dznav diff <a.nm> <b.nm> [top N]           Compare two navmeshes tile by tile\n"
               "  dznav verify <reference.nm> <ours.nm> [--ignore-order]\n"
               "                                             Byte-compare common tiles (canonical form);\n"
               "                                             --ignore-order: obstacles and doors as sets\n"
               "  dznav merge <base.nm> <patch.nm> <out.nm> [x0 z0 x1 z1]\n"
               "                                             Replace base tiles with patch tiles (optionally a\n"
               "                                             tile range) and relink the seams\n"
               "  dznav resave <in.nm> <out.nm>              Load into Detour and save again\n"
               "  dznav islands <in.nm> <out.nm> <x> <y> <z> [--all-heights]\n"
               "                                             Re-run the isolated-island filter from a seed\n"
               "\n"
               "Data server (DayZDiag_x64 -server -startNavmeshDataServer):\n"
               "  dznav probe [host] [x z]                   Print world params (and summarize one tile)\n"
               "  dznav capture <out.dzgeo> <x0> <z0> <x1> <z1> [host]\n"
               "                                             Save tile input geometry for offline generation\n"
               "  dznav capture-info <file.dzgeo>            Summarize a capture\n"
               "  dznav replay-server <file.dzgeo>... [--bind addr] [--port N] [--crop] [--sea-level M]\n"
               "                      [--no-island-filter] [--once]\n"
               "                                             Serve captured geometry as a data server, so any\n"
               "                                             client builds from it\n"
               "\n"
               "Generation:\n"
               "  dznav generate <out.nm> [options]\n"
               "      --server <host[,host]>  data server(s) to fetch geometry from (default 127.0.0.1);\n"
               "                              each serves one client, so list several to fetch in parallel\n"
               "      --capture <file.dzgeo>  use captured geometry instead of a server\n"
               "      --range x0 z0 x1 z1     tile range (inclusive); default: whole world / whole capture\n"
               "      --base <file.nm>        patch the generated tiles into an existing navmesh\n"
               "      --skip-existing         with --base: only build tiles the base doesn't have (resume)\n"
               "      --checkpoint N          save the output every N tiles\n"
               "      --record <file.dzgeo>   save fetched geometry while generating\n"
               "      --threads N             builder threads (default: cores - 1)\n"
               "      --connections N         fetch connections (default: one per server)\n"
               "      --sea-level M           override the server's (tide-dependent) sea level\n"
               "      --doors closed|open|enabled   initial door state baked into the mesh (default closed)\n"
               "      --no-links              skip jump/climb links\n"
               "      --no-island-filter      don't flag polygons unreachable from the seed\n"
               "      --seed x y z            island filter seed (default: world params)\n"
               "  Ctrl+C stops generation and saves the tiles built so far (Ctrl+C again quits at once).\n"
               "\n"
               "Add --json to generate, probe, capture, info, tile, render, verify, diff, merge or islands for\n"
               "JSON-lines output (docs/cli-json.md). With --json, generate also stops and saves on a \"stop\"\n"
               "line on stdin.\n");
  return 2;
}

const Tile* findTile(const NavMeshFile& f, int x, int z) {
  for (const Tile& t : f.tiles)
    if (t.header.x == x && t.header.y == z) return &t;
  return nullptr;
}

int cmdInfo(const std::string& path, bool listTiles) {
  NavMeshFile f = readNavMeshFile(path);
  const SetHeader& h = f.header;
  if (!gJson) std::printf("tiles: %d (%d x %d), tile size %.2f x %.2f, origin (%.2f, %.2f, %.2f)\n", h.numTiles, h.tilesX,
              h.tilesZ, h.tileWidth, h.tileHeight, h.orig[0], h.orig[1], h.orig[2]);
  if (!gJson) std::printf("maxTiles %d, maxPolys/tile %d\n", h.maxTiles, h.maxPolys);

  size_t polys = 0, verts = 0, offMesh = 0, parts = 0, partPolys = 0, obstacles = 0, detail = 0;
  std::map<float, int> cellSizes;
  std::map<int, size_t> areas;
  std::map<int, size_t> flags;
  for (const Tile& t : f.tiles) {
    polys += t.polys.size();
    verts += t.verts.size() / 3;
    offMesh += t.offMeshCons.size();
    parts += t.parts.size();
    partPolys += t.partPolys.size();
    obstacles += t.obstacles.size();
    detail += t.detailMeshes.size();
    cellSizes[1.0f / t.header.bvQuantFactor]++;
    for (const Poly& p : t.polys) {
      areas[p.area()]++;
      flags[p.flags]++;
    }
  }
  if (gJson) {
    Json j;
    j.obj().field("event", "info").field("path", path).field("tiles", f.tiles.size());
    j.field("tilesX", h.tilesX).field("tilesZ", h.tilesZ).field("tileWidth", h.tileWidth).field("tileHeight", h.tileHeight);
    j.key("origin").arr().value(h.orig[0]).value(h.orig[1]).value(h.orig[2]).end();
    j.field("maxTiles", h.maxTiles).field("maxPolys", h.maxPolys);
    j.field("polys", polys).field("verts", verts).field("links", offMesh).field("doorParts", parts);
    j.field("doorPolys", partPolys).field("obstacles", obstacles);
    if (!f.tiles.empty()) {
      const MeshHeader& t = f.tiles[0].header;
      j.key("agent").obj().field("height", t.walkableHeight).field("radius", t.walkableRadius);
      j.field("climb", t.walkableClimb).end();
    }
    j.key("cellSizes").arr();
    for (auto [cs, n] : cellSizes) j.arr().value(cs).value(n).end();
    j.end().key("areas").obj();
    for (auto [a, n] : areas) j.field(std::to_string(a).c_str(), n);
    j.end().key("flags").obj();
    for (auto [fl, n] : flags) j.field(std::to_string(fl).c_str(), n);
    j.end();
    if (listTiles) {
      // [x, z, bytes, polys, links, obstacles, door parts] per tile
      j.key("tileList").arr();
      for (const Tile& t : f.tiles)
        j.arr().value(t.header.x).value(t.header.y).value(t.dataSize()).value(t.polys.size())
            .value(t.offMeshCons.size()).value(t.obstacles.size()).value(t.parts.size()).end();
      j.end();
    }
    emitLine(j.end().str());
    return 0;
  }
  std::printf("polys %zu, verts %zu, off-mesh links %zu, door parts %zu (%zu polys), obstacles %zu, detail meshes %zu\n",
              polys, verts, offMesh, parts, partPolys, obstacles, detail);
  if (!f.tiles.empty()) {
    const MeshHeader& t = f.tiles[0].header;
    std::printf("agent: height %.2f radius %.2f climb %.2f\n", t.walkableHeight, t.walkableRadius, t.walkableClimb);
  }
  std::printf("cell sizes:");
  for (auto [cs, n] : cellSizes) std::printf(" %.3f x%d", cs, n);
  std::printf("\nareas:");
  for (auto [a, n] : areas) std::printf(" %d:%zu", a, n);
  std::printf("\nflags:");
  for (auto [fl, n] : flags) std::printf(" 0x%x:%zu", fl, n);
  std::printf("\n");
  return 0;
}

int cmdRoundtrip(const std::string& path) {
  auto t0 = std::chrono::steady_clock::now();
  std::vector<uint8_t> original = readFileBytes(path);
  NavMeshFile f = parseNavMesh(original);
  auto t1 = std::chrono::steady_clock::now();
  std::vector<uint8_t> rewritten = serializeNavMesh(f);
  auto t2 = std::chrono::steady_clock::now();
  auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
  std::printf("parsed %zu tiles in %.0f ms, serialized in %.0f ms\n", f.tiles.size(), ms(t0, t1), ms(t1, t2));
  if (original.size() != rewritten.size()) {
    std::printf("FAIL: size %zu vs %zu\n", original.size(), rewritten.size());
    return 1;
  }
  for (size_t i = 0; i < original.size(); ++i) {
    if (original[i] != rewritten[i]) {
      std::printf("FAIL: first difference at byte %zu\n", i);
      return 1;
    }
  }
  std::printf("OK: %zu bytes identical\n", original.size());
  return 0;
}

int cmdTile(const std::string& path, int x, int z) {
  NavMeshFile f = readNavMeshFile(path);
  const Tile* t = findTile(f, x, z);
  if (!t) throw FormatError{"no tile (" + std::to_string(x) + "," + std::to_string(z) + ") in " + path};
  const MeshHeader& h = t->header;
  if (gJson) {
    char ref[24];
    std::snprintf(ref, sizeof ref, "0x%016llx", (unsigned long long)t->ref);
    Json j;
    j.obj().field("event", "tileDetail").field("x", h.x).field("z", h.y).field("ref", ref);
    j.key("bmin").arr().value(h.bmin[0]).value(h.bmin[1]).value(h.bmin[2]).end();
    j.key("bmax").arr().value(h.bmax[0]).value(h.bmax[1]).value(h.bmax[2]).end();
    j.field("cellSize", 1.0f / h.bvQuantFactor).field("bytes", t->dataSize()).field("polys", h.polyCount);
    j.field("verts", h.vertCount);
    std::map<int, int> areas, flags;
    for (const Poly& p : t->polys) {
      if (p.type() != 0) continue;  // off-mesh link polys are listed under links
      areas[p.area()]++;
      flags[p.flags]++;
    }
    j.key("areas").obj();
    for (auto [a, n] : areas) j.field(std::to_string(a).c_str(), n);
    j.end().key("flags").obj();
    for (auto [fl, n] : flags) j.field(std::to_string(fl).c_str(), n);
    j.end().key("links").arr();
    for (const OffMeshConnection& c : t->offMeshCons) {
      j.obj().field("class", c.userId).field("bidirectional", (c.flags & 1) != 0);
      j.key("from").arr().value(c.pos[0]).value(c.pos[1]).value(c.pos[2]).end();
      j.key("to").arr().value(c.pos[3]).value(c.pos[4]).value(c.pos[5]).end();
      j.field("centerHeight", c.centerHeight).field("radius", c.rad);
      j.field("area", t->polys[c.poly].area()).field("flags", t->polys[c.poly].flags).end();
    }
    j.end().key("doors").arr();
    for (const TilePart& part : t->parts) {
      char id[24];
      std::snprintf(id, sizeof id, "0x%016llx", (unsigned long long)part.objectId);
      int n = 0;
      for (int32_t k = part.firstPoly; k >= 0; k = t->partPolys[k].next) ++n;
      j.obj().field("objectId", id).field("part", part.partIndex / 2).field("state", part.partIndex % 2);
      j.field("polys", n).end();
    }
    j.end().key("obstacles").arr();
    for (const Obstacle& o : t->obstacles) j.arr().value(o.pos[0]).value(o.pos[1]).value(o.pos[2]).value(o.radius).end();
    emitLine(j.end().end().str());
    return 0;
  }
  std::printf("tile (%d,%d) ref 0x%016llx salt %u index %u\n", h.x, h.y, (unsigned long long)t->ref, refSalt(t->ref),
              refTile(t->ref));
  std::printf("bounds (%.2f %.2f %.2f) - (%.2f %.2f %.2f), cell %.3f\n", h.bmin[0], h.bmin[1], h.bmin[2], h.bmax[0],
              h.bmax[1], h.bmax[2], 1.0f / h.bvQuantFactor);
  std::printf("polys %d verts %d links %d bv %d offmesh %d (base %d) parts %zu partPolys %zu obstacles %zu\n",
              h.polyCount, h.vertCount, h.maxLinkCount, h.bvNodeCount, h.offMeshConCount, h.offMeshBase,
              t->parts.size(), t->partPolys.size(), t->obstacles.size());
  for (size_t i = 0; i < t->polys.size(); ++i) {
    const Poly& p = t->polys[i];
    std::printf("  poly %zu: area %d type %d flags 0x%04x verts", i, p.area(), p.type(), p.flags);
    for (int k = 0; k < p.vertCount; ++k) std::printf(" %u", p.verts[k]);
    std::printf("\n");
  }
  for (size_t i = 0; i < t->offMeshCons.size(); ++i) {
    const OffMeshConnection& c = t->offMeshCons[i];
    std::printf("  link %zu: class %u (%.2f %.2f %.2f) -> (%.2f %.2f %.2f) centre %.2f reserved %.1f rad %.2f poly %u flags %u side %u\n",
                i, c.userId, c.pos[0], c.pos[1], c.pos[2], c.pos[3], c.pos[4], c.pos[5], c.centerHeight, c.reserved,
                c.rad, c.poly, c.flags, c.side);
  }
  for (const TilePart& p : t->parts) {
    std::printf("  part object 0x%016llx index %u polys:", (unsigned long long)p.objectId, p.partIndex);
    for (int32_t k = p.firstPoly; k >= 0; k = t->partPolys[k].next) std::printf(" %u", t->partPolys[k].poly);
    std::printf("\n");
  }
  for (const Obstacle& o : t->obstacles)
    std::printf("  obstacle (%.2f %.2f %.2f) r %.2f\n", o.pos[0], o.pos[1], o.pos[2], o.radius);
  return 0;
}

int cmdExportObj(const std::string& path, const std::string& out, int x0, int z0, int x1, int z1) {
  NavMeshFile f = readNavMeshFile(path);
  std::ofstream o(out);
  o << "# dznav export of " << path << "\n";
  size_t base = 1;
  for (const Tile& t : f.tiles) {
    if (t.header.x < x0 || t.header.x > x1 || t.header.y < z0 || t.header.y > z1) continue;
    o << "o tile_" << t.header.x << "_" << t.header.y << "\n";
    for (size_t v = 0; v < t.verts.size(); v += 3)
      o << "v " << t.verts[v] << " " << t.verts[v + 1] << " " << -t.verts[v + 2] << "\n";
    for (const Poly& p : t.polys) {
      if (p.type() != 0) continue;
      o << "f";
      for (int k = 0; k < p.vertCount; ++k) o << " " << base + p.verts[k];
      o << "\n";
    }
    base += t.verts.size() / 3;
  }
  std::printf("wrote %s\n", out.c_str());
  return 0;
}

int cmdDiff(const std::string& pathA, const std::string& pathB, int top) {
  NavMeshFile a = readNavMeshFile(pathA);
  NavMeshFile b = readNavMeshFile(pathB);
  CompareResult r = compareNavMeshes(a, b);
  if (!gJson) std::printf("tiles compared %zu, only in A %d, only in B %d\n", r.tiles.size(), r.onlyInA, r.onlyInB);

  long long polysA = 0, polysB = 0, offA = 0, offB = 0, obsA = 0, obsB = 0, partsA = 0, partsB = 0;
  int sameCell = 0;
  double delta = 0, total = 0;
  for (const TileDiff& d : r.tiles) {
    polysA += d.a.polys, polysB += d.b.polys;
    offA += d.a.offMesh, offB += d.b.offMesh;
    obsA += d.a.obstacles, obsB += d.b.obstacles;
    partsA += d.a.parts, partsB += d.b.parts;
    sameCell += d.a.cellSize == d.b.cellSize;
    delta += d.areaDelta;
    total += std::max(d.a.walkableArea(), d.b.walkableArea());
  }
  std::map<int, int> ids;
  for (auto& [id, v] : r.totalAreaA) ids[id];
  for (auto& [id, v] : r.totalAreaB) ids[id];
  std::vector<const TileDiff*> worst;
  for (const TileDiff& d : r.tiles) worst.push_back(&d);
  std::sort(worst.begin(), worst.end(), [](auto* x, auto* y) { return x->areaDelta > y->areaDelta; });
  if (gJson) {
    Json j;
    j.obj().field("event", "diff").field("compared", r.tiles.size()).field("onlyInA", r.onlyInA);
    j.field("onlyInB", r.onlyInB).field("sameCellSize", sameCell);
    j.key("a").obj().field("polys", polysA).field("links", offA).field("obstacles", obsA).field("doorParts", partsA).end();
    j.key("b").obj().field("polys", polysB).field("links", offB).field("obstacles", obsB).field("doorParts", partsB).end();
    j.key("areaM2").obj();  // area id -> [A, B] in square metres
    for (auto& [id, unused] : ids)
      j.key(std::to_string(id).c_str()).arr().value(r.totalAreaA.count(id) ? r.totalAreaA[id] : 0.0)
          .value(r.totalAreaB.count(id) ? r.totalAreaB[id] : 0.0).end();
    j.end().field("mismatchPercent", total > 0 ? 100.0 * delta / total : 0.0);
    j.key("worst").arr();
    for (int i = 0; i < top && i < int(worst.size()); ++i) {
      const TileDiff& d = *worst[i];
      j.obj().field("x", d.a.x).field("z", d.a.z).field("areaDelta", d.areaDelta);
      j.key("polys").arr().value(d.a.polys).value(d.b.polys).end();
      j.key("links").arr().value(d.a.offMesh).value(d.b.offMesh).end();
      j.key("cellSize").arr().value(d.a.cellSize).value(d.b.cellSize).end().end();
    }
    emitLine(j.end().end().str());
    return 0;
  }
  std::printf("%-12s %14s %14s\n", "", "A", "B");
  std::printf("%-12s %14lld %14lld\n", "polys", polysA, polysB);
  std::printf("%-12s %14lld %14lld\n", "off-mesh", offA, offB);
  std::printf("%-12s %14lld %14lld\n", "obstacles", obsA, obsB);
  std::printf("%-12s %14lld %14lld\n", "door parts", partsA, partsB);
  std::printf("same cell size: %d of %zu tiles\n", sameCell, r.tiles.size());
  std::printf("area by type (km^2):\n");
  for (auto& [id, unused] : ids) {
    const double va = r.totalAreaA.count(id) ? r.totalAreaA[id] : 0;
    const double vb = r.totalAreaB.count(id) ? r.totalAreaB[id] : 0;
    std::printf("  area %2d %12.4f %12.4f\n", id, va / 1e6, vb / 1e6);
  }
  std::printf("area mismatch: %.3f%% of walkable area\n", total > 0 ? 100.0 * delta / total : 0.0);
  for (int i = 0; i < top && i < int(worst.size()); ++i) {
    const TileDiff& d = *worst[i];
    std::printf("  tile (%d,%d): area delta %.1f m^2, polys %d/%d, links %d/%d, cell %.2f/%.2f\n", d.a.x, d.a.z,
                d.areaDelta, d.a.polys, d.b.polys, d.a.offMesh, d.b.offMesh, d.a.cellSize, d.b.cellSize);
  }
  return 0;
}

int cmdMerge(const std::string& basePath, const std::string& patchPath, const std::string& out, int x0, int z0,
             int x1, int z1) {
  NavMeshFile base = readNavMeshFile(basePath);
  NavMeshFile patch = readNavMeshFile(patchPath);
  if (base.header.tilesX != patch.header.tilesX || base.header.tilesZ != patch.header.tilesZ ||
      base.header.tileWidth != patch.header.tileWidth)
    throw FormatError{"tile grids differ"};
  NavMeshAssembly mesh;
  mesh.load(base);
  int replaced = 0;
  for (const Tile& t : patch.tiles) {
    if (t.header.x < x0 || t.header.x > x1 || t.header.y < z0 || t.header.y > z1) continue;
    Tile fresh = t;
    // Links are rebuilt by addTile.
    std::fill(fresh.links.begin(), fresh.links.end(), Link{});
    for (Poly& p : fresh.polys) p.firstLink = kNullLink;
    mesh.setTile(serializeTile(fresh));
    ++replaced;
  }
  writeNavMeshFile(out, mesh.toFile());
  if (gJson)
    emitLine(Json().obj().field("event", "merge").field("replaced", replaced).field("output", out).end().str());
  else
    std::printf("replaced %d tiles, wrote %s\n", replaced, out.c_str());
  return 0;
}

int cmdVerify(const std::string& refPath, const std::string& candPath, bool ignoreOrder) {
  const VerifyResult r = verifyTiles(readNavMeshFile(refPath), readNavMeshFile(candPath), ignoreOrder);
  if (gJson) {
    Json j;
    j.obj().field("event", "verify").field("compared", r.compared).field("identical", r.identical);
    j.field("ignoredIsolated", r.ignoredIsolated).field("ignoredOrder", r.ignoredOrder);
    j.key("different").arr();
    for (auto [x, z] : r.different) j.arr().value(x).value(z).end();
    emitLine(j.end().end().str());
    return r.different.empty() ? 0 : 1;
  }
  std::printf("%d of %d common tiles identical (ignoring links, snapped endpoints, obstacle Y, door state%s%s)\n",
              r.identical, r.compared, r.ignoredIsolated ? ", isolated flag: the reference has none" : "",
              r.ignoredOrder ? ", object order" : "");
  for (size_t i = 0; i < r.different.size() && i < 20; ++i)
    std::printf("  differs: tile (%d,%d)\n", r.different[i].first, r.different[i].second);
  if (r.different.size() > 20) std::printf("  ... and %zu more\n", r.different.size() - 20);
  return r.different.empty() ? 0 : 1;
}

int cmdIslands(int argc, char** argv) {
  NavMeshAssembly mesh;
  mesh.load(readNavMeshFile(argv[2]));
  const float seed[3] = {float(std::atof(argv[4])), float(std::atof(argv[5])), float(std::atof(argv[6]))};
  const bool allHeights = argc >= 8 && std::string(argv[7]) == "--all-heights";
  const auto r = mesh.filterIsolatedIslands(seed, allHeights);
  if (!r.seedFound) throw FormatError{"no polygon near the seed (try --all-heights)"};
  writeNavMeshFile(argv[3], mesh.toFile());
  if (gJson) {
    Json j;
    j.obj().field("event", "islands").field("seedFound", true);
    j.key("seedPoint").arr().value(r.seedPoint[0]).value(r.seedPoint[1]).value(r.seedPoint[2]).end();
    j.field("reachable", r.reachable).field("isolated", r.isolated).field("output", argv[3]);
    emitLine(j.end().str());
    return 0;
  }
  std::printf("seed snapped to (%.2f %.2f %.2f): %zu reachable, %zu isolated polys\n", r.seedPoint[0],
              r.seedPoint[1], r.seedPoint[2], r.reachable, r.isolated);
  std::printf("wrote %s\n", argv[3]);
  return 0;
}

int cmdResave(const std::string& in, const std::string& out) {
  NavMeshAssembly mesh;
  mesh.load(readNavMeshFile(in));
  writeNavMeshFile(out, mesh.toFile());
  std::printf("wrote %s\n", out.c_str());
  return 0;
}

void printWorldParams(const WorldParams& p) {
  const WorldParamsBlock& b = p.block;
  std::printf("world %.1f x %.1f m, tiles %d x %d of %.1f x %.1f m\n", b.worldSizeX, b.worldSizeZ, b.tilesX,
              b.tilesZ, b.tileWidth, b.tileHeight);
  std::printf("deep sea height %.2f, deep water depth %.2f, contour error %.3f, obstacles %u, block %d\n",
              b.deepSeaHeight, b.deepWaterDepth, b.contourMaxError, b.obstacles, b.blockSize);
  std::printf("filter islands %u, seed (%.1f, %.1f, %.1f)\n", b.filterIsolatedIslands, b.seedPosition[0],
              b.seedPosition[1], b.seedPosition[2]);
  std::printf("agent: stand %.2f crouch %.2f prone %.2f diameter %.2f step %.2f slope %.1f\n", b.standHeight,
              b.crouchHeight, b.proneHeight, b.diameter, b.maxStepHeight, b.maxSlope);
  for (size_t i = 0; i < p.cellClasses.size(); ++i) {
    const CellClass& c = p.cellClasses[i];
    std::printf("cell class %zu: %d cells over %.2f m (cs %.3f) [unk %u %u]\n", i, c.cells, c.tileWidth,
                c.tileWidth / c.cells, c.unk04, c.unk0C);
  }
  for (const LinkClass& l : p.links)
    std::printf("link %u: typeId %d len %.2f height %.2f centre %.2f dropdown %.2f..%.2f agent h %.2f step %.2f r %.2f "
                "flags 0x%x area %u\n",
                l.index, l.typeId, l.jumpLength, l.jumpHeight, l.minCenterHeight, l.jumpDropdownMin,
                l.jumpDropdownMax, l.agentHeight, l.maxStepHeight, l.agentRadius, l.flags, l.area);
}

void printTileInput(const TileInput& t) {
  std::printf("tile (%d,%d) cell class %u bounds (%.2f %.2f %.2f)-(%.2f %.2f %.2f), %zu groups\n", t.x, t.z,
              t.cellClass, t.bmin[0], t.bmin[1], t.bmin[2], t.bmax[0], t.bmax[1], t.bmax[2], t.groups.size());
  for (const InputGroup& g : t.groups)
    std::printf("  area %2u: %zu parts, %zu verts\n", g.area, g.parts.size(), g.vertexCount());
}

std::string worldParamsJson(const WorldParams& p) {
  const WorldParamsBlock& b = p.block;
  Json j;
  j.obj().field("worldSizeX", b.worldSizeX).field("worldSizeZ", b.worldSizeZ);
  j.field("tilesX", b.tilesX).field("tilesZ", b.tilesZ).field("tileWidth", b.tileWidth).field("tileHeight", b.tileHeight);
  j.field("seaLevel", b.deepSeaHeight).field("deepWaterDepth", b.deepWaterDepth);
  j.field("contourMaxError", b.contourMaxError).field("obstacles", b.obstacles != 0).field("blockSize", b.blockSize);
  j.field("filterIsolatedIslands", b.filterIsolatedIslands != 0);
  j.key("seed").arr().value(b.seedPosition[0]).value(b.seedPosition[1]).value(b.seedPosition[2]).end();
  j.key("agent").obj().field("standHeight", b.standHeight).field("crouchHeight", b.crouchHeight);
  j.field("proneHeight", b.proneHeight).field("diameter", b.diameter).field("maxStepHeight", b.maxStepHeight);
  j.field("maxSlope", b.maxSlope).end();
  j.key("cellClasses").arr();
  for (const CellClass& c : p.cellClasses)
    j.obj().field("cells", c.cells).field("width", c.tileWidth).field("cellSize", c.tileWidth / c.cells).end();
  j.end().key("linkClasses").arr();
  for (const LinkClass& l : p.links) {
    j.obj().field("index", l.index).field("typeId", l.typeId).field("jumpLength", l.jumpLength);
    j.field("jumpHeight", l.jumpHeight).field("minCenterHeight", l.minCenterHeight);
    j.field("dropdownMin", l.jumpDropdownMin).field("dropdownMax", l.jumpDropdownMax);
    j.field("agentHeight", l.agentHeight).field("maxStepHeight", l.maxStepHeight).field("agentRadius", l.agentRadius);
    j.field("flags", l.flags).field("area", l.area).end();
  }
  return j.end().end().str();
}

int cmdProbe(const std::string& host, int x, int z) {
  DataServerClient client(host);
  WorldParams p = client.requestWorldParams();
  if (!gJson) {
    printWorldParams(p);
    if (x >= 0) printTileInput(client.requestTile(x, z, p));
    return 0;
  }
  Json j;
  j.obj().field("event", "world").field("host", host).key("params").raw(worldParamsJson(p));
  if (x >= 0) {
    const TileInput t = client.requestTile(x, z, p);
    j.key("tile").obj().field("x", t.x).field("z", t.z).field("cellClass", t.cellClass);
    j.key("bmin").arr().value(t.bmin[0]).value(t.bmin[1]).value(t.bmin[2]).end();
    j.key("bmax").arr().value(t.bmax[0]).value(t.bmax[1]).value(t.bmax[2]).end();
    j.key("groups").arr();
    for (const InputGroup& g : t.groups)
      j.obj().field("area", g.area).field("parts", g.parts.size()).field("verts", g.vertexCount()).end();
    j.end().end();
  }
  emitLine(j.end().str());
  return 0;
}

int cmdCapture(const std::string& out, int x0, int z0, int x1, int z1, const std::string& host) {
  DataServerClient client(host);
  WorldParams p = client.requestWorldParams();
  CaptureWriter w(out, p);
  const auto t0 = std::chrono::steady_clock::now();
  int n = 0;
  const int total = (x1 - x0 + 1) * (z1 - z0 + 1);
  for (int z = z0; z <= z1; ++z)
    for (int x = x0; x <= x1; ++x) {
      w.add(client.requestTile(x, z, p));
      ++n;
      const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      if (gJson) {
        emitLine(Json().obj().field("event", "progress").field("done", n).field("total", total).field("elapsed", s)
                     .field("rate", n / std::max(s, 1e-3)).end().str());
      } else {
        std::printf("\rcaptured %d/%d tiles (%.1f tiles/s)", n, total, n / std::max(s, 1e-3));
        std::fflush(stdout);
      }
    }
  if (gJson)
    emitLine(Json().obj().field("event", "done").field("output", out).field("tiles", n).end().str());
  else
    std::printf("\nwrote %s\n", out.c_str());
  return 0;
}

int cmdGenerate(int argc, char** argv) {
  const std::string out = argv[2];
  std::string server = "127.0.0.1", capturePath, basePath, recordPath;
  GenerateOptions go;
  go.fetchers = 1;
  TileBuildOptions bo;
  bool haveRange = false, islandFilter = true, haveSeed = false, skipExisting = false, fetchersGiven = false;
  int checkpointEvery = 0;
  float seed[3] = {0, 0, 0};
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) throw FormatError{"missing value for " + a};
      return argv[++i];
    };
    if (a == "--server") server = next();
    else if (a == "--capture") capturePath = next();
    else if (a == "--base") basePath = next();
    else if (a == "--record") recordPath = next();
    else if (a == "--threads") go.builders = std::atoi(next());
    else if (a == "--connections") {
      go.fetchers = std::atoi(next());
      fetchersGiven = true;
    }
    else if (a == "--sea-level") bo.seaLevel = float(std::atof(next()));
    else if (a == "--no-links") bo.jumpLinks = false;
    else if (a == "--no-island-filter") islandFilter = false;
    else if (a == "--skip-existing") skipExisting = true;
    else if (a == "--checkpoint") checkpointEvery = std::atoi(next());
    else if (a == "--seed") {
      for (float& v : seed) v = float(std::atof(next()));
      haveSeed = true;
    }
    else if (a == "--doors") {
      const std::string d = next();
      bo.doors = d == "open" ? DoorStatePolicy::Open : d == "enabled" ? DoorStatePolicy::Enabled : DoorStatePolicy::Closed;
    } else if (a == "--range") {
      go.x0 = std::atoi(next());
      go.z0 = std::atoi(next());
      go.x1 = std::atoi(next());
      go.z1 = std::atoi(next());
      haveRange = true;
    } else throw FormatError{"unknown option " + a};
  }
  go.capturePath = recordPath;

  std::unique_ptr<TileSource> source;
  std::shared_ptr<const Capture> capture;
  if (capturePath.empty()) {
    std::vector<std::string> hosts;
    for (size_t pos = 0; pos <= server.size();) {
      const size_t comma = std::min(server.find(',', pos), server.size());
      if (comma > pos) hosts.push_back(server.substr(pos, comma - pos));
      pos = comma + 1;
    }
    if (!fetchersGiven) go.fetchers = int(hosts.size());
    source = makeDataServerSource(hosts, kDataServerPort);
  } else {
    capture = std::make_shared<Capture>(readCapture(capturePath));
    source = makeCaptureSource(capture);
  }
  const WorldParams params = source->worldParams();
  if (capture && !haveRange) {
    // Default to the captured tiles' bounding range.
    go.x0 = go.z0 = 1 << 30;
    go.x1 = go.z1 = -1;
    for (const TileInput& t : capture->tiles) {
      go.x0 = std::min(go.x0, t.x);
      go.z0 = std::min(go.z0, t.z);
      go.x1 = std::max(go.x1, t.x);
      go.z1 = std::max(go.z1, t.z);
    }
  }
  NavMeshAssembly mesh;
  if (!basePath.empty()) {
    mesh.load(readNavMeshFile(basePath));
  } else {
    const float orig[3] = {0, 0, 0};
    mesh.init(params.block.tilesX, params.block.tilesZ, params.block.tileWidth, params.block.tileHeight, orig);
  }

  if (skipExisting) {
    if (basePath.empty()) throw FormatError{"--skip-existing needs --base"};
    go.include = [&](int x, int z) { return !mesh.hasTile(x, z); };
  }

  const auto planned = plannedTiles(params, go);
  if (gJson) {
    const WorldParamsBlock& b = params.block;
    Json j;
    j.obj().field("event", "start").field("output", out).field("tiles", planned.size());
    j.key("range").arr().value(std::max(0, go.x0)).value(std::max(0, go.z0));
    j.value(go.x1 < 0 ? b.tilesX - 1 : std::min(go.x1, b.tilesX - 1));
    j.value(go.z1 < 0 ? b.tilesZ - 1 : std::min(go.z1, b.tilesZ - 1)).end();
    j.key("world").obj().field("tilesX", b.tilesX).field("tilesZ", b.tilesZ).field("tileWidth", b.tileWidth);
    j.field("tileHeight", b.tileHeight).end();
    if (capture) j.field("capture", capturePath);
    else j.field("server", server);
    if (!basePath.empty()) j.field("base", basePath).field("skipExisting", skipExisting);
    emitLine(j.end().str());
    go.onFetched = [](int x, int z) {
      emitLine(Json().obj().field("event", "fetched").field("x", x).field("z", z).end().str());
    };
  }

  const auto t0 = std::chrono::steady_clock::now();
  TileBuildFn build = [&](const WorldParams& wp, const TileInput& in, TileBuildStats& st) {
    return buildTile(wp, in, st, bo);
  };
  auto save = [&](const std::string& path) {
    // Write to a temporary file first so an interrupted save never destroys the previous one.
    writeNavMeshFile(path + ".tmp", mesh.toFile());
    std::remove(path.c_str());
    if (std::rename((path + ".tmp").c_str(), path.c_str()) != 0) throw FormatError{"cannot rename to " + path};
  };
  auto emitProgress = [&](const GenerateProgress& p) {
    const int done = p.built + p.failed;
    const double rate = done / std::max(p.elapsedSec, 1e-3);
    const double eta = rate > 0 ? (p.total - done) / rate : 0;
    if (gJson) {
      emitLine(Json().obj().field("event", "progress").field("done", done).field("total", p.total)
                   .field("fetched", p.fetched).field("failed", p.failed).field("elapsed", p.elapsedSec)
                   .field("rate", rate).field("eta", eta).end().str());
    } else {
      const int e = int(eta);
      std::printf("\r%d/%d tiles (%d failed), %.1f tiles/s, %d:%02d:%02d left   ", done, p.total, p.failed, rate,
                  e / 3600, e / 60 % 60, e % 60);
      std::fflush(stdout);
    }
  };

  go.cancel = &gCancel;
  std::signal(SIGINT, onInterrupt);
  if (gJson) watchStdinForStop();
  int strayTiles = 0, sinceCheckpoint = 0;
  GenerateProgress lastProgress;
  auto lastEmit = std::chrono::steady_clock::now() - std::chrono::hours(1);
  GenerateResult r = generate(*source, params, mesh, build, go, [&](const GenerateProgress& p) {
    if (p.last && p.last->strayPartIds) ++strayTiles;
    if (gJson && p.last) {
      const TileBuildStats& t = *p.last;
      Json j;
      j.obj().field("event", "tile").field("x", t.x).field("z", t.z);
      j.field("outcome", p.lastOutcome == TileOutcome::Built   ? "built"
                         : p.lastOutcome == TileOutcome::Empty ? "empty"
                                                               : "failed");
      if (p.lastOutcome == TileOutcome::Built)
        j.field("bytes", t.dataSize).field("polys", t.polys).field("verts", t.verts).field("links", t.offMesh)
            .field("obstacles", t.obstacles);
      if (t.strayPartIds) j.field("strayPartIds", t.strayPartIds);
      j.field("ms", t.buildMs);
      if (p.lastError) j.field("error", *p.lastError);
      emitLine(j.end().str());
    }
    // Throttle progress output; JSON consumers get at most 4 updates a second.
    lastProgress = p;
    lastProgress.last = nullptr;
    const auto now = std::chrono::steady_clock::now();
    if (!gJson || now - lastEmit >= std::chrono::milliseconds(250)) {
      lastEmit = now;
      emitProgress(p);
    }
    if (checkpointEvery > 0 && ++sinceCheckpoint >= checkpointEvery) {
      sinceCheckpoint = 0;
      save(out);
      if (gJson)
        emitLine(Json().obj().field("event", "saved").field("path", out).field("checkpoint", true).end().str());
    }
  });
  if (gJson && lastProgress.total) emitProgress(lastProgress);
  const bool cancelled = gCancel.load();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (!gJson) {
    std::printf("\n%sbuilt %d tiles, %d empty, %d failed in %.1f s\n", cancelled ? "stopped: " : "", r.built, r.empty,
                r.failed, seconds);
    for (size_t i = 0; i < r.errors.size() && i < 20; ++i) std::printf("  %s\n", r.errors[i].c_str());
    if (strayTiles)
      std::printf("%d tiles had out-of-range door part ids, which were ignored\n",
                  strayTiles);
  }

  // A stopped run skips the island filter; resuming runs it over the whole mesh.
  if (!cancelled && islandFilter && params.block.filterIsolatedIslands) {
    if (!haveSeed) std::copy(params.block.seedPosition, params.block.seedPosition + 3, seed);
    const auto f = mesh.filterIsolatedIslands(seed);
    if (gJson) {
      Json j;
      j.obj().field("event", "islands").field("seedFound", f.seedFound);
      j.key("seed").arr().value(seed[0]).value(seed[1]).value(seed[2]).end();
      if (f.seedFound)
        j.key("seedPoint").arr().value(f.seedPoint[0]).value(f.seedPoint[1]).value(f.seedPoint[2]).end()
            .field("reachable", f.reachable).field("isolated", f.isolated);
      emitLine(j.end().str());
    } else if (f.seedFound) {
      std::printf("island filter: seed (%.1f %.1f %.1f) -> %zu reachable, %zu isolated polys\n", f.seedPoint[0],
                  f.seedPoint[1], f.seedPoint[2], f.reachable, f.isolated);
    } else {
      std::printf("island filter skipped: no polygon near seed (%.1f %.1f %.1f) in the generated area\n", seed[0],
                  seed[1], seed[2]);
    }
  }

  // Don't replace the output with an empty mesh when stopped before the first tile.
  const bool saved = !cancelled || r.built + r.empty > 0;
  if (saved) save(out);
  const bool resumable = cancelled || (!r.errors.empty() && !r.failed);
  if (gJson) {
    Json j;
    j.obj().field("event", "done").field("output", out).field("saved", saved).field("cancelled", cancelled);
    j.field("built", r.built).field("empty", r.empty).field("failed", r.failed).field("seconds", seconds);
    j.field("strayTiles", strayTiles).field("resumable", resumable).field("errorCount", r.errors.size());
    j.key("errors").arr();
    for (size_t i = 0; i < r.errors.size() && i < 100; ++i) j.value(r.errors[i]);
    emitLine(j.end().end().str());
  } else {
    if (saved) std::printf("wrote %s\n", out.c_str());
    if (resumable && saved)
      std::printf("generation stopped early; continue with: --base %s --skip-existing\n", out.c_str());
  }
  if (cancelled) return 130;
  return (r.failed || !r.errors.empty()) ? 1 : 0;
}

int cmdReplayServer(int argc, char** argv) {
  ReplayOptions o;
  Capture capture;
  std::map<std::pair<int, int>, size_t> index;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) throw FormatError{"missing value for " + a};
      return argv[++i];
    };
    if (a == "--bind") o.address = next();
    else if (a == "--port") o.port = uint16_t(std::atoi(next()));
    else if (a == "--crop") o.crop = true;
    else if (a == "--sea-level") o.seaLevel = float(std::atof(next()));
    else if (a == "--no-island-filter") o.islandFilter = false;
    else if (a == "--once") o.exitAfterClient = true;
    else if (a.starts_with("--")) throw FormatError{"unknown option " + a};
    else {
      Capture c = readCapture(a);
      if (index.empty()) capture.params = c.params;
      for (TileInput& t : c.tiles) {
        auto [it, added] = index.try_emplace({t.x, t.z}, capture.tiles.size());
        if (added) capture.tiles.push_back(std::move(t));
        else capture.tiles[it->second] = std::move(t);
      }
    }
  }
  if (index.empty()) throw FormatError{"no capture given"};
  std::signal(SIGINT, onInterrupt);
  const ReplayStats st = serveCapture(capture, o, &gCancel, [](const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
  });
  std::printf("served %d clients: %d captured tiles, %d empty\n", st.clients, st.captured, st.empty);
  return 0;
}

int cmdCaptureInfo(const std::string& path) {
  Capture c = readCapture(path);
  printWorldParams(c.params);
  std::printf("%zu tiles\n", c.tiles.size());
  for (size_t i = 0; i < c.tiles.size() && i < 5; ++i) printTileInput(c.tiles[i]);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // --json may appear anywhere; remove it before the commands read their arguments.
  int kept = 1;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--json") == 0)
      gJson = true;
    else
      argv[kept++] = argv[i];
  }
  argc = kept;
  if (argc < 2) return usage();
  const std::string cmd = argv[1];
  // kind: "usage", "file", "network" or "protocol"
  auto fail = [](const std::string& kind, const std::string& message) {
    if (gJson) {
      emitLine(Json().obj().field("event", "error").field("kind", kind).field("message", message).end().str());
    } else {
      const char* label = kind == "network" ? "network error" : kind == "protocol" ? "protocol error" : "error";
      std::fprintf(stderr, "\n%s: %s\n", label, message.c_str());
    }
    return 1;
  };
  static const char* const kJsonCommands[] = {"generate", "probe", "capture", "info",   "tile",
                                              "render",   "verify", "diff",   "merge", "islands"};
  if (gJson && std::none_of(std::begin(kJsonCommands), std::end(kJsonCommands), [&](const char* c) { return cmd == c; }))
    return fail("usage", "--json is not supported by '" + cmd + "'");
  try {
    if (cmd == "probe")
      return cmdProbe(argc >= 3 ? argv[2] : "127.0.0.1", argc >= 5 ? std::atoi(argv[3]) : -1,
                      argc >= 5 ? std::atoi(argv[4]) : -1);
    if (cmd == "capture" && argc >= 7)
      return cmdCapture(argv[2], std::atoi(argv[3]), std::atoi(argv[4]), std::atoi(argv[5]), std::atoi(argv[6]),
                        argc >= 8 ? argv[7] : "127.0.0.1");
    if (cmd == "capture-info" && argc >= 3) return cmdCaptureInfo(argv[2]);
    if (cmd == "replay-server" && argc >= 3) return cmdReplayServer(argc, argv);
    if (cmd == "generate" && argc >= 3) return cmdGenerate(argc, argv);
    if (argc < 3) return usage();
    if (cmd == "info") return cmdInfo(argv[2], argc >= 4 && std::string(argv[3]) == "--tiles");
    if (cmd == "roundtrip") return cmdRoundtrip(argv[2]);
    if (cmd == "tile" && argc >= 5) return cmdTile(argv[2], std::atoi(argv[3]), std::atoi(argv[4]));
    if (cmd == "export-obj" && argc >= 4) {
      int r[4] = {0, 0, 1 << 30, 1 << 30};
      if (argc >= 8)
        for (int i = 0; i < 4; ++i) r[i] = std::atoi(argv[4 + i]);
      return cmdExportObj(argv[2], argv[3], r[0], r[1], r[2], r[3]);
    }
    if (cmd == "resave" && argc >= 4) return cmdResave(argv[2], argv[3]);
    if (cmd == "islands" && argc >= 7) return cmdIslands(argc, argv);
    if (cmd == "verify" && argc >= 4)
      return cmdVerify(argv[2], argv[3], argc >= 5 && std::string(argv[4]) == "--ignore-order");
    if (cmd == "merge" && argc >= 5) {
      int r[4] = {0, 0, 1 << 30, 1 << 30};
      if (argc >= 9)
        for (int i = 0; i < 4; ++i) r[i] = std::atoi(argv[5 + i]);
      return cmdMerge(argv[2], argv[3], argv[4], r[0], r[1], r[2], r[3]);
    }
    if (cmd == "diff" && argc >= 4) return cmdDiff(argv[2], argv[3], argc >= 5 ? std::atoi(argv[4]) : 10);
    if (cmd == "render" && argc >= 8) {
      PreviewOptions o;
      o.minX = float(std::atof(argv[4]));
      o.minZ = float(std::atof(argv[5]));
      o.maxX = float(std::atof(argv[6]));
      o.maxZ = float(std::atof(argv[7]));
      o.pixelsPerMeter = argc >= 9 ? float(std::atof(argv[8])) : 1.0f;
      NavMeshFile f = readNavMeshFile(argv[2]);
      const Raster image = renderPreview(f, o);
      image.writePng(argv[3]);
      if (gJson)
        emitLine(Json().obj().field("event", "render").field("output", argv[3]).field("width", image.width())
                     .field("height", image.height()).end().str());
      else
        std::printf("wrote %s\n", argv[3]);
      return 0;
    }
  } catch (const FormatError& e) {
    return fail("file", e.message);
  } catch (const NetError& e) {
    return fail("network", e.message);
  } catch (const WireError& e) {
    return fail("protocol", e.message);
  } catch (const std::exception& e) {
    return fail("file", e.what());
  }
  return usage();
}
