#include "dznav/Capture.h"

#include <cstring>

#include "dznav/NavMeshFormat.h"

namespace dznav {
namespace {

constexpr char kMagic[4] = {'D', 'Z', 'G', 'C'};
constexpr uint32_t kVersion = 1;

void writeBlock(std::FILE* f, const std::vector<uint8_t>& b) {
  const auto n = uint32_t(b.size());
  if (std::fwrite(&n, 4, 1, f) != 1 || (n && std::fwrite(b.data(), n, 1, f) != 1))
    throw WireError{"cannot write capture"};
}

}  // namespace

CaptureWriter::CaptureWriter(const std::string& path, const WorldParams& params) {
  f_ = std::fopen(path.c_str(), "wb");
  if (!f_) throw WireError{"cannot create " + path};
  std::fwrite(kMagic, 4, 1, f_);
  std::fwrite(&kVersion, 4, 1, f_);
  writeBlock(f_, serializeWorldParams(params));
}

CaptureWriter::~CaptureWriter() {
  if (f_) std::fclose(f_);
}

void CaptureWriter::add(const TileInput& tile) {
  writeBlock(f_, serializeTileInput(tile));
  std::fflush(f_);
}

Capture readCapture(const std::string& path) {
  const std::vector<uint8_t> bytes = readFileBytes(path);
  if (bytes.size() < 12 || std::memcmp(bytes.data(), kMagic, 4) != 0) throw WireError{"not a capture file"};
  uint32_t version;
  std::memcpy(&version, bytes.data() + 4, 4);
  if (version != kVersion) throw WireError{"unsupported capture version"};
  size_t pos = 8;
  auto next = [&]() -> std::span<const uint8_t> {
    uint32_t n;
    std::memcpy(&n, bytes.data() + pos, 4);
    if (pos + 4 + n > bytes.size()) throw WireError{"capture truncated"};
    std::span<const uint8_t> s(bytes.data() + pos + 4, n);
    pos += 4 + n;
    return s;
  };
  Capture c;
  c.params = parseWorldParams(next());
  // A capture interrupted mid-write keeps every complete tile.
  while (pos + 4 <= bytes.size()) {
    uint32_t n;
    std::memcpy(&n, bytes.data() + pos, 4);
    if (pos + 4 + n > bytes.size()) break;
    c.tiles.push_back(parseTileInput(next()));
  }
  return c;
}

}  // namespace dznav
