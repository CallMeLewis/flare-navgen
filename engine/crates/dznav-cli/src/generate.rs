//! `generate`: build a navmesh from a data server or a capture.

use std::collections::HashSet;
use std::io::Write as _;
use std::process::ExitCode;
use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

use dznav::generator::{
    CaptureSource, DataServerSource, GenerateOptions, GenerateProgress, TileOutcome, TileSource, generate as run,
    planned_tiles,
};
use dznav::net::DATA_SERVER_PORT;
use dznav::{Capture, DoorStatePolicy, Error, NavMeshAssembly, NavMeshFile, TileBuildOptions, build_tile};
use serde_json::json;

use crate::{CmdResult, atof, atoi, bad_args, cancel_flag, emit, json, stop_on_ctrl_c, stop_on_stdin};

struct Args {
    out: String,
    server: String,
    capture: Option<String>,
    base: Option<String>,
    record: Option<String>,
    builders: usize,
    connections: Option<usize>,
    build: TileBuildOptions,
    range: Option<[i32; 4]>,
    island_filter: bool,
    seed: Option<[f32; 3]>,
    skip_existing: bool,
    checkpoint_every: usize,
}

fn parse_args(args: &[&str]) -> Result<Args, crate::Fail> {
    let mut a = Args {
        out: args[0].to_string(),
        server: "127.0.0.1".into(),
        capture: None,
        base: None,
        record: None,
        builders: 0,
        connections: None,
        build: TileBuildOptions::default(),
        range: None,
        island_filter: true,
        seed: None,
        skip_existing: false,
        checkpoint_every: 0,
    };
    let mut it = args[1..].iter();
    while let Some(&opt) = it.next() {
        let mut next = || it.next().copied().ok_or_else(|| bad_args(format!("missing value for {opt}")));
        match opt {
            "--server" => a.server = next()?.into(),
            "--capture" => a.capture = Some(next()?.into()),
            "--base" => a.base = Some(next()?.into()),
            "--record" => a.record = Some(next()?.into()),
            "--threads" => a.builders = atoi(next()?).max(0) as usize,
            "--connections" => a.connections = Some(atoi(next()?).max(0) as usize),
            "--sea-level" => a.build.sea_level = Some(atof(next()?) as f32),
            "--no-links" => a.build.jump_links = false,
            "--no-island-filter" => a.island_filter = false,
            "--skip-existing" => a.skip_existing = true,
            "--checkpoint" => a.checkpoint_every = atoi(next()?).max(0) as usize,
            "--seed" => a.seed = Some([atof(next()?) as f32, atof(next()?) as f32, atof(next()?) as f32]),
            "--doors" => {
                a.build.doors = match next()? {
                    "open" => DoorStatePolicy::Open,
                    "enabled" => DoorStatePolicy::Enabled,
                    _ => DoorStatePolicy::Closed,
                }
            }
            "--range" => a.range = Some([atoi(next()?), atoi(next()?), atoi(next()?), atoi(next()?)]),
            _ => return Err(bad_args(format!("unknown option {opt}"))),
        }
    }
    Ok(a)
}

/// Writes to a temporary file first, so an interrupted save never destroys the previous one.
fn save(mesh: &NavMeshAssembly, path: &str) -> Result<(), Error> {
    let tmp = format!("{path}.tmp");
    mesh.to_file()?.write(&tmp)?;
    let _ = std::fs::remove_file(path);
    std::fs::rename(&tmp, path).map_err(|_| Error::Format(format!("cannot rename to {path}")))
}

/// The run's totals, kept for the final progress report.
#[derive(Clone, Copy, Default)]
struct Totals {
    total: usize,
    fetched: usize,
    built: usize,
    failed: usize,
    elapsed: Duration,
}

fn emit_progress(p: &Totals) {
    let done = p.built + p.failed;
    let elapsed = p.elapsed.as_secs_f64();
    let rate = done as f64 / elapsed.max(1e-3);
    let eta = if rate > 0.0 { (p.total - done) as f64 / rate } else { 0.0 };
    if json() {
        emit(json!({
            "event": "progress", "done": done, "total": p.total, "fetched": p.fetched, "failed": p.failed,
            "elapsed": elapsed, "rate": rate, "eta": eta,
        }));
    } else {
        let e = eta as i64;
        print!(
            "\r{done}/{} tiles ({} failed), {rate:.1} tiles/s, {}:{:02}:{:02} left   ",
            p.total,
            p.failed,
            e / 3600,
            e / 60 % 60,
            e % 60
        );
        let _ = std::io::stdout().flush();
    }
}

pub fn generate(args: &[&str]) -> CmdResult {
    let a = parse_args(args)?;
    let out = a.out.as_str();

    let capture = a.capture.as_deref().map(Capture::read).transpose()?.map(Arc::new);
    let mut source: Box<dyn TileSource> = match &capture {
        Some(c) => Box::new(CaptureSource::new(c.clone())),
        None => {
            let hosts: Vec<String> = a.server.split(',').filter(|h| !h.is_empty()).map(String::from).collect();
            Box::new(DataServerSource::new(hosts, DATA_SERVER_PORT)?)
        }
    };
    let fetchers = a.connections.unwrap_or(if capture.is_some() {
        1
    } else {
        a.server.split(',').filter(|h| !h.is_empty()).count()
    });
    let params = source.world_params()?;
    let b = params.block;

    let mut range = a.range;
    if let Some(c) = &capture
        && range.is_none()
    {
        // Default to the captured tiles' bounding range.
        let mut r = [1 << 30, 1 << 30, -1, -1];
        for t in &c.tiles {
            r = [r[0].min(t.x), r[1].min(t.z), r[2].max(t.x), r[3].max(t.z)];
        }
        range = Some(r);
    }

    let (mut mesh, existing) = match &a.base {
        Some(path) => {
            let file = NavMeshFile::read(path)?;
            let existing: HashSet<(i32, i32)> =
                file.tiles.iter().filter(|t| t.header.layer == 0).map(|t| (t.header.x, t.header.y)).collect();
            (NavMeshAssembly::load(&file)?, existing)
        }
        None => (NavMeshAssembly::new(b.tiles_x, b.tiles_z, b.tile_width, b.tile_height, [0.0; 3])?, HashSet::new()),
    };
    if a.skip_existing && a.base.is_none() {
        return Err(bad_args("--skip-existing needs --base"));
    }
    let include = |x: i32, z: i32| !existing.contains(&(x, z));
    let on_fetched = |x: i32, z: i32| emit(json!({ "event": "fetched", "x": x, "z": z }));
    let options = GenerateOptions {
        range,
        builders: a.builders,
        fetchers,
        cancel: Some(cancel_flag()),
        capture_path: a.record.as_ref().map(Into::into),
        include: if a.skip_existing { Some(&include) } else { None },
        on_fetched: if json() { Some(&on_fetched) } else { None },
        ..Default::default()
    };

    let planned = planned_tiles(&params, &options);
    if json() {
        let [x0, z0, x1, z1] = range.unwrap_or([0, 0, -1, -1]);
        let clamp = |v: i32, n: i32| if v < 0 { n - 1 } else { v.min(n - 1) };
        let mut j = json!({
            "event": "start", "output": out, "tiles": planned.len(),
            "range": [x0.max(0), z0.max(0), clamp(x1, b.tiles_x), clamp(z1, b.tiles_z)],
            "world": { "tilesX": b.tiles_x, "tilesZ": b.tiles_z, "tileWidth": b.tile_width, "tileHeight": b.tile_height },
        });
        match &a.capture {
            Some(c) => j["capture"] = json!(c),
            None => j["server"] = json!(a.server),
        }
        if let Some(base) = &a.base {
            j["base"] = json!(base);
            j["skipExisting"] = json!(a.skip_existing);
        }
        emit(j);
    }

    let t0 = Instant::now();
    let build = |p: &_, input: &_| build_tile(p, input, &a.build);
    stop_on_ctrl_c();
    if json() {
        stop_on_stdin();
    }
    let (mut stray_tiles, mut since_checkpoint) = (0, 0);
    let mut last = Totals::default();
    let mut last_emit: Option<Instant> = None;
    // Checkpoints save from inside the run; one that fails stops it, and is reported after.
    let mut checkpoint_error = None;
    let mut on_progress = |p: &GenerateProgress, mesh: &NavMeshAssembly| {
        if p.last.stray_part_ids != 0 {
            stray_tiles += 1;
        }
        if json() {
            let t = p.last;
            let mut j = json!({
                "event": "tile", "x": t.x, "z": t.z,
                "outcome": match p.last_outcome {
                    TileOutcome::Built => "built",
                    TileOutcome::Empty => "empty",
                    TileOutcome::Failed => "failed",
                },
            });
            if p.last_outcome == TileOutcome::Built {
                j["bytes"] = json!(t.data_size);
                j["polys"] = json!(t.polys);
                j["verts"] = json!(t.verts);
                j["links"] = json!(t.off_mesh);
                j["obstacles"] = json!(t.obstacles);
            }
            if t.stray_part_ids != 0 {
                j["strayPartIds"] = json!(t.stray_part_ids);
            }
            j["ms"] = json!(t.build_ms);
            if let Some(e) = p.last_error {
                j["error"] = json!(e);
            }
            emit(j);
        }
        // Throttle progress output; JSON consumers get at most 4 updates a second.
        last = Totals { total: p.total, fetched: p.fetched, built: p.built, failed: p.failed, elapsed: p.elapsed };
        if !json() || last_emit.is_none_or(|t| t.elapsed() >= Duration::from_millis(250)) {
            last_emit = Some(Instant::now());
            emit_progress(&last);
        }
        if a.checkpoint_every > 0 && checkpoint_error.is_none() {
            since_checkpoint += 1;
            if since_checkpoint >= a.checkpoint_every {
                since_checkpoint = 0;
                match save(mesh, out) {
                    Ok(()) if json() => emit(json!({ "event": "saved", "path": out, "checkpoint": true })),
                    Ok(()) => {}
                    Err(e) => {
                        checkpoint_error = Some(e);
                        cancel_flag().store(true, Ordering::Relaxed);
                    }
                }
            }
        }
    };
    let r = run(source.as_mut(), &params, &mut mesh, &build, &options, &mut on_progress)?;
    if let Some(e) = checkpoint_error {
        return Err(e.into());
    }
    if json() && last.total > 0 {
        emit_progress(&last);
    }
    let cancelled = cancel_flag().load(Ordering::Relaxed);
    let seconds = t0.elapsed().as_secs_f64();
    if !json() {
        println!(
            "\n{}built {} tiles, {} empty, {} failed in {seconds:.1} s",
            if cancelled { "stopped: " } else { "" },
            r.built,
            r.empty,
            r.failed
        );
        for e in r.errors.iter().take(20) {
            println!("  {e}");
        }
        if stray_tiles > 0 {
            println!("{stray_tiles} tiles had out-of-range door part ids, which were ignored");
        }
    }

    // A stopped run skips the island filter; resuming runs it over the whole mesh.
    if !cancelled && a.island_filter && b.filter_isolated_islands != 0 {
        let seed = a.seed.unwrap_or(b.seed_position);
        let f = mesh.filter_isolated_islands(seed, false)?;
        if json() {
            let mut j = json!({ "event": "islands", "seedFound": f.seed_found, "seed": seed });
            if f.seed_found {
                j["seedPoint"] = json!(f.seed_point);
                j["reachable"] = json!(f.reachable);
                j["isolated"] = json!(f.isolated);
            }
            emit(j);
        } else if f.seed_found {
            let p = f.seed_point;
            println!(
                "island filter: seed ({:.1} {:.1} {:.1}) -> {} reachable, {} isolated polys",
                p[0], p[1], p[2], f.reachable, f.isolated
            );
        } else {
            println!(
                "island filter skipped: no polygon near seed ({:.1} {:.1} {:.1}) in the generated area",
                seed[0], seed[1], seed[2]
            );
        }
    }

    // Don't replace the output with an empty mesh when stopped before the first tile.
    let saved = !cancelled || r.built + r.empty > 0;
    if saved {
        save(&mesh, out)?;
    }
    let resumable = cancelled || (!r.errors.is_empty() && r.failed == 0);
    if json() {
        emit(json!({
            "event": "done", "output": out, "saved": saved, "cancelled": cancelled, "built": r.built,
            "empty": r.empty, "failed": r.failed, "seconds": seconds, "strayTiles": stray_tiles,
            "resumable": resumable, "errorCount": r.errors.len(), "errors": &r.errors[..r.errors.len().min(100)],
        }));
    } else {
        if saved {
            println!("wrote {out}");
        }
        if resumable && saved {
            println!("generation stopped early; continue with: --base {out} --skip-existing");
        }
    }
    Ok(if cancelled {
        ExitCode::from(130)
    } else if r.failed > 0 || !r.errors.is_empty() {
        ExitCode::FAILURE
    } else {
        ExitCode::SUCCESS
    })
}
