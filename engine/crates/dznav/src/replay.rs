//! Serves captured geometry (.dzgeo) over the data-server protocol, so any client can build from exactly the same
//! input as dznav, to compare their output on identical geometry.

use std::collections::HashMap;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use crate::capture::Capture;
use crate::error::{Error, Result};
use crate::net::{
    Connection, DATA_SERVER_PORT, Listener, MSG_FINISH, MSG_REQUEST_TILE, MSG_REQUEST_WORLD_PARAMS, MSG_TILE,
    MSG_WORLD_PARAMS, PROTOCOL_VERSION,
};
use crate::world::{InputGroup, TileInput, WorldParams, area};

#[derive(Clone, Debug, PartialEq)]
pub struct ReplayOptions {
    pub address: String,
    pub port: u16,
    /// Shrink the world to the captured tiles' bounding grid (from tile 0, 0), so a client that generates every tile
    /// only requests those.
    pub crop: bool,
    /// Overrides the captured tide.
    pub sea_level: Option<f32>,
    /// False clears the world's filter-isolated-islands flag.
    pub island_filter: bool,
    /// Stop when the first client disconnects.
    pub exit_after_client: bool,
}

impl Default for ReplayOptions {
    fn default() -> Self {
        Self {
            address: "127.0.0.1".into(),
            port: DATA_SERVER_PORT,
            crop: false,
            sea_level: None,
            island_filter: true,
            exit_after_client: false,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ReplayStats {
    pub clients: usize,
    pub captured: usize,
    pub empty: usize,
}

/// The world params as served, after the options' changes.
pub fn served_params(capture: &Capture, o: &ReplayOptions) -> WorldParams {
    let mut params = capture.params.clone();
    let b = &mut params.block;
    if o.crop && !capture.tiles.is_empty() {
        b.tiles_x = capture.tiles.iter().map(|t| t.x).max().unwrap_or(0).max(0) + 1;
        b.tiles_z = capture.tiles.iter().map(|t| t.z).max().unwrap_or(0).max(0) + 1;
        b.world_size_x = b.tiles_x as f32 * b.tile_width;
        b.world_size_z = b.tiles_z as f32 * b.tile_height;
    }
    if let Some(sea) = o.sea_level {
        b.deep_sea_height = sea;
    }
    if !o.island_filter {
        b.filter_isolated_islands = 0;
    }
    params
}

/// A tile that wasn't captured: flat seabed 40 m down, as a client may not cope with a tile without terrain. Like real
/// terrain it reaches 24 m past the tile, wound the same way.
fn seabed_tile(x: i32, z: i32, params: &WorldParams) -> TileInput {
    let b = &params.block;
    let (x0, z0) = (x as f32 * b.tile_width, z as f32 * b.tile_height);
    let (x1, z1, y, pad) = (x0 + b.tile_width, z0 + b.tile_height, -40.0f32, 24.0f32);
    #[rustfmt::skip]
    let verts = vec![
        x0 - pad, y, z0 - pad, x1 + pad, y, z0 - pad, x0 - pad, y, z1 + pad,
        x1 + pad, y, z0 - pad, x1 + pad, y, z1 + pad, x0 - pad, y, z1 + pad,
    ];
    TileInput {
        x,
        z,
        cell_class: if params.cell_classes.len() > 1 { 1 } else { 0 },
        bmin: [x0, y - 1.0, z0],
        bmax: [x1, y + 1.0, z1],
        groups: vec![InputGroup { area: area::TERRAIN, parts: Vec::new(), verts }],
    }
}

/// Serves `capture` until `stop` is set (or the first client leaves, with `exit_after_client`). Tiles not in the
/// capture are answered with flat seabed only. `log` gets one line per client event.
pub fn serve_capture(
    capture: &Capture,
    o: &ReplayOptions,
    stop: Option<&AtomicBool>,
    log: &(dyn Fn(&str) + Sync),
) -> Result<ReplayStats> {
    let params = served_params(capture, o);
    let mut world_message = PROTOCOL_VERSION.to_le_bytes().to_vec();
    world_message.extend(params.serialize());
    let tiles: HashMap<(i32, i32), &TileInput> = capture.tiles.iter().map(|t| ((t.x, t.z), t)).collect();

    let stats = Mutex::new(ReplayStats::default());
    let client_left = AtomicBool::new(false);
    let stopped = || stop.is_some_and(|s| s.load(Ordering::Relaxed));
    let finished = || stopped() || (o.exit_after_client && client_left.load(Ordering::Relaxed));

    let listener = Listener::bind(&o.address, o.port)?;
    let b = &params.block;
    log(&format!(
        "listening on {}:{}, {} captured tiles, world {} x {} tiles",
        o.address,
        o.port,
        tiles.len(),
        b.tiles_x,
        b.tiles_z
    ));

    std::thread::scope(|scope| -> Result<()> {
        while !finished() {
            let Some(mut conn) = listener.accept(Duration::from_millis(250))? else { continue };
            stats.lock().unwrap().clients += 1;
            log("client connected");
            let (params, world_message, tiles, stats, client_left, stopped) =
                (&params, &world_message, &tiles, &stats, &client_left, &stopped);
            scope.spawn(move || {
                let (mut captured, mut empty) = (0, 0);
                let served =
                    serve_client(&mut conn, params, world_message, tiles, stopped, log, &mut captured, &mut empty);
                if let Err(Error::Net(m)) = served
                    && !m.starts_with("connection closed")
                {
                    log(&format!("client error: {m}"));
                }
                {
                    let mut s = stats.lock().unwrap();
                    s.captured += captured;
                    s.empty += empty;
                }
                log(&format!("client left after {captured} captured and {empty} empty tiles"));
                client_left.store(true, Ordering::Relaxed);
            });
        }
        Ok(())
    })?;
    Ok(stats.into_inner().unwrap())
}

#[allow(clippy::too_many_arguments)]
fn serve_client(
    conn: &mut Connection,
    params: &WorldParams,
    world_message: &[u8],
    tiles: &HashMap<(i32, i32), &TileInput>,
    stopped: &dyn Fn() -> bool,
    log: &(dyn Fn(&str) + Sync),
    captured: &mut usize,
    empty: &mut usize,
) -> Result<()> {
    while !stopped() {
        let m = match conn.receive(Some(Duration::from_secs(1))) {
            Ok(m) => m,
            Err(Error::Net(m)) if m.starts_with("timed out") => continue,
            Err(e) => return Err(e),
        };
        match m.id {
            MSG_REQUEST_WORLD_PARAMS => conn.send(MSG_WORLD_PARAMS, world_message)?,
            MSG_REQUEST_TILE => {
                let Some(xz) = m.body.get(..8) else { return Err(Error::Net("short tile request".into())) };
                let x = i32::from_le_bytes(xz[..4].try_into().unwrap());
                let z = i32::from_le_bytes(xz[4..].try_into().unwrap());
                let body = match tiles.get(&(x, z)) {
                    Some(t) => {
                        *captured += 1;
                        t.serialize()
                    }
                    None => {
                        *empty += 1;
                        seabed_tile(x, z, params).serialize()
                    }
                };
                conn.send(MSG_TILE, &body)?;
            }
            MSG_FINISH => break,
            id => log(&format!("ignoring message {id}")),
        }
    }
    Ok(())
}
