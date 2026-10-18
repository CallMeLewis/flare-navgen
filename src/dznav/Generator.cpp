#include "dznav/Generator.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include "dznav/Capture.h"
#include "dznav/DataServerClient.h"

namespace dznav {
namespace {

// Bounded multi-producer/multi-consumer queue with close().
template <typename T>
class Channel {
 public:
  explicit Channel(size_t capacity) : capacity_(capacity) {}
  bool push(T v) {
    std::unique_lock lock(m_);
    notFull_.wait(lock, [&] { return closed_ || q_.size() < capacity_; });
    if (closed_) return false;
    q_.push_back(std::move(v));
    notEmpty_.notify_one();
    return true;
  }
  std::optional<T> pop() {
    std::unique_lock lock(m_);
    notEmpty_.wait(lock, [&] { return closed_ || !q_.empty(); });
    if (q_.empty()) return std::nullopt;
    T v = std::move(q_.front());
    q_.pop_front();
    notFull_.notify_one();
    return v;
  }
  void close() {
    std::lock_guard lock(m_);
    closed_ = true;
    notEmpty_.notify_all();
    notFull_.notify_all();
  }

 private:
  std::mutex m_;
  std::condition_variable notEmpty_, notFull_;
  std::deque<T> q_;
  size_t capacity_;
  bool closed_ = false;
};

// One connection to one data server. clone() moves on to the next server in the list: DayZDiag
// serves one client at a time, so parallel fetching needs several servers.
class DataServerSource : public TileSource {
 public:
  struct Servers {
    std::vector<std::string> hosts;
    uint16_t port;
    std::atomic<size_t> next{1};
  };
  DataServerSource(std::shared_ptr<Servers> servers, size_t index)
      : servers_(std::move(servers)), client_(servers_->hosts[index % servers_->hosts.size()], servers_->port) {}
  WorldParams worldParams() override { return client_.requestWorldParams(); }
  bool fetch(int x, int z, const WorldParams& p, TileInput& out) override {
    // DayZDiag serves one client per frame and drops broken connections; retry a few times.
    for (int attempt = 0;; ++attempt) {
      try {
        out = client_.requestTile(x, z, p);
        return true;
      } catch (const NetError&) {
        client_.reset();
        if (attempt >= 4) throw;
        std::this_thread::sleep_for(std::chrono::seconds(2 * (attempt + 1)));
      }
    }
  }
  std::unique_ptr<TileSource> clone() override {
    return std::make_unique<DataServerSource>(servers_, servers_->next++);
  }

 private:
  std::shared_ptr<Servers> servers_;
  DataServerClient client_;
};

class CaptureSource : public TileSource {
 public:
  explicit CaptureSource(std::shared_ptr<const Capture> c) : c_(std::move(c)) {
    for (size_t i = 0; i < c_->tiles.size(); ++i) index_[{c_->tiles[i].x, c_->tiles[i].z}] = i;
  }
  WorldParams worldParams() override { return c_->params; }
  bool fetch(int x, int z, const WorldParams&, TileInput& out) override {
    auto it = index_.find({x, z});
    if (it == index_.end()) return false;
    out = c_->tiles[it->second];
    return true;
  }
  std::unique_ptr<TileSource> clone() override { return std::make_unique<CaptureSource>(c_); }

 private:
  std::shared_ptr<const Capture> c_;
  std::map<std::pair<int, int>, size_t> index_;
};

// Tile order: square blocks of blockSize tiles, row-major inside a block, which
// keeps the data server's streaming cache warm.
std::vector<std::pair<int, int>> tileOrder(int x0, int z0, int x1, int z1, int block) {
  block = std::max(1, block);
  std::vector<std::pair<int, int>> order;
  for (int bz = z0; bz <= z1; bz += block)
    for (int bx = x0; bx <= x1; bx += block)
      for (int z = bz; z <= std::min(z1, bz + block - 1); ++z)
        for (int x = bx; x <= std::min(x1, bx + block - 1); ++x) order.emplace_back(x, z);
  return order;
}

}  // namespace

std::unique_ptr<TileSource> makeDataServerSource(const std::vector<std::string>& hosts, uint16_t port) {
  if (hosts.empty()) throw NetError{"no data server given"};
  auto servers = std::make_shared<DataServerSource::Servers>();
  servers->hosts = hosts;
  servers->port = port;
  return std::make_unique<DataServerSource>(servers, 0);
}

std::unique_ptr<TileSource> makeCaptureSource(const std::string& path) {
  return std::make_unique<CaptureSource>(std::make_shared<Capture>(readCapture(path)));
}

std::unique_ptr<TileSource> makeCaptureSource(std::shared_ptr<const Capture> capture) {
  return std::make_unique<CaptureSource>(std::move(capture));
}

std::vector<std::pair<int, int>> plannedTiles(const WorldParams& params, const GenerateOptions& opt) {
  const WorldParamsBlock& b = params.block;
  const int x1 = opt.x1 < 0 ? b.tilesX - 1 : std::min(opt.x1, b.tilesX - 1);
  const int z1 = opt.z1 < 0 ? b.tilesZ - 1 : std::min(opt.z1, b.tilesZ - 1);
  const int x0 = std::max(0, opt.x0), z0 = std::max(0, opt.z0);
  auto order = tileOrder(x0, z0, x1, z1, b.blockSize);
  if (opt.include)
    order.erase(std::remove_if(order.begin(), order.end(), [&](const auto& t) { return !opt.include(t.first, t.second); }),
                order.end());
  return order;
}

GenerateResult generate(TileSource& source, const WorldParams& params, NavMeshAssembly& mesh, const TileBuildFn& build,
                        const GenerateOptions& opt, const std::function<void(const GenerateProgress&)>& onProgress) {
  const auto order = plannedTiles(params, opt);
  const int builders = opt.builders > 0 ? opt.builders : std::max(1, int(std::thread::hardware_concurrency()) - 1);
  const int fetchers = std::max(1, opt.fetchers);

  std::atomic<size_t> nextTile{0};
  Channel<TileInput> inputs(size_t(std::max(1, opt.maxQueuedInputs)));
  struct Built {
    TileBuildStats stats;
    std::vector<uint8_t> data;
    std::string error;
  };
  Channel<Built> results(size_t(builders) * 4);
  std::unique_ptr<CaptureWriter> capture;
  std::mutex captureMutex;
  if (!opt.capturePath.empty()) capture = std::make_unique<CaptureWriter>(opt.capturePath, params);

  GenerateResult result;
  std::mutex resultMutex;
  std::atomic<int> fetched{0};
  auto cancelled = [&] { return opt.cancel && opt.cancel->load(); };
  auto fail = [&](const std::string& msg) {
    std::lock_guard lock(resultMutex);
    result.errors.push_back(msg);
  };

  // Fetchers: each owns a source (connection).
  std::vector<std::thread> fetchThreads;
  std::atomic<int> fetchersLeft{fetchers};
  for (int f = 0; f < fetchers; ++f) {
    fetchThreads.emplace_back([&, f] {
      std::unique_ptr<TileSource> own;
      try {
        own = f == 0 ? nullptr : source.clone();
      } catch (const std::exception& e) {
        fail(std::string("source: ") + e.what());
      } catch (const NetError& e) {
        fail("source: " + e.message);
      }
      TileSource& src = own ? *own : source;
      if (f == 0 || own) {
        for (size_t i; !cancelled() && (i = nextTile++) < order.size();) {
          TileInput in;
          try {
            if (!src.fetch(order[i].first, order[i].second, params, in)) continue;
          } catch (const NetError& e) {
            fail("tile (" + std::to_string(order[i].first) + "," + std::to_string(order[i].second) + "): " + e.message);
            break;
          } catch (const WireError& e) {
            fail("tile (" + std::to_string(order[i].first) + "," + std::to_string(order[i].second) + "): " + e.message);
            continue;
          }
          ++fetched;
          if (opt.onFetched) opt.onFetched(in.x, in.z);
          if (capture) {
            std::lock_guard lock(captureMutex);
            capture->add(in);
          }
          if (!inputs.push(std::move(in))) break;
        }
      }
      if (--fetchersLeft == 0) inputs.close();
    });
  }

  // Builders.
  std::vector<std::thread> buildThreads;
  std::atomic<int> buildersLeft{builders};
  for (int i = 0; i < builders; ++i) {
    buildThreads.emplace_back([&] {
      while (auto in = inputs.pop()) {
        if (cancelled()) continue;
        Built out;
        out.stats.x = in->x;
        out.stats.z = in->z;
        const auto t0 = std::chrono::steady_clock::now();
        try {
          out.data = build(params, *in, out.stats);
        } catch (const std::exception& e) {
          out.error = e.what();
        } catch (const FormatError& e) {
          out.error = e.message;
        }
        out.stats.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        results.push(std::move(out));
      }
      if (--buildersLeft == 0) results.close();
    });
  }

  // Assembly on this thread (Detour is not thread-safe).
  GenerateProgress progress;
  progress.total = int(order.size());
  const auto start = std::chrono::steady_clock::now();
  while (auto r = results.pop()) {
    if (r->error.empty() && r->data.empty()) {
      ++result.empty;
      progress.lastOutcome = TileOutcome::Empty;
      mesh.removeTile(r->stats.x, r->stats.z);
    } else if (r->error.empty()) {
      try {
        mesh.setTile(std::move(r->data));
        ++result.built;
        progress.lastOutcome = TileOutcome::Built;
      } catch (const FormatError& e) {
        r->error = e.message;
      }
    }
    if (!r->error.empty()) {
      ++result.failed;
      progress.lastOutcome = TileOutcome::Failed;
      fail("tile (" + std::to_string(r->stats.x) + "," + std::to_string(r->stats.z) + "): " + r->error);
    }
    progress.lastError = r->error.empty() ? nullptr : &r->error;
    progress.fetched = fetched.load();
    progress.built = result.built + result.empty;
    progress.failed = result.failed;
    progress.elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    progress.last = &r->stats;
    if (onProgress) onProgress(progress);
  }
  for (auto& t : fetchThreads) t.join();
  for (auto& t : buildThreads) t.join();
  return result;
}

}  // namespace dznav
