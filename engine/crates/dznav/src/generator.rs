//! Parallel navmesh generation: fetch tile inputs, build tiles, link them into a navmesh.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use crate::capture::{Capture, CaptureWriter};
use crate::error::{Error, Result};
use crate::native::{BuiltTile, NavMeshAssembly, TileBuildStats};
use crate::net::DataServerClient;
use crate::world::{TileInput, WorldParams};

/// Supplies world parameters and tile inputs. Each fetcher thread has its own source, from `split`.
pub trait TileSource: Send {
    fn world_params(&mut self) -> Result<WorldParams>;
    /// The tile's input, or None if it has none (it is then skipped).
    fn fetch(&mut self, x: i32, z: i32, params: &WorldParams) -> Result<Option<TileInput>>;
    /// A source for another thread (e.g. another connection).
    fn split(&self) -> Result<Box<dyn TileSource>>;
}

struct Servers {
    hosts: Vec<String>,
    port: u16,
    next: AtomicUsize,
}

/// One connection to one data server. `split` moves on to the next server in the list (round robin): DayZDiag serves
/// one client at a time, so parallel fetching needs several servers.
pub struct DataServerSource {
    servers: Arc<Servers>,
    client: DataServerClient,
}

impl DataServerSource {
    pub fn new(hosts: Vec<String>, port: u16) -> Result<Self> {
        if hosts.is_empty() {
            return Err(Error::Net("no data server given".into()));
        }
        let client = DataServerClient::new(hosts[0].clone(), port);
        Ok(Self { servers: Arc::new(Servers { hosts, port, next: AtomicUsize::new(1) }), client })
    }
}

impl TileSource for DataServerSource {
    fn world_params(&mut self) -> Result<WorldParams> {
        self.client.request_world_params()
    }

    fn fetch(&mut self, x: i32, z: i32, params: &WorldParams) -> Result<Option<TileInput>> {
        // DayZDiag serves one client per frame and drops broken connections; retry a few times.
        for attempt in 0.. {
            match self.client.request_tile(x, z, params) {
                Ok(t) => return Ok(Some(t)),
                Err(Error::Net(m)) => {
                    self.client.reset();
                    if attempt >= 4 {
                        return Err(Error::Net(m));
                    }
                    std::thread::sleep(Duration::from_secs(2 * (attempt + 1)));
                }
                Err(e) => return Err(e),
            }
        }
        unreachable!()
    }

    fn split(&self) -> Result<Box<dyn TileSource>> {
        let s = &self.servers;
        let host = s.hosts[s.next.fetch_add(1, Ordering::Relaxed) % s.hosts.len()].clone();
        Ok(Box::new(DataServerSource { servers: s.clone(), client: DataServerClient::new(host, s.port) }))
    }
}

/// Serves tiles from a capture held in memory.
pub struct CaptureSource {
    capture: Arc<Capture>,
    index: Arc<HashMap<(i32, i32), usize>>,
}

impl CaptureSource {
    pub fn new(capture: Arc<Capture>) -> Self {
        // A tile captured twice is served as the later copy.
        let index = capture.tiles.iter().enumerate().map(|(i, t)| ((t.x, t.z), i)).collect();
        Self { capture, index: Arc::new(index) }
    }
}

impl TileSource for CaptureSource {
    fn world_params(&mut self) -> Result<WorldParams> {
        Ok(self.capture.params.clone())
    }

    fn fetch(&mut self, x: i32, z: i32, _: &WorldParams) -> Result<Option<TileInput>> {
        Ok(self.index.get(&(x, z)).map(|&i| self.capture.tiles[i].clone()))
    }

    fn split(&self) -> Result<Box<dyn TileSource>> {
        Ok(Box::new(CaptureSource { capture: self.capture.clone(), index: self.index.clone() }))
    }
}

/// Builds one tile: the blob (None when the tile has no walkable surface) and its stats.
pub type TileBuildFn<'a> = dyn Fn(&WorldParams, &TileInput) -> Result<BuiltTile> + Sync + 'a;

pub struct GenerateOptions<'a> {
    /// Inclusive tile range `[x0, z0, x1, z1]`, clamped to the world; a negative x1 or z1 means to the world's edge.
    /// None means the whole world.
    pub range: Option<[i32; 4]>,
    /// Builder threads; 0 means hardware threads - 1.
    pub builders: usize,
    /// Parallel source connections.
    pub fetchers: usize,
    /// Bounds the memory held by fetched-but-unbuilt tiles.
    pub max_queued_inputs: usize,
    pub cancel: Option<&'a AtomicBool>,
    /// If set, every fetched tile input is recorded here.
    pub capture_path: Option<PathBuf>,
    /// Tiles for which this returns false are skipped (used to resume).
    pub include: Option<&'a (dyn Fn(i32, i32) -> bool + Sync)>,
    /// Called from the fetcher threads when a tile's input has arrived and is queued for building.
    pub on_fetched: Option<&'a (dyn Fn(i32, i32) + Sync)>,
}

impl Default for GenerateOptions<'_> {
    fn default() -> Self {
        Self {
            range: None,
            builders: 0,
            fetchers: 4,
            max_queued_inputs: 64,
            cancel: None,
            capture_path: None,
            include: None,
            on_fetched: None,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TileOutcome {
    Built,
    Empty,
    Failed,
}

pub struct GenerateProgress<'a> {
    pub total: usize,
    pub fetched: usize,
    /// Tiles finished without failing, empty ones included.
    pub built: usize,
    pub failed: usize,
    pub elapsed: Duration,
    /// The tile that just finished.
    pub last: &'a TileBuildStats,
    pub last_outcome: TileOutcome,
    /// Set when `last_outcome` is Failed.
    pub last_error: Option<&'a str>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct GenerateResult {
    pub built: usize,
    pub empty: usize,
    pub failed: usize,
    pub errors: Vec<String>,
}

/// Tile order: square blocks of `block` tiles, row-major inside a block, which keeps the data server's streaming
/// cache warm.
fn tile_order(x0: i32, z0: i32, x1: i32, z1: i32, block: i32) -> Vec<(i32, i32)> {
    let block = block.max(1);
    let mut order = Vec::new();
    for bz in (z0..=z1).step_by(block as usize) {
        for bx in (x0..=x1).step_by(block as usize) {
            for z in bz..=z1.min(bz + block - 1) {
                for x in bx..=x1.min(bx + block - 1) {
                    order.push((x, z));
                }
            }
        }
    }
    order
}

/// The tiles `generate` will process, in processing order (range clamped to the world, include filter applied).
pub fn planned_tiles(params: &WorldParams, options: &GenerateOptions) -> Vec<(i32, i32)> {
    let b = &params.block;
    let [x0, z0, x1, z1] = options.range.unwrap_or([0, 0, -1, -1]);
    let x1 = if x1 < 0 { b.tiles_x - 1 } else { x1.min(b.tiles_x - 1) };
    let z1 = if z1 < 0 { b.tiles_z - 1 } else { z1.min(b.tiles_z - 1) };
    let mut order = tile_order(x0.max(0), z0.max(0), x1, z1, b.block_size);
    if let Some(include) = options.include {
        order.retain(|&(x, z)| include(x, z));
    }
    order
}

struct Built {
    stats: TileBuildStats,
    result: Result<Option<Vec<u8>>, String>,
}

/// Fetches, builds and links the planned tiles into `mesh`. Detour is not thread-safe, so tiles are linked on this
/// thread, which also calls `on_progress` after each tile, with the mesh so far (to save checkpoints).
pub fn generate(
    source: &mut dyn TileSource,
    params: &WorldParams,
    mesh: &mut NavMeshAssembly,
    build: &TileBuildFn,
    options: &GenerateOptions,
    on_progress: &mut dyn FnMut(&GenerateProgress, &NavMeshAssembly),
) -> Result<GenerateResult> {
    let order = planned_tiles(params, options);
    let builders = match options.builders {
        0 => std::thread::available_parallelism().map_or(1, |n| n.get().saturating_sub(1).max(1)),
        n => n,
    };
    let fetchers = options.fetchers.max(1);
    let capture = match &options.capture_path {
        Some(path) => Some(Mutex::new(Some(CaptureWriter::create(path, params)?))),
        None => None,
    };
    let cancelled = || options.cancel.is_some_and(|c| c.load(Ordering::Relaxed));
    let errors = Mutex::new(Vec::new());
    let fail = |message: String| errors.lock().unwrap().push(message);
    let next_tile = AtomicUsize::new(0);
    let fetched = AtomicUsize::new(0);
    let mut result = GenerateResult::default();

    // Sources for the other fetchers.
    let mut others = Vec::new();
    for _ in 1..fetchers {
        match source.split() {
            Ok(s) => others.push(Some(s)),
            Err(e) => {
                fail(format!("source: {e}"));
                others.push(None);
            }
        }
    }

    std::thread::scope(|scope| {
        let (input_tx, input_rx) = crossbeam_channel::bounded::<TileInput>(options.max_queued_inputs.max(1));
        let (result_tx, result_rx) = crossbeam_channel::bounded::<Built>(builders * 4);

        let mut sources: Vec<Option<&mut dyn TileSource>> = vec![Some(source)];
        sources.extend(others.iter_mut().map(|s| s.as_deref_mut().map(|s| s as &mut dyn TileSource)));
        for src in sources {
            let tx = input_tx.clone();
            let (order, next_tile, fetched, capture, fail, cancelled) =
                (&order, &next_tile, &fetched, &capture, &fail, &cancelled);
            scope.spawn(move || {
                let Some(src) = src else { return };
                while !cancelled() {
                    let i = next_tile.fetch_add(1, Ordering::Relaxed);
                    let Some(&(x, z)) = order.get(i) else { break };
                    let input = match src.fetch(x, z, params) {
                        Ok(Some(input)) => input,
                        Ok(None) => continue,
                        Err(e) => {
                            fail(format!("tile ({x},{z}): {e}"));
                            // A network failure ends this fetcher; bad data only loses the tile.
                            if matches!(e, Error::Net(_)) {
                                break;
                            }
                            continue;
                        }
                    };
                    fetched.fetch_add(1, Ordering::Relaxed);
                    if let Some(on_fetched) = options.on_fetched {
                        on_fetched(input.x, input.z);
                    }
                    if let Some(capture) = capture {
                        let mut writer = capture.lock().unwrap();
                        if let Some(Err(e)) = writer.as_mut().map(|w| w.add(&input)) {
                            fail(format!("capture: {e}"));
                            *writer = None;
                        }
                    }
                    if tx.send(input).is_err() {
                        break;
                    }
                }
            });
        }
        drop(input_tx);

        for _ in 0..builders {
            let (rx, tx) = (input_rx.clone(), result_tx.clone());
            scope.spawn(move || {
                for input in rx {
                    if cancelled() {
                        continue;
                    }
                    let t0 = Instant::now();
                    let built = build(params, &input);
                    let build_ms = t0.elapsed().as_secs_f64() * 1000.0;
                    let out = match built {
                        Ok(b) => Built { stats: TileBuildStats { build_ms, ..b.stats }, result: Ok(b.data) },
                        Err(e) => Built {
                            stats: TileBuildStats { x: input.x, z: input.z, build_ms, ..Default::default() },
                            result: Err(e.to_string()),
                        },
                    };
                    if tx.send(out).is_err() {
                        break;
                    }
                }
            });
        }
        drop(input_rx);
        drop(result_tx);

        let start = Instant::now();
        for built in result_rx {
            let Built { stats, result: outcome } = built;
            let outcome = match outcome {
                Ok(None) => {
                    mesh.remove_tile(stats.x, stats.z);
                    Ok(TileOutcome::Empty)
                }
                Ok(Some(data)) => mesh.set_tile(&data).map(|_| TileOutcome::Built).map_err(|e| e.to_string()),
                Err(e) => Err(e),
            };
            let (last_outcome, last_error) = match &outcome {
                Ok(o) => (*o, None),
                Err(e) => (TileOutcome::Failed, Some(e.as_str())),
            };
            match last_outcome {
                TileOutcome::Built => result.built += 1,
                TileOutcome::Empty => result.empty += 1,
                TileOutcome::Failed => {
                    result.failed += 1;
                    fail(format!("tile ({},{}): {}", stats.x, stats.z, last_error.unwrap_or_default()));
                }
            }
            let progress = GenerateProgress {
                total: order.len(),
                fetched: fetched.load(Ordering::Relaxed),
                built: result.built + result.empty,
                failed: result.failed,
                elapsed: start.elapsed(),
                last: &stats,
                last_outcome,
                last_error,
            };
            on_progress(&progress, mesh);
        }
    });
    result.errors = errors.into_inner().unwrap();
    Ok(result)
}
