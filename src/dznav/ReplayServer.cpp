#include "dznav/ReplayServer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace dznav {

ReplayStats serveCapture(const Capture& capture, const ReplayOptions& o, const std::atomic<bool>* stop,
                         const std::function<void(const std::string&)>& log) {
  WorldParams params = capture.params;
  WorldParamsBlock& b = params.block;
  if (o.crop && !capture.tiles.empty()) {
    int maxX = 0, maxZ = 0;
    for (const TileInput& t : capture.tiles) {
      maxX = std::max(maxX, t.x);
      maxZ = std::max(maxZ, t.z);
    }
    b.tilesX = maxX + 1;
    b.tilesZ = maxZ + 1;
    b.worldSizeX = float(b.tilesX) * b.tileWidth;
    b.worldSizeZ = float(b.tilesZ) * b.tileHeight;
  }
  if (!std::isnan(o.seaLevel)) b.deepSeaHeight = o.seaLevel;
  if (!o.islandFilter) b.filterIsolatedIslands = 0;

  std::vector<uint8_t> worldMessage(4);
  std::memcpy(worldMessage.data(), &kProtocolVersion, 4);
  const std::vector<uint8_t> body = serializeWorldParams(params);
  worldMessage.insert(worldMessage.end(), body.begin(), body.end());

  std::map<std::pair<int, int>, const TileInput*> tiles;
  for (const TileInput& t : capture.tiles) tiles[{t.x, t.z}] = &t;

  ReplayStats stats;
  std::mutex statsMutex;
  std::atomic<bool> clientLeft{false};
  auto finished = [&] { return (stop && stop->load()) || (o.exitAfterClient && clientLeft.load()); };

  Listener listener(o.address, o.port);
  log("listening on " + o.address + ":" + std::to_string(o.port) + ", " + std::to_string(tiles.size()) +
      " captured tiles, world " + std::to_string(b.tilesX) + " x " + std::to_string(b.tilesZ) + " tiles");

  std::vector<std::thread> threads;
  while (!finished()) {
    std::unique_ptr<Connection> conn = listener.accept(250);
    if (!conn) continue;
    {
      std::lock_guard lock(statsMutex);
      ++stats.clients;
    }
    log("client connected");
    threads.emplace_back([&, c = std::shared_ptr<Connection>(std::move(conn))] {
      int captured = 0, empty = 0;
      try {
        while (!(stop && stop->load())) {
          Message m;
          try {
            m = c->receive(1000);
          } catch (const NetError& e) {
            if (e.message.starts_with("timed out")) continue;
            throw;
          }
          if (m.id == kMsgRequestWorldParams) {
            c->send(kMsgWorldParams, worldMessage);
          } else if (m.id == kMsgRequestTile) {
            if (m.body.size() < 8) throw NetError{"short tile request"};
            int32_t xz[2];
            std::memcpy(xz, m.body.data(), 8);
            auto it = tiles.find({xz[0], xz[1]});
            TileInput t;
            if (it != tiles.end()) {
              t = *it->second;
              ++captured;
            } else {
              // Not captured: flat seabed 40 m down, as a client may not cope with a tile without
              // terrain. Like real terrain it reaches 24 m past the tile, wound the same way.
              t.x = xz[0];
              t.z = xz[1];
              t.cellClass = params.cellClasses.size() > 1 ? 1 : 0;
              const float x0 = float(t.x) * b.tileWidth, z0 = float(t.z) * b.tileHeight;
              const float x1 = x0 + b.tileWidth, z1 = z0 + b.tileHeight, y = -40.0f, pad = 24.0f;
              t.bmin[0] = x0;
              t.bmin[1] = y - 1;
              t.bmin[2] = z0;
              t.bmax[0] = x1;
              t.bmax[1] = y + 1;
              t.bmax[2] = z1;
              InputGroup terrain;
              terrain.area = 1;
              terrain.verts = {x0 - pad, y, z0 - pad, x1 + pad, y, z0 - pad, x0 - pad, y, z1 + pad,
                               x1 + pad, y, z0 - pad, x1 + pad, y, z1 + pad, x0 - pad, y, z1 + pad};
              t.groups.push_back(std::move(terrain));
              ++empty;
            }
            c->send(kMsgTile, serializeTileInput(t));
          } else if (m.id == kMsgFinish) {
            break;
          } else {
            log("ignoring message " + std::to_string(m.id));
          }
        }
      } catch (const NetError& e) {
        if (!e.message.starts_with("connection closed")) log("client error: " + e.message);
      }
      {
        std::lock_guard lock(statsMutex);
        stats.captured += captured;
        stats.empty += empty;
      }
      log("client left after " + std::to_string(captured) + " captured and " + std::to_string(empty) + " empty tiles");
      clientLeft = true;
    });
  }
  for (auto& t : threads) t.join();
  return stats;
}

}  // namespace dznav
