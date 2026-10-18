// Serves captured geometry (.dzgeo) over the data-server protocol, so any client can build from
// exactly the same input as dznav, to compare their output on identical geometry.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>

#include "dznav/Capture.h"
#include "dznav/DataServerClient.h"

namespace dznav {

struct ReplayOptions {
  std::string address = "127.0.0.1";
  uint16_t port = kDataServerPort;
  // Shrink the world to the captured tiles' bounding grid (from tile 0, 0), so a client that
  // generates every tile only requests those.
  bool crop = false;
  float seaLevel = std::numeric_limits<float>::quiet_NaN();  // override the captured tide
  bool islandFilter = true;  // false clears the world's filter-isolated-islands flag
  bool exitAfterClient = false;  // stop when the first client disconnects
};

struct ReplayStats {
  int clients = 0, captured = 0, empty = 0;
};

// Blocks until `stop` is set (or the first client leaves, with exitAfterClient). Tiles not in the
// capture are answered with flat seabed only. `log` gets one line per client event.
ReplayStats serveCapture(const Capture& capture, const ReplayOptions& options, const std::atomic<bool>* stop,
                         const std::function<void(const std::string&)>& log);

}  // namespace dznav
