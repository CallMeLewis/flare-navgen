#include "dznav/DataServerClient.h"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketLen = int;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#define WSAPoll poll
using SOCKET = int;
#endif

namespace dznav {
namespace {

constexpr uint32_t kMaxMessage = 1u << 30;  // 1 GiB; tile messages are far smaller

void ensureSocketsStarted() {
#ifdef _WIN32
  static const bool started = [] {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
  }();
  if (!started) throw NetError{"WSAStartup failed"};
#endif
}

int lastError() {
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

// Waits until the socket is readable/writable. Returns false on timeout.
bool waitFor(SOCKET s, bool write, int timeoutMs) {
  pollfd p{};
  p.fd = s;
  p.events = write ? POLLOUT : POLLIN;
  const int r = WSAPoll(&p, 1, timeoutMs > 0 ? timeoutMs : -1);
  if (r < 0) throw NetError{"poll failed: " + std::to_string(lastError())};
  return r > 0;
}

}  // namespace

std::unique_ptr<Connection> Connection::connect(const std::string& host, uint16_t port, int timeoutMs) {
  ensureSocketsStarted();
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
    throw NetError{"cannot resolve " + host};

  SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (s == INVALID_SOCKET) {
    freeaddrinfo(res);
    throw NetError{"socket() failed"};
  }
#ifdef _WIN32
  u_long nonBlocking = 1;
  ioctlsocket(s, FIONBIO, &nonBlocking);
#endif
  int rc = ::connect(s, res->ai_addr, SocketLen(res->ai_addrlen));
  freeaddrinfo(res);
  if (rc == SOCKET_ERROR) {
#ifdef _WIN32
    const bool pending = lastError() == WSAEWOULDBLOCK;
#else
    const bool pending = errno == EINPROGRESS;
#endif
    if (!pending || !waitFor(s, true, timeoutMs)) {
      closesocket(s);
      throw NetError{"cannot connect to " + host + ":" + std::to_string(port)};
    }
    int err = 0;
    SocketLen len = sizeof(err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
    if (err != 0) {
      closesocket(s);
      throw NetError{"cannot connect to " + host + ":" + std::to_string(port)};
    }
  }
#ifdef _WIN32
  nonBlocking = 0;
  ioctlsocket(s, FIONBIO, &nonBlocking);
#endif
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
  return std::unique_ptr<Connection>(new Connection(uintptr_t(s)));
}

Connection::~Connection() { closesocket(SOCKET(socket_)); }

Listener::Listener(const std::string& address, uint16_t port) {
  ensureSocketsStarted();
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) throw NetError{"socket() failed"};
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
    closesocket(s);
    throw NetError{"not an IPv4 address: " + address};
  }
  if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR || listen(s, 8) == SOCKET_ERROR) {
    const int err = lastError();
    closesocket(s);
    throw NetError{"cannot listen on " + address + ":" + std::to_string(port) + " (error " + std::to_string(err) + ")"};
  }
  socket_ = uintptr_t(s);
}

Listener::~Listener() { closesocket(SOCKET(socket_)); }

std::unique_ptr<Connection> Listener::accept(int timeoutMs) {
  if (!waitFor(SOCKET(socket_), false, timeoutMs)) return nullptr;
  SOCKET c = ::accept(SOCKET(socket_), nullptr, nullptr);
  if (c == INVALID_SOCKET) throw NetError{"accept failed: " + std::to_string(lastError())};
  int one = 1;
  setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
  return std::unique_ptr<Connection>(new Connection(uintptr_t(c)));
}

void Connection::send(uint32_t id, std::span<const uint8_t> payload) {
  std::vector<uint8_t> frame(8 + payload.size());
  const uint32_t len = uint32_t(4 + payload.size());
  std::memcpy(frame.data(), &len, 4);
  std::memcpy(frame.data() + 4, &id, 4);
  if (!payload.empty()) std::memcpy(frame.data() + 8, payload.data(), payload.size());
  size_t sent = 0;
  while (sent < frame.size()) {
    const int n = ::send(SOCKET(socket_), reinterpret_cast<const char*>(frame.data() + sent),
                         int(std::min<size_t>(frame.size() - sent, 1 << 20)), 0);
    if (n <= 0) throw NetError{"send failed: " + std::to_string(lastError())};
    sent += size_t(n);
  }
}

void Connection::readExact(uint8_t* dst, size_t n, int timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  size_t got = 0;
  while (got < n) {
    int wait = 0;
    if (timeoutMs > 0) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0) throw NetError{"timed out waiting for data"};
      wait = int(left.count());
    }
    if (!waitFor(SOCKET(socket_), false, wait)) throw NetError{"timed out waiting for data"};
    const int r = recv(SOCKET(socket_), reinterpret_cast<char*>(dst + got), int(std::min<size_t>(n - got, 1 << 20)), 0);
    if (r == 0) throw NetError{"connection closed by peer"};
    if (r < 0) throw NetError{"recv failed: " + std::to_string(lastError())};
    got += size_t(r);
  }
}

Message Connection::receive(int timeoutMs) {
  uint32_t len = 0;
  readExact(reinterpret_cast<uint8_t*>(&len), 4, timeoutMs);
  if (len < 4 || len > kMaxMessage) throw NetError{"invalid frame length " + std::to_string(len)};
  Message m;
  readExact(reinterpret_cast<uint8_t*>(&m.id), 4, timeoutMs);
  m.body.resize(len - 4);
  if (!m.body.empty()) readExact(m.body.data(), m.body.size(), timeoutMs);
  return m;
}

DataServerClient::DataServerClient(std::string host, uint16_t port, int timeoutMs)
    : host_(std::move(host)), port_(port), timeoutMs_(timeoutMs) {}

Connection& DataServerClient::conn() {
  if (!conn_) conn_ = Connection::connect(host_, port_, 10000);
  return *conn_;
}

WorldParams DataServerClient::requestWorldParams() {
  conn().send(kMsgRequestWorldParams, {});
  Message m = conn().receive(timeoutMs_);
  if (m.id != kMsgWorldParams) throw NetError{"expected world params, got message " + std::to_string(m.id)};
  if (m.body.size() < 4) throw NetError{"world params truncated"};
  uint32_t version;
  std::memcpy(&version, m.body.data(), 4);
  if (version != kProtocolVersion)
    throw NetError{"server protocol version " + std::to_string(version) + ", expected " +
                   std::to_string(kProtocolVersion)};
  return parseWorldParams(std::span<const uint8_t>(m.body).subspan(4));
}

TileInput DataServerClient::requestTile(int x, int z, const WorldParams& params) {
  struct {
    int32_t x, z;
    float minX, minZ, maxX, maxZ;
  } req{x, z, x * params.block.tileWidth, z * params.block.tileHeight, (x + 1) * params.block.tileWidth,
        (z + 1) * params.block.tileHeight};
  conn().send(kMsgRequestTile, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&req), sizeof(req)));
  Message m = conn().receive(timeoutMs_);
  if (m.id != kMsgTile) throw NetError{"expected tile, got message " + std::to_string(m.id)};
  TileInput t = parseTileInput(m.body);
  if (t.x != x || t.z != z)
    throw NetError{"server answered tile (" + std::to_string(t.x) + "," + std::to_string(t.z) + ") for (" +
                   std::to_string(x) + "," + std::to_string(z) + ")"};
  if (t.cellClass >= params.cellClasses.size()) throw NetError{"tile has unknown cell class"};
  return t;
}

void DataServerClient::sendFinish() { conn().send(kMsgFinish, {}); }

}  // namespace dznav
