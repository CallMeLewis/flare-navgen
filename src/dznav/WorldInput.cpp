#include "dznav/WorldInput.h"

#include <cmath>
#include <cstring>

namespace dznav {
namespace {

// Upper bounds that reject corrupt or hostile data instead of allocating gigabytes.
constexpr uint32_t kMaxCellClasses = 64;
constexpr uint32_t kMaxLinks = 256;
constexpr uint32_t kMaxGroups = 1 << 16;
constexpr uint32_t kMaxParts = 1 << 20;
constexpr uint32_t kMaxVerts = 1 << 26;

class In {
 public:
  explicit In(std::span<const uint8_t> d) : d_(d) {}
  template <typename T>
  T get() {
    T v;
    need(sizeof(T));
    std::memcpy(&v, d_.data() + p_, sizeof(T));
    p_ += sizeof(T);
    return v;
  }
  void raw(void* dst, size_t n) {
    need(n);
    std::memcpy(dst, d_.data() + p_, n);
    p_ += n;
  }
  uint32_t count(uint32_t max, const char* what) {
    const auto n = get<uint32_t>();
    if (n > max) throw WireError{std::string("implausible ") + what + " count " + std::to_string(n)};
    return n;
  }
  size_t pos() const { return p_; }

 private:
  void need(size_t n) const {
    if (p_ + n > d_.size()) throw WireError{"message truncated"};
  }
  std::span<const uint8_t> d_;
  size_t p_ = 0;
};

class Out {
 public:
  template <typename T>
  void put(const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
  }
  void raw(const void* src, size_t n) {
    const auto* p = static_cast<const uint8_t*>(src);
    b.insert(b.end(), p, p + n);
  }
  std::vector<uint8_t> b;
};

}  // namespace

std::pair<uint32_t, uint32_t> InputGroup::partRange(size_t i) const {
  const uint32_t first = parts[i].firstVertex;
  const uint32_t end = i + 1 < parts.size() ? parts[i + 1].firstVertex : uint32_t(vertexCount());
  return {first, end};
}

WorldParams parseWorldParams(std::span<const uint8_t> body) {
  In in(body);
  WorldParams p;
  in.raw(&p.block, sizeof(p.block));
  p.cellClasses.resize(in.count(kMaxCellClasses, "cell class"));
  if (!p.cellClasses.empty()) in.raw(p.cellClasses.data(), p.cellClasses.size() * sizeof(CellClass));
  p.links.resize(in.count(kMaxLinks, "link"));
  if (!p.links.empty()) in.raw(p.links.data(), p.links.size() * sizeof(LinkClass));
  if (in.pos() != body.size()) throw WireError{"unexpected bytes after world params"};

  const WorldParamsBlock& b = p.block;
  if (b.tilesX <= 0 || b.tilesZ <= 0 || int64_t(b.tilesX) * b.tilesZ > (1 << 28))
    throw WireError{"invalid tile grid"};
  if (!(b.tileWidth > 0) || !(b.tileHeight > 0)) throw WireError{"invalid tile size"};
  for (const CellClass& c : p.cellClasses)
    if (c.cells <= 0 || !(c.tileWidth > 0)) throw WireError{"invalid cell class"};
  return p;
}

std::vector<uint8_t> serializeWorldParams(const WorldParams& p) {
  Out o;
  o.put(p.block);
  o.put(uint32_t(p.cellClasses.size()));
  o.raw(p.cellClasses.data(), p.cellClasses.size() * sizeof(CellClass));
  o.put(uint32_t(p.links.size()));
  o.raw(p.links.data(), p.links.size() * sizeof(LinkClass));
  return std::move(o.b);
}

TileInput parseTileInput(std::span<const uint8_t> body, size_t* consumed) {
  In in(body);
  TileInput t;
  t.x = in.get<int32_t>();
  t.z = in.get<int32_t>();
  t.cellClass = in.get<uint32_t>();
  const uint32_t groups = in.count(kMaxGroups, "group");
  in.raw(t.bmin, sizeof(t.bmin));
  in.raw(t.bmax, sizeof(t.bmax));
  t.groups.resize(groups);
  for (InputGroup& g : t.groups) {
    g.area = in.get<uint32_t>();
    g.parts.resize(in.count(kMaxParts, "part"));
    for (InputPart& part : g.parts) {
      part.partIndex = in.get<uint32_t>();
      part.objectId = in.get<uint64_t>();
      part.firstVertex = in.get<uint32_t>();
    }
    const auto nVerts = in.get<int32_t>();
    if (nVerts < 0 || uint32_t(nVerts) > kMaxVerts) throw WireError{"implausible vertex count"};
    g.verts.resize(size_t(nVerts) * 3);
    if (nVerts) in.raw(g.verts.data(), g.verts.size() * sizeof(float));
    for (size_t i = 0; i < g.parts.size(); ++i) {
      const auto [first, end] = g.partRange(i);
      if (first > end || end > uint32_t(nVerts)) throw WireError{"part vertex range out of bounds"};
    }
    for (float v : g.verts)
      if (!std::isfinite(v)) throw WireError{"non-finite vertex"};
  }
  if (consumed)
    *consumed = in.pos();
  else if (in.pos() != body.size())
    throw WireError{"unexpected bytes after tile record"};
  return t;
}

std::vector<uint8_t> serializeTileInput(const TileInput& t) {
  Out o;
  o.put(t.x);
  o.put(t.z);
  o.put(t.cellClass);
  o.put(uint32_t(t.groups.size()));
  o.raw(t.bmin, sizeof(t.bmin));
  o.raw(t.bmax, sizeof(t.bmax));
  for (const InputGroup& g : t.groups) {
    o.put(g.area);
    o.put(uint32_t(g.parts.size()));
    for (const InputPart& part : g.parts) {
      o.put(part.partIndex);
      o.put(part.objectId);
      o.put(part.firstVertex);
    }
    o.put(int32_t(g.vertexCount()));
    o.raw(g.verts.data(), g.verts.size() * sizeof(float));
  }
  return std::move(o.b);
}

}  // namespace dznav
