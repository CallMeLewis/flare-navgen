// Geometry capture files (.dzgeo): world parameters plus captured tile inputs, so generation can
// be re-run offline without DayZDiag.
//
// Layout: "DZGC", u32 version (1), u32 size + world params body, then repeated u32 size + tile body
// (bodies use the data-server wire format).
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "dznav/WorldInput.h"

namespace dznav {

class CaptureWriter {
 public:
  CaptureWriter(const std::string& path, const WorldParams& params);
  ~CaptureWriter();
  void add(const TileInput& tile);

 private:
  std::FILE* f_ = nullptr;
};

struct Capture {
  WorldParams params;
  std::vector<TileInput> tiles;
};

Capture readCapture(const std::string& path);

}  // namespace dznav
