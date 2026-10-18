// Client for DayZDiag's navmesh data server (`DayZDiag_x64 -server -startNavmeshDataServer`).
//
// Wire protocol (TCP, little-endian): u32 length (bytes after itself), u32 message id, payload.
// One request may be outstanding per connection; open several connections to fetch tiles in
// parallel.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dznav/WorldInput.h"

namespace dznav {

constexpr uint16_t kDataServerPort = 36533;
constexpr uint32_t kProtocolVersion = 10;

enum MessageId : uint32_t {
  kMsgFinish = 1,
  kMsgRequestWorldParams = 2,
  kMsgRequestTile = 3,
  kMsgWorldParams = 4,
  kMsgTile = 5,
};

struct NetError {
  std::string message;
};

struct Message {
  uint32_t id = 0;
  std::vector<uint8_t> body;  // payload after the id
};

// A blocking, framed TCP connection.
class Connection {
 public:
  static std::unique_ptr<Connection> connect(const std::string& host, uint16_t port, int timeoutMs);
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  void send(uint32_t id, std::span<const uint8_t> payload);
  // Waits up to timeoutMs (0 = forever) for a complete message.
  Message receive(int timeoutMs);

 private:
  friend class Listener;
  explicit Connection(uintptr_t socket) : socket_(socket) {}
  void readExact(uint8_t* dst, size_t n, int timeoutMs);
  uintptr_t socket_;
};

// A listening TCP socket (for the replay server).
class Listener {
 public:
  Listener(const std::string& address, uint16_t port);
  ~Listener();
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;

  // Waits up to timeoutMs (0 = forever) for a client; returns null on timeout.
  std::unique_ptr<Connection> accept(int timeoutMs);

 private:
  uintptr_t socket_;
};

class DataServerClient {
 public:
  DataServerClient(std::string host, uint16_t port = kDataServerPort, int timeoutMs = 600000);

  // Connects (if needed) and fetches world parameters.
  WorldParams requestWorldParams();
  // Fetches one tile's input geometry. Thread-safe only when each thread uses its own client.
  TileInput requestTile(int x, int z, const WorldParams& params);
  // Asks the server to shut down.
  void sendFinish();
  // Drops the connection; the next request reconnects.
  void reset() { conn_.reset(); }

 private:
  Connection& conn();
  std::string host_;
  uint16_t port_;
  int timeoutMs_;
  std::unique_ptr<Connection> conn_;
};

}  // namespace dznav
