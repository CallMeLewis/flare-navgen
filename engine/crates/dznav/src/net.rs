//! Client for DayZDiag's navmesh data server (`DayZDiag_x64 -server -startNavmeshDataServer`), and the framed
//! connections the replay server uses too.
//!
//! Wire protocol (TCP, little-endian): u32 length (bytes after itself), u32 message id, payload. One request may be
//! outstanding per connection; open several connections to fetch tiles in parallel.

use std::io::{ErrorKind, Read, Write};
use std::net::{Ipv4Addr, SocketAddr, TcpListener, TcpStream, ToSocketAddrs};
use std::time::{Duration, Instant};

use crate::error::{Error, Result, net_err};
use crate::world::{TileInput, WorldParams};

pub const DATA_SERVER_PORT: u16 = 36533;
pub const PROTOCOL_VERSION: u32 = 10;

pub const MSG_FINISH: u32 = 1;
pub const MSG_REQUEST_WORLD_PARAMS: u32 = 2;
pub const MSG_REQUEST_TILE: u32 = 3;
pub const MSG_WORLD_PARAMS: u32 = 4;
pub const MSG_TILE: u32 = 5;

/// 1 GiB; tile messages are far smaller.
const MAX_MESSAGE: u32 = 1 << 30;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Message {
    pub id: u32,
    /// The payload after the id.
    pub body: Vec<u8>,
}

/// A blocking, framed TCP connection.
pub struct Connection {
    stream: TcpStream,
}

impl Connection {
    /// Connects to an IPv4 address of `host`, waiting up to `timeout`.
    pub fn connect(host: &str, port: u16, timeout: Duration) -> Result<Connection> {
        let addr = (host, port)
            .to_socket_addrs()
            .ok()
            .and_then(|mut addrs| addrs.find(SocketAddr::is_ipv4))
            .ok_or_else(|| Error::Net(format!("cannot resolve {host}")))?;
        let stream = TcpStream::connect_timeout(&addr, timeout)
            .map_err(|_| Error::Net(format!("cannot connect to {host}:{port}")))?;
        Ok(Self::from_stream(stream))
    }

    fn from_stream(stream: TcpStream) -> Connection {
        let _ = stream.set_nodelay(true);
        Connection { stream }
    }

    pub fn send(&mut self, id: u32, payload: &[u8]) -> Result<()> {
        let mut frame = Vec::with_capacity(8 + payload.len());
        frame.extend_from_slice(&(4 + payload.len() as u32).to_le_bytes());
        frame.extend_from_slice(&id.to_le_bytes());
        frame.extend_from_slice(payload);
        self.stream.write_all(&frame).map_err(|e| Error::Net(format!("send failed: {e}")))
    }

    /// Waits up to `timeout` (None = forever) for a complete message.
    pub fn receive(&mut self, timeout: Option<Duration>) -> Result<Message> {
        let deadline = timeout.map(|t| Instant::now() + t);
        let mut word = [0u8; 4];
        self.read_exact(&mut word, deadline)?;
        let len = u32::from_le_bytes(word);
        if !(4..=MAX_MESSAGE).contains(&len) {
            return net_err(format!("invalid frame length {len}"));
        }
        self.read_exact(&mut word, deadline)?;
        let mut body = vec![0; len as usize - 4];
        self.read_exact(&mut body, deadline)?;
        Ok(Message { id: u32::from_le_bytes(word), body })
    }

    fn read_exact(&mut self, mut dst: &mut [u8], deadline: Option<Instant>) -> Result<()> {
        let timed_out = || Error::Net("timed out waiting for data".to_string());
        while !dst.is_empty() {
            let wait = match deadline {
                Some(d) => {
                    Some(d.checked_duration_since(Instant::now()).filter(|w| !w.is_zero()).ok_or_else(timed_out)?)
                }
                None => None,
            };
            self.stream.set_read_timeout(wait).map_err(|e| Error::Net(format!("recv failed: {e}")))?;
            match self.stream.read(dst) {
                Ok(0) => return net_err("connection closed by peer"),
                Ok(n) => dst = &mut dst[n..],
                Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => return Err(timed_out()),
                Err(e) if e.kind() == ErrorKind::Interrupted => {}
                Err(e) => return net_err(format!("recv failed: {e}")),
            }
        }
        Ok(())
    }
}

/// A listening TCP socket (for the replay server).
pub struct Listener {
    listener: TcpListener,
}

impl Listener {
    pub fn bind(address: &str, port: u16) -> Result<Listener> {
        let ip: Ipv4Addr = address.parse().map_err(|_| Error::Net(format!("not an IPv4 address: {address}")))?;
        let listen_err = |e: std::io::Error| Error::Net(format!("cannot listen on {address}:{port} ({e})"));
        let listener = TcpListener::bind((ip, port)).map_err(listen_err)?;
        listener.set_nonblocking(true).map_err(listen_err)?;
        Ok(Listener { listener })
    }

    /// The port it listens on (useful after binding port 0).
    pub fn port(&self) -> u16 {
        self.listener.local_addr().map_or(0, |a| a.port())
    }

    /// Waits up to `timeout` for a client; None on timeout.
    pub fn accept(&self, timeout: Duration) -> Result<Option<Connection>> {
        let deadline = Instant::now() + timeout;
        loop {
            match self.listener.accept() {
                Ok((stream, _)) => {
                    // Accepted sockets inherit non-blocking mode on some platforms.
                    stream.set_nonblocking(false).map_err(|e| Error::Net(format!("accept failed: {e}")))?;
                    return Ok(Some(Connection::from_stream(stream)));
                }
                Err(e) if e.kind() == ErrorKind::WouldBlock => {
                    if Instant::now() >= deadline {
                        return Ok(None);
                    }
                    std::thread::sleep(Duration::from_millis(10));
                }
                Err(e) => return net_err(format!("accept failed: {e}")),
            }
        }
    }
}

/// One client of a data server. It connects on the first request, and again after `reset`.
pub struct DataServerClient {
    host: String,
    port: u16,
    timeout: Duration,
    conn: Option<Connection>,
}

impl DataServerClient {
    /// A client that waits up to 10 minutes for each answer.
    pub fn new(host: impl Into<String>, port: u16) -> Self {
        Self { host: host.into(), port, timeout: Duration::from_secs(600), conn: None }
    }

    fn conn(&mut self) -> Result<&mut Connection> {
        if self.conn.is_none() {
            self.conn = Some(Connection::connect(&self.host, self.port, Duration::from_secs(10))?);
        }
        Ok(self.conn.as_mut().unwrap())
    }

    /// Connects (if needed) and fetches world parameters.
    pub fn request_world_params(&mut self) -> Result<WorldParams> {
        let timeout = self.timeout;
        let conn = self.conn()?;
        conn.send(MSG_REQUEST_WORLD_PARAMS, &[])?;
        let m = conn.receive(Some(timeout))?;
        if m.id != MSG_WORLD_PARAMS {
            return net_err(format!("expected world params, got message {}", m.id));
        }
        let Some(version) = m.body.get(..4) else { return net_err("world params truncated") };
        let version = u32::from_le_bytes(version.try_into().unwrap());
        if version != PROTOCOL_VERSION {
            return net_err(format!("server protocol version {version}, expected {PROTOCOL_VERSION}"));
        }
        WorldParams::parse(&m.body[4..])
    }

    /// Fetches one tile's input geometry.
    pub fn request_tile(&mut self, x: i32, z: i32, params: &WorldParams) -> Result<TileInput> {
        let b = &params.block;
        let mut req = Vec::with_capacity(24);
        req.extend_from_slice(&x.to_le_bytes());
        req.extend_from_slice(&z.to_le_bytes());
        let bounds = [
            x as f32 * b.tile_width,
            z as f32 * b.tile_height,
            (x + 1) as f32 * b.tile_width,
            (z + 1) as f32 * b.tile_height,
        ];
        for v in bounds {
            req.extend_from_slice(&v.to_le_bytes());
        }
        let timeout = self.timeout;
        let conn = self.conn()?;
        conn.send(MSG_REQUEST_TILE, &req)?;
        let m = conn.receive(Some(timeout))?;
        if m.id != MSG_TILE {
            return net_err(format!("expected tile, got message {}", m.id));
        }
        let t = TileInput::parse(&m.body)?;
        if (t.x, t.z) != (x, z) {
            return net_err(format!("server answered tile ({},{}) for ({x},{z})", t.x, t.z));
        }
        if t.cell_class as usize >= params.cell_classes.len() {
            return net_err("tile has unknown cell class");
        }
        Ok(t)
    }

    /// Asks the server to shut down.
    pub fn send_finish(&mut self) -> Result<()> {
        self.conn()?.send(MSG_FINISH, &[])
    }

    /// Drops the connection; the next request reconnects.
    pub fn reset(&mut self) {
        self.conn = None;
    }
}
