// Parallel navmesh generation: fetch tile inputs, build tiles, link them into a navmesh.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dznav/NavMeshAssembly.h"
#include "dznav/WorldInput.h"

namespace dznav {

// Supplies world parameters and tile inputs. fetch() may be called from several threads, but each
// thread gets its own source from clone().
class TileSource {
 public:
  virtual ~TileSource() = default;
  virtual WorldParams worldParams() = 0;
  // Returns false if the tile has no input (it is then skipped).
  virtual bool fetch(int x, int z, const WorldParams& params, TileInput& out) = 0;
  // A source for another thread (e.g. another connection to the same server).
  virtual std::unique_ptr<TileSource> clone() = 0;
};

// clone() connects to the next host in the list (round robin).
std::unique_ptr<TileSource> makeDataServerSource(const std::vector<std::string>& hosts, uint16_t port);
// Serves tiles from a .dzgeo capture (all of it is loaded into memory).
std::unique_ptr<TileSource> makeCaptureSource(const std::string& path);
struct Capture;
std::unique_ptr<TileSource> makeCaptureSource(std::shared_ptr<const Capture> capture);

struct TileBuildStats {
  int x = 0, z = 0;
  int dataSize = 0, polys = 0, verts = 0, offMesh = 0, obstacles = 0;
  int strayPartIds = 0;  // polys whose part id is out of range (ignored)
  double buildMs = 0;
};

// Builds a DayZ tile blob (tile version 12, links not yet built) from one tile's input.
// Returns an empty vector for tiles without walkable surface.
using TileBuildFn = std::function<std::vector<uint8_t>(const WorldParams&, const TileInput&, TileBuildStats&)>;

struct GenerateOptions {
  int x0 = 0, z0 = 0, x1 = -1, z1 = -1;  // inclusive tile range; x1 < 0 means the whole world
  int builders = 0;                      // 0 = hardware threads - 1
  int fetchers = 4;                      // parallel source connections
  int maxQueuedInputs = 64;              // bounds memory held by fetched-but-unbuilt tiles
  std::atomic<bool>* cancel = nullptr;
  std::string capturePath;  // if set, record every fetched tile input here
  // Optional filter: tiles for which it returns false are skipped (used to resume).
  std::function<bool(int x, int z)> include;
  // Optional: called from the fetcher threads when a tile's input has arrived and is queued for building.
  std::function<void(int x, int z)> onFetched;
};

enum class TileOutcome { Built, Empty, Failed };

struct GenerateProgress {
  int total = 0, fetched = 0, built = 0, failed = 0;
  double elapsedSec = 0;
  // The tile that just finished.
  const TileBuildStats* last = nullptr;
  TileOutcome lastOutcome = TileOutcome::Built;
  const std::string* lastError = nullptr;  // set when lastOutcome is Failed
};

struct GenerateResult {
  int built = 0, empty = 0, failed = 0;
  std::vector<std::string> errors;
};

// The tiles generate() will process, in processing order (range clamped to the world, include filter applied).
std::vector<std::pair<int, int>> plannedTiles(const WorldParams& params, const GenerateOptions& options);

GenerateResult generate(TileSource& source, const WorldParams& params, NavMeshAssembly& mesh, const TileBuildFn& build,
                        const GenerateOptions& options,
                        const std::function<void(const GenerateProgress&)>& onProgress = {});

}  // namespace dznav
