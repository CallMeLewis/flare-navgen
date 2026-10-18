// Minimal RGBA raster with PNG output, used for top-down navmesh previews.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dznav {

struct Rgba {
  uint8_t r, g, b, a;
};

class Raster {
 public:
  Raster(int width, int height, Rgba fill);

  int width() const { return w_; }
  int height() const { return h_; }

  void blend(int x, int y, Rgba c);
  // Fills a convex polygon given in pixel coordinates.
  void fillConvex(const float* xy, int count, Rgba c);
  void line(float x0, float y0, float x1, float y1, Rgba c);
  void disc(float cx, float cy, float r, Rgba c);

  void writePng(const std::string& path) const;

 private:
  int w_, h_;
  std::vector<uint8_t> px_;
};

}  // namespace dznav
