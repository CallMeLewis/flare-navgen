#include "dznav/Raster.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace dznav {
namespace {

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
  return ~crc;
}

void putBE32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

void writeChunk(std::ofstream& f, const char* type, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> buf;
  putBE32(buf, uint32_t(data.size()));
  buf.insert(buf.end(), type, type + 4);
  buf.insert(buf.end(), data.begin(), data.end());
  putBE32(buf, crc32(buf.data() + 4, buf.size() - 4));
  f.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(buf.size()));
}

}  // namespace

Raster::Raster(int width, int height, Rgba fill) : w_(width), h_(height), px_(size_t(width) * height * 4) {
  for (size_t i = 0; i < px_.size(); i += 4) {
    px_[i] = fill.r;
    px_[i + 1] = fill.g;
    px_[i + 2] = fill.b;
    px_[i + 3] = fill.a;
  }
}

void Raster::blend(int x, int y, Rgba c) {
  if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
  uint8_t* p = &px_[(size_t(y) * w_ + x) * 4];
  const int a = c.a;
  p[0] = uint8_t((c.r * a + p[0] * (255 - a)) / 255);
  p[1] = uint8_t((c.g * a + p[1] * (255 - a)) / 255);
  p[2] = uint8_t((c.b * a + p[2] * (255 - a)) / 255);
  p[3] = 255;
}

void Raster::fillConvex(const float* xy, int count, Rgba c) {
  float ymin = xy[1], ymax = xy[1];
  for (int i = 1; i < count; ++i) {
    ymin = std::min(ymin, xy[i * 2 + 1]);
    ymax = std::max(ymax, xy[i * 2 + 1]);
  }
  const int y0 = std::max(0, int(std::ceil(ymin - 0.5f)));
  const int y1 = std::min(h_ - 1, int(std::floor(ymax - 0.5f)));
  for (int y = y0; y <= y1; ++y) {
    const float sy = y + 0.5f;
    float xl = 1e30f, xr = -1e30f;
    for (int i = 0, j = count - 1; i < count; j = i++) {
      const float ax = xy[j * 2], ay = xy[j * 2 + 1], bx = xy[i * 2], by = xy[i * 2 + 1];
      if ((ay <= sy && by > sy) || (by <= sy && ay > sy)) {
        const float x = ax + (sy - ay) / (by - ay) * (bx - ax);
        xl = std::min(xl, x);
        xr = std::max(xr, x);
      }
    }
    if (xl > xr) continue;
    const int x0 = std::max(0, int(std::ceil(xl - 0.5f)));
    const int x1 = std::min(w_ - 1, int(std::floor(xr - 0.5f)));
    for (int x = x0; x <= x1; ++x) blend(x, y, c);
  }
}

void Raster::line(float x0, float y0, float x1, float y1, Rgba c) {
  const float dx = x1 - x0, dy = y1 - y0;
  const int steps = std::max(1, int(std::max(std::fabs(dx), std::fabs(dy))));
  for (int i = 0; i <= steps; ++i) {
    const float t = float(i) / steps;
    blend(int(x0 + dx * t), int(y0 + dy * t), c);
  }
}

void Raster::disc(float cx, float cy, float r, Rgba c) {
  const int x0 = int(std::floor(cx - r)), x1 = int(std::ceil(cx + r));
  const int y0 = int(std::floor(cy - r)), y1 = int(std::ceil(cy + r));
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const float ddx = x + 0.5f - cx, ddy = y + 0.5f - cy;
      if (ddx * ddx + ddy * ddy <= r * r) blend(x, y, c);
    }
}

void Raster::writePng(const std::string& path) const {
  std::ofstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot create " + path);
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  f.write(reinterpret_cast<const char*>(sig), 8);

  std::vector<uint8_t> ihdr;
  putBE32(ihdr, uint32_t(w_));
  putBE32(ihdr, uint32_t(h_));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA
  writeChunk(f, "IHDR", ihdr);

  // zlib stream of stored (uncompressed) deflate blocks.
  const size_t rowBytes = size_t(w_) * 4 + 1;
  std::vector<uint8_t> raw;
  raw.reserve(rowBytes * h_);
  for (int y = 0; y < h_; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), px_.begin() + size_t(y) * w_ * 4, px_.begin() + size_t(y + 1) * w_ * 4);
  }
  std::vector<uint8_t> z;
  z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
  z.push_back(0x78);
  z.push_back(0x01);
  uint32_t s1 = 1, s2 = 0;
  for (uint8_t b : raw) {
    s1 = (s1 + b) % 65521;
    s2 = (s2 + s1) % 65521;
  }
  for (size_t pos = 0; pos < raw.size() || pos == 0;) {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    const bool last = pos + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
    if (last) break;
  }
  putBE32(z, (s2 << 16) | s1);
  // Split IDAT so each chunk stays well below the 2^31 limit.
  const size_t chunk = size_t(1) << 26;
  for (size_t pos = 0; pos < z.size(); pos += chunk)
    writeChunk(f, "IDAT", std::vector<uint8_t>(z.begin() + pos, z.begin() + std::min(z.size(), pos + chunk)));
  writeChunk(f, "IEND", {});
}

}  // namespace dznav
