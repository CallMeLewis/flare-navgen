// Top-down navmesh preview images.
#pragma once

#include <string>

#include "dznav/NavMeshFormat.h"
#include "dznav/Raster.h"

namespace dznav {

struct PreviewOptions {
  // World-space XZ rectangle to draw.
  float minX = 0, minZ = 0, maxX = 0, maxZ = 0;
  float pixelsPerMeter = 1.0f;
  bool edges = true;
  bool links = true;
  bool obstacles = true;
};

Rgba areaColor(int area);
Raster renderPreview(const NavMeshFile& file, const PreviewOptions& options);

}  // namespace dznav
