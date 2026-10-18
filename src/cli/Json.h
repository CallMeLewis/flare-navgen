// JSON output for --json mode: every result or event is one JSON object on its own line of stdout.
#pragma once

#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

namespace dznav::cli {

// Builds one JSON value. Objects and arrays are written incrementally:
//   Json().obj().field("event", "tile").field("x", 3).end().str()
class Json {
 public:
  Json& obj() { return open('{'); }
  Json& arr() { return open('['); }
  Json& end() {
    const char c = stack_.back() == '{' ? '}' : ']';
    stack_.pop_back();
    if (s_.back() == ',')
      s_.back() = c;  // drop the trailing separator
    else
      s_ += c;
    return next();
  }

  // Object member; open a nested object or array with key(k).obj() / key(k).arr().
  Json& key(const char* k) {
    quote(k);
    s_ += ':';
    return *this;
  }
  template <typename T>
  Json& field(const char* k, const T& v) {
    return key(k).value(v);
  }

  Json& value(const std::string& v) { return quote(v.c_str()), next(); }
  Json& value(const char* v) { return quote(v), next(); }
  Json& value(bool v) { return s_ += v ? "true" : "false", next(); }
  Json& value(double v) {
    if (!std::isfinite(v)) return s_ += "null", next();
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return s_ += buf, next();
  }
  Json& value(float v) { return value(double(v)); }
  template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
  Json& value(T v) {
    return s_ += std::to_string(v), next();
  }
  template <typename T>
  Json& value(const std::vector<T>& v) {
    arr();
    for (const T& e : v) value(e);
    return end();
  }
  // Inserts already-encoded JSON.
  Json& raw(const std::string& json) { return s_ += json, next(); }

  const std::string& str() const { return s_; }

 private:
  Json& open(char c) {
    s_ += c;
    stack_.push_back(c);
    return *this;
  }
  Json& next() {
    // After a complete value inside a container, add the separator for the next one.
    if (!stack_.empty()) s_ += ',';
    return *this;
  }
  void quote(const char* v) {
    s_ += '"';
    for (const char* p = v; *p; ++p) {
      const unsigned char c = static_cast<unsigned char>(*p);
      if (c == '"' || c == '\\') {
        s_ += '\\';
        s_ += char(c);
      } else if (c < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "\\u%04x", c);
        s_ += buf;
      } else {
        s_ += char(c);
      }
    }
    s_ += '"';
  }

  std::string s_;
  std::vector<char> stack_;
};

// Writes one line to stdout and flushes it. Safe to call from several threads.
inline void emitLine(const std::string& line) {
  static std::mutex m;
  std::lock_guard lock(m);
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

}  // namespace dznav::cli
