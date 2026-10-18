#include "dznav/Preview.h"

#include <algorithm>

namespace dznav {

Rgba areaColor(int area) {
  switch (area) {
    case 1: return {96, 156, 72, 255};    // terrain
    case 2: return {120, 180, 230, 255};  // water
    case 3: return {50, 110, 200, 255};   // waterdeep
    case 4: return {90, 200, 210, 255};   // watersea
    case 5: return {25, 55, 120, 255};    // waterdeepsea
    case 8: return {214, 140, 60, 255};   // building
    case 12: return {240, 220, 70, 255};  // dooropened
    case 13: return {220, 60, 50, 255};   // doorclosed
    case 15: return {220, 80, 220, 255};  // ladder
    case 16: return {150, 90, 200, 255};  // crawl
    case 17: return {230, 140, 190, 255}; // crouch
    default:
      if (area >= 48) return {255, 255, 255, 255};  // jump links
      return {160, 160, 160, 255};
  }
}

Raster renderPreview(const NavMeshFile& file, const PreviewOptions& o) {
  const float s = o.pixelsPerMeter;
  const int w = std::max(1, int((o.maxX - o.minX) * s));
  const int h = std::max(1, int((o.maxZ - o.minZ) * s));
  Raster img(w, h, {18, 18, 22, 255});
  auto px = [&](float x) { return (x - o.minX) * s; };
  auto py = [&](float z) { return (o.maxZ - z) * s; };

  std::vector<float> pts;
  for (const Tile& t : file.tiles) {
    const MeshHeader& hd = t.header;
    if (hd.bmax[0] < o.minX || hd.bmin[0] > o.maxX || hd.bmax[2] < o.minZ || hd.bmin[2] > o.maxZ) continue;
    for (const Poly& p : t.polys) {
      if (p.type() != 0) continue;
      pts.clear();
      for (int k = 0; k < p.vertCount; ++k) {
        const float* v = &t.verts[p.verts[k] * 3];
        pts.push_back(px(v[0]));
        pts.push_back(py(v[2]));
      }
      Rgba c = areaColor(p.area());
      if (p.flags & 0x8000) c = {uint8_t(c.r / 3 + 60), uint8_t(c.g / 3), uint8_t(c.b / 3), 255};
      if (p.flags & 0x0002) c = {uint8_t(c.r / 2), uint8_t(c.g / 2), uint8_t(c.b / 2), 255};
      img.fillConvex(pts.data(), p.vertCount, c);
    }
    if (o.edges && s >= 2.0f) {
      for (const Poly& p : t.polys) {
        if (p.type() != 0) continue;
        for (int k = 0; k < p.vertCount; ++k) {
          const float* a = &t.verts[p.verts[k] * 3];
          const float* b = &t.verts[p.verts[(k + 1) % p.vertCount] * 3];
          const bool border = p.neis[k] == 0;
          img.line(px(a[0]), py(a[2]), px(b[0]), py(b[2]), border ? Rgba{0, 0, 0, 200} : Rgba{0, 0, 0, 60});
        }
      }
    }
    if (o.obstacles)
      for (const Obstacle& ob : t.obstacles)
        img.disc(px(ob.pos[0]), py(ob.pos[2]), std::max(1.0f, ob.radius * s), {30, 30, 30, 220});
    if (o.links)
      for (const OffMeshConnection& c : t.offMeshCons) {
        Rgba lc = c.flags & 1 ? Rgba{255, 255, 255, 200} : Rgba{255, 230, 0, 200};
        img.line(px(c.pos[0]), py(c.pos[2]), px(c.pos[3]), py(c.pos[5]), lc);
        img.disc(px(c.pos[3]), py(c.pos[5]), std::max(1.0f, 0.15f * s), lc);
      }
  }
  return img;
}

}  // namespace dznav
