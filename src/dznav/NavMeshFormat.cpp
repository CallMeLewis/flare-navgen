#include "dznav/NavMeshFormat.h"

#include <cstring>
#include <fstream>

namespace dznav {
namespace {

class Reader {
 public:
  explicit Reader(std::span<const uint8_t> data) : data_(data) {}

  template <typename T>
  T read() {
    T v;
    need(sizeof(T));
    std::memcpy(&v, data_.data() + pos_, sizeof(T));
    pos_ += sizeof(T);
    return v;
  }

  template <typename T>
  void readArray(std::vector<T>& out, size_t count) {
    need(count * sizeof(T));
    out.resize(count);
    if (count) std::memcpy(out.data(), data_.data() + pos_, count * sizeof(T));
    pos_ += count * sizeof(T);
  }

  size_t pos() const { return pos_; }
  size_t remaining() const { return data_.size() - pos_; }

 private:
  void need(size_t n) const {
    if (pos_ + n > data_.size()) throw FormatError{"unexpected end of data"};
  }

  std::span<const uint8_t> data_;
  size_t pos_ = 0;
};

class Writer {
 public:
  explicit Writer(std::vector<uint8_t>& out) : out_(out) {}

  template <typename T>
  void write(const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out_.insert(out_.end(), p, p + sizeof(T));
  }

  template <typename T>
  void writeArray(const std::vector<T>& v) {
    if (v.empty()) return;
    const auto* p = reinterpret_cast<const uint8_t*>(v.data());
    out_.insert(out_.end(), p, p + v.size() * sizeof(T));
  }

 private:
  std::vector<uint8_t>& out_;
};

void checkCount(int32_t n, const char* what) {
  if (n < 0) throw FormatError{std::string("negative ") + what};
}

}  // namespace

size_t Tile::extraDataSize() const {
  return 12 + parts.size() * sizeof(TilePart) + partPolys.size() * sizeof(TilePartPoly) +
         obstacles.size() * sizeof(Obstacle);
}

size_t Tile::dataSize() const {
  return sizeof(MeshHeader) + verts.size() * sizeof(float) + polys.size() * sizeof(Poly) +
         links.size() * sizeof(Link) + detailMeshes.size() * sizeof(PolyDetail) +
         detailVerts.size() * sizeof(float) + detailTris.size() + bvTree.size() * sizeof(BVNode) +
         offMeshCons.size() * sizeof(OffMeshConnection) + extraDataSize();
}

void syncTileHeader(Tile& t) {
  MeshHeader& h = t.header;
  h.magic = kTileMagic;
  h.version = kTileVersion;
  h.vertCount = int32_t(t.verts.size() / 3);
  h.polyCount = int32_t(t.polys.size());
  h.maxLinkCount = int32_t(t.links.size());
  h.detailMeshCount = int32_t(t.detailMeshes.size());
  h.detailVertCount = int32_t(t.detailVerts.size() / 3);
  h.detailTriCount = int32_t(t.detailTris.size() / 4);
  h.bvNodeCount = int32_t(t.bvTree.size());
  h.offMeshConCount = int32_t(t.offMeshCons.size());
  h.extraDataSize = int32_t(t.extraDataSize());
}

Tile parseTile(std::span<const uint8_t> data, uint64_t ref) {
  Reader r(data);
  Tile t;
  t.ref = ref;
  t.header = r.read<MeshHeader>();
  const MeshHeader& h = t.header;
  if (h.magic != kTileMagic) throw FormatError{"bad tile magic"};
  if (h.version != kTileVersion) throw FormatError{"unsupported tile version " + std::to_string(h.version)};
  checkCount(h.vertCount, "vertCount");
  checkCount(h.polyCount, "polyCount");
  checkCount(h.maxLinkCount, "maxLinkCount");
  checkCount(h.detailMeshCount, "detailMeshCount");
  checkCount(h.detailVertCount, "detailVertCount");
  checkCount(h.detailTriCount, "detailTriCount");
  checkCount(h.bvNodeCount, "bvNodeCount");
  checkCount(h.offMeshConCount, "offMeshConCount");
  checkCount(h.extraDataSize, "extraDataSize");

  r.readArray(t.verts, size_t(h.vertCount) * 3);
  r.readArray(t.polys, h.polyCount);
  r.readArray(t.links, h.maxLinkCount);
  r.readArray(t.detailMeshes, h.detailMeshCount);
  r.readArray(t.detailVerts, size_t(h.detailVertCount) * 3);
  r.readArray(t.detailTris, size_t(h.detailTriCount) * 4);
  r.readArray(t.bvTree, h.bvNodeCount);
  r.readArray(t.offMeshCons, h.offMeshConCount);

  const size_t extraStart = r.pos();
  const auto partCount = r.read<uint32_t>();
  const auto partPolyCount = r.read<uint32_t>();
  r.readArray(t.parts, partCount);
  r.readArray(t.partPolys, partPolyCount);
  const auto obstacleCount = r.read<uint32_t>();
  r.readArray(t.obstacles, obstacleCount);
  if (r.pos() - extraStart != size_t(h.extraDataSize)) throw FormatError{"extra section size mismatch"};
  if (r.remaining() != 0) throw FormatError{"trailing bytes after tile data"};
  return t;
}

std::vector<uint8_t> serializeTile(const Tile& t) {
  std::vector<uint8_t> out;
  out.reserve(t.dataSize());
  Writer w(out);
  w.write(t.header);
  w.writeArray(t.verts);
  w.writeArray(t.polys);
  w.writeArray(t.links);
  w.writeArray(t.detailMeshes);
  w.writeArray(t.detailVerts);
  w.writeArray(t.detailTris);
  w.writeArray(t.bvTree);
  w.writeArray(t.offMeshCons);
  w.write(uint32_t(t.parts.size()));
  w.write(uint32_t(t.partPolys.size()));
  w.writeArray(t.parts);
  w.writeArray(t.partPolys);
  w.write(uint32_t(t.obstacles.size()));
  w.writeArray(t.obstacles);
  return out;
}

NavMeshFile parseNavMesh(std::span<const uint8_t> bytes) {
  Reader r(bytes);
  NavMeshFile f;
  f.header = r.read<SetHeader>();
  const SetHeader& h = f.header;
  if (h.magic != kSetMagic) throw FormatError{"Invalid header format (bad magic)"};
  if (h.version != kSetVersion) throw FormatError{"Invalid version " + std::to_string(h.version)};
  checkCount(h.numTiles, "numTiles");

  if (h.maxTiles < h.numTiles) throw FormatError{"numTiles exceeds maxTiles"};

  // The game skips the slot-indexed offset table and reads numTiles records back to back.
  std::vector<uint32_t> offsets;
  r.readArray(offsets, h.maxTiles);
  const size_t base = r.pos();

  f.tiles.reserve(h.numTiles);
  size_t pos = base;
  for (int32_t i = 0; i < h.numTiles; ++i) {
    if (pos + sizeof(TileRecordHeader) > bytes.size()) throw FormatError{"tile record out of range"};
    TileRecordHeader rec;
    std::memcpy(&rec, bytes.data() + pos, sizeof(rec));
    if (rec.tileRef == 0 || rec.dataSize <= 0) throw FormatError{"empty tile record (the game rejects these)"};
    if (pos + sizeof(rec) + size_t(rec.dataSize) > bytes.size()) throw FormatError{"tile data out of range"};
    Tile t = parseTile(bytes.subspan(pos + sizeof(rec), rec.dataSize), rec.tileRef);
    t.recordPad = rec.pad;
    f.tiles.push_back(std::move(t));
    pos += sizeof(rec) + size_t(rec.dataSize);
  }
  return f;
}

std::vector<uint8_t> serializeNavMesh(const NavMeshFile& f) {
  std::vector<uint8_t> out;
  size_t total = sizeof(SetHeader) + f.tiles.size() * 4;
  for (const Tile& t : f.tiles) total += sizeof(TileRecordHeader) + t.dataSize();
  out.reserve(total);

  Writer w(out);
  SetHeader h = f.header;
  h.magic = kSetMagic;
  h.version = kSetVersion;
  h.numTiles = int32_t(f.tiles.size());
  if (h.maxTiles < h.numTiles) throw FormatError{"more tiles than maxTiles"};
  w.write(h);

  std::vector<uint32_t> table(size_t(h.maxTiles), kNoTile);
  uint64_t offset = 0;
  for (const Tile& t : f.tiles) {
    const uint32_t slot = refTile(t.ref);
    if (slot >= table.size()) throw FormatError{"tile ref outside maxTiles"};
    if (offset > 0xfffffffeull) throw FormatError{"navmesh larger than 4 GB"};
    table[slot] = uint32_t(offset);
    offset += sizeof(TileRecordHeader) + t.dataSize();
  }
  w.writeArray(table);
  for (const Tile& t : f.tiles) {
    std::vector<uint8_t> data = serializeTile(t);
    w.write(TileRecordHeader{t.ref, int32_t(data.size()), t.recordPad});
    out.insert(out.end(), data.begin(), data.end());
  }
  return out;
}

std::vector<uint8_t> readFileBytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) throw FormatError{"cannot open " + path};
  const auto size = static_cast<size_t>(in.tellg());
  std::vector<uint8_t> bytes(size);
  in.seekg(0);
  in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size));
  if (!in) throw FormatError{"cannot read " + path};
  return bytes;
}

void writeFileBytes(const std::string& path, std::span<const uint8_t> bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw FormatError{"cannot create " + path};
  out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  if (!out) throw FormatError{"cannot write " + path};
}

NavMeshFile readNavMeshFile(const std::string& path) { return parseNavMesh(readFileBytes(path)); }

void writeNavMeshFile(const std::string& path, const NavMeshFile& file) {
  writeFileBytes(path, serializeNavMesh(file));
}

}  // namespace dznav
