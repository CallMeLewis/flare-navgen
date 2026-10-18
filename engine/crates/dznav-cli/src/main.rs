//! The `dznav` command-line tool. The desktop app runs it with `--json` (docs/cli-json.md).

mod edit;
mod generate;
mod inspect;
mod server;

use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, Ordering};

use dznav::Error;

/// --json: print results and progress as JSON lines (see docs/cli-json.md).
static JSON: AtomicBool = AtomicBool::new(false);

/// Set by Ctrl+C, or by a "stop" line on stdin in --json mode: generation stops and saves what it has.
static CANCEL: AtomicBool = AtomicBool::new(false);

pub fn json() -> bool {
    JSON.load(Ordering::Relaxed)
}

pub fn cancel_flag() -> &'static AtomicBool {
    &CANCEL
}

/// Writes one JSON event as a line on stdout. Safe to call from several threads.
pub fn emit(event: serde_json::Value) {
    println!("{event}");
}

/// The first Ctrl+C asks the running command to stop; a second one quits at once.
pub fn stop_on_ctrl_c() {
    let _ = ctrlc::set_handler(|| {
        if CANCEL.swap(true, Ordering::Relaxed) {
            std::process::exit(130);
        }
    });
}

/// In --json mode, a "stop" line on stdin asks the running command to stop. Closing stdin doesn't.
pub fn stop_on_stdin() {
    std::thread::spawn(|| {
        for line in std::io::stdin().lines() {
            let Ok(line) = line else { break };
            if line.trim_end_matches('\r') == "stop" {
                CANCEL.store(true, Ordering::Relaxed);
            }
        }
    });
}

/// A command's failure, reported as an error event or message.
pub struct Fail(Error);

impl From<Error> for Fail {
    fn from(e: Error) -> Self {
        Fail(e)
    }
}

pub type CmdResult = Result<ExitCode, Fail>;

/// A command-line mistake, reported like a bad file, as the C++ engine did.
pub fn bad_args(message: impl Into<String>) -> Fail {
    Fail(Error::Format(message.into()))
}

/// Like C's atoi: the leading integer, or 0.
pub fn atoi(s: &str) -> i32 {
    let t = s.trim_start();
    let end = t.char_indices().take_while(|&(i, c)| c.is_ascii_digit() || (i == 0 && (c == '-' || c == '+'))).count();
    t[..end].parse::<i64>().map_or(0, |v| v.clamp(i32::MIN.into(), i32::MAX.into()) as i32)
}

/// Like C's atof: the longest leading number, or 0.
pub fn atof(s: &str) -> f64 {
    let t = s.trim_start();
    (1..=t.len()).rev().filter(|&n| t.is_char_boundary(n)).find_map(|n| t[..n].parse::<f64>().ok()).unwrap_or(0.0)
}

fn usage() -> ExitCode {
    eprint!(
        "dznav - DayZ navmesh toolkit

Usage:
  dznav info <file.nm> [--tiles]             Summary of a navmesh file (--tiles: per-tile list, JSON only)
  dznav roundtrip <file.nm>                  Parse and re-serialize, verify byte equality
  dznav tile <file.nm> <x> <z>               Dump one tile
  dznav export-obj <file.nm> <out.obj> [x0 z0 x1 z1]
                                             Export polygons (optionally a tile range) as OBJ
  dznav render <file.nm> <out.png> <minX> <minZ> <maxX> <maxZ> [px/m]
                                             Top-down preview of a world-space rectangle
  dznav diff <a.nm> <b.nm> [top N]           Compare two navmeshes tile by tile
  dznav verify <reference.nm> <ours.nm> [--ignore-order]
                                             Byte-compare common tiles (canonical form);
                                             --ignore-order: obstacles and doors as sets
  dznav merge <base.nm> <patch.nm> <out.nm> [x0 z0 x1 z1]
                                             Replace base tiles with patch tiles (optionally a
                                             tile range) and relink the seams
  dznav resave <in.nm> <out.nm>              Load into Detour and save again
  dznav islands <in.nm> <out.nm> <x> <y> <z> [--all-heights]
                                             Re-run the isolated-island filter from a seed

Data server (DayZDiag_x64 -server -startNavmeshDataServer):
  dznav probe [host] [x z]                   Print world params (and summarize one tile)
  dznav capture <out.dzgeo> <x0> <z0> <x1> <z1> [host]
                                             Save tile input geometry for offline generation
  dznav capture-info <file.dzgeo>            Summarize a capture
  dznav replay-server <file.dzgeo>... [--bind addr] [--port N] [--crop] [--sea-level M]
                      [--no-island-filter] [--once]
                                             Serve captured geometry as a data server, so any
                                             client builds from it

Generation:
  dznav generate <out.nm> [options]
      --server <host[,host]>  data server(s) to fetch geometry from (default 127.0.0.1);
                              each serves one client, so list several to fetch in parallel
      --capture <file.dzgeo>  use captured geometry instead of a server
      --range x0 z0 x1 z1     tile range (inclusive); default: whole world / whole capture
      --base <file.nm>        patch the generated tiles into an existing navmesh
      --skip-existing         with --base: only build tiles the base doesn't have (resume)
      --checkpoint N          save the output every N tiles
      --record <file.dzgeo>   save fetched geometry while generating
      --threads N             builder threads (default: cores - 1)
      --connections N         fetch connections (default: one per server)
      --sea-level M           override the server's (tide-dependent) sea level
      --doors closed|open|enabled   initial door state baked into the mesh (default closed)
      --no-links              skip jump/climb links
      --no-island-filter      don't flag polygons unreachable from the seed
      --seed x y z            island filter seed (default: world params)
  Ctrl+C stops generation and saves the tiles built so far (Ctrl+C again quits at once).

Add --json to generate, probe, capture, info, tile, render, verify, diff, merge or islands for
JSON-lines output (docs/cli-json.md). With --json, generate also stops and saves on a \"stop\"
line on stdin.
"
    );
    ExitCode::from(2)
}

/// Reports a failure: `kind` is "usage", "file", "network" or "protocol".
fn fail(kind: &str, message: &str) -> ExitCode {
    if json() {
        emit(serde_json::json!({ "event": "error", "kind": kind, "message": message }));
    } else {
        let label = match kind {
            "network" => "network error",
            "protocol" => "protocol error",
            _ => "error",
        };
        eprintln!("\n{label}: {message}");
    }
    ExitCode::FAILURE
}

const JSON_COMMANDS: &[&str] =
    &["generate", "probe", "capture", "info", "tile", "render", "verify", "diff", "merge", "islands"];

fn run(args: &[&str]) -> Option<CmdResult> {
    let cmd = args[1];
    let rest = &args[2..];
    let range = |r: &[&str]| -> [i32; 4] {
        if r.len() >= 4 { [atoi(r[0]), atoi(r[1]), atoi(r[2]), atoi(r[3])] } else { [0, 0, 1 << 30, 1 << 30] }
    };
    Some(match (cmd, rest) {
        ("probe", _) => {
            let host = rest.first().copied().unwrap_or("127.0.0.1");
            let tile = if rest.len() >= 3 { Some((atoi(rest[1]), atoi(rest[2]))) } else { None };
            server::probe(host, tile)
        }
        ("capture", [out, x0, z0, x1, z1, more @ ..]) => {
            server::capture(out, [atoi(x0), atoi(z0), atoi(x1), atoi(z1)], more.first().copied().unwrap_or("127.0.0.1"))
        }
        ("capture-info", [path, ..]) => server::capture_info(path),
        ("replay-server", [_, ..]) => server::replay_server(rest),
        ("generate", [_, ..]) => generate::generate(rest),
        (_, []) => return None,
        ("info", [path, more @ ..]) => inspect::info(path, more.first() == Some(&"--tiles")),
        ("roundtrip", [path, ..]) => inspect::roundtrip(path),
        ("tile", [path, x, z, ..]) => inspect::tile(path, atoi(x), atoi(z)),
        ("export-obj", [path, out, more @ ..]) => inspect::export_obj(path, out, range(more)),
        ("resave", [input, output, ..]) => edit::resave(input, output),
        ("islands", [input, output, x, y, z, more @ ..]) => edit::islands(
            input,
            output,
            [atof(x) as f32, atof(y) as f32, atof(z) as f32],
            more.first() == Some(&"--all-heights"),
        ),
        ("verify", [reference, candidate, more @ ..]) => {
            inspect::verify(reference, candidate, more.first() == Some(&"--ignore-order"))
        }
        ("merge", [base, patch, out, more @ ..]) => edit::merge(base, patch, out, range(more)),
        ("diff", [a, b, more @ ..]) => inspect::diff(a, b, more.first().map_or(10, |n| atoi(n))),
        ("render", [path, out, x0, z0, x1, z1, more @ ..]) => {
            let ppm = more.first().map_or(1.0, |v| atof(v) as f32);
            inspect::render(path, out, [atof(x0) as f32, atof(z0) as f32, atof(x1) as f32, atof(z1) as f32], ppm)
        }
        _ => return None,
    })
}

fn main() -> ExitCode {
    // --json may appear anywhere; remove it before the commands read their arguments.
    let mut args: Vec<String> = std::env::args().collect();
    JSON.store(args.iter().any(|a| a == "--json"), Ordering::Relaxed);
    args.retain(|a| a != "--json");
    let args: Vec<&str> = args.iter().map(String::as_str).collect();
    let Some(&cmd) = args.get(1) else { return usage() };
    if json() && !JSON_COMMANDS.contains(&cmd) {
        return fail("usage", &format!("--json is not supported by '{cmd}'"));
    }
    match run(&args) {
        None => usage(),
        Some(Ok(code)) => code,
        Some(Err(Fail(e))) => {
            let kind = match e {
                Error::Net(_) => "network",
                Error::Wire(_) => "protocol",
                Error::Format(_) | Error::Native(_) | Error::Io { .. } => "file",
            };
            fail(kind, &e.to_string())
        }
    }
}
