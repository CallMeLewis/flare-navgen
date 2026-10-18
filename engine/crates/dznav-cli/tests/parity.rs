//! Runs the Rust and the C++ `dznav` on the same inputs and requires the same results, when `DZNAV_ORACLE` names the
//! C++ binary: the same files byte for byte, the same text output, and the same JSON events (timings aside, numbers
//! to float precision).

#[path = "../../dznav/tests/common/mod.rs"]
mod common;

use std::path::{Path, PathBuf};
use std::process::{Child, Command, Output, Stdio};
use std::time::{Duration, Instant};

use dznav::CaptureWriter;
use serde_json::Value;

const RUST: &str = env!("CARGO_BIN_EXE_dznav");

struct Run {
    code: Option<i32>,
    stdout: String,
}

fn run(exe: &Path, args: &[&str]) -> Run {
    let Output { status, stdout, stderr } = Command::new(exe).args(args).output().unwrap();
    // The C++ engine's stdout is in text mode, which writes \r\n on Windows.
    let stdout = String::from_utf8_lossy(&stdout).replace("\r\n", "\n");
    if !status.success() {
        eprintln!("{} {args:?} exited with {status}:\n{stdout}{}", exe.display(), String::from_utf8_lossy(&stderr));
    }
    Run { code: status.code(), stdout }
}

/// Both engines' output for the same arguments.
fn both(oracle: &Path, args: &[&str]) -> (Run, Run) {
    (run(oracle, args), run(Path::new(RUST), args))
}

fn same_text(oracle: &Path, args: &[&str]) {
    let (c, r) = both(oracle, args);
    assert_eq!(r.code, c.code, "exit code of {args:?}");
    assert_eq!(r.stdout, c.stdout, "output of {args:?}");
}

/// Timings, which differ from run to run.
const TIMING: &[&str] = &["ms", "elapsed", "rate", "eta", "seconds"];

fn same_value(a: &Value, b: &Value, path: &str) {
    match (a, b) {
        (Value::Number(x), Value::Number(y)) => {
            let (x, y) = (x.as_f64().unwrap(), y.as_f64().unwrap());
            // The C++ engine prints floats with 9 significant digits, this one as the shortest f32 that reads back.
            assert!((x - y).abs() <= 1e-6 * x.abs().max(y.abs()).max(1.0), "{path}: {x} vs {y}");
        }
        (Value::Object(x), Value::Object(y)) => {
            let keys = |o: &serde_json::Map<String, Value>| {
                let mut k: Vec<_> = o.keys().filter(|k| !TIMING.contains(&k.as_str())).cloned().collect();
                k.sort();
                k
            };
            assert_eq!(keys(x), keys(y), "{path}: fields");
            for k in keys(x) {
                same_value(&x[&k], &y[&k], &format!("{path}.{k}"));
            }
        }
        (Value::Array(x), Value::Array(y)) => {
            assert_eq!(x.len(), y.len(), "{path}: length");
            for (i, (x, y)) in x.iter().zip(y).enumerate() {
                same_value(x, y, &format!("{path}[{i}]"));
            }
        }
        _ => assert_eq!(a, b, "{path}"),
    }
}

fn events(out: &str) -> Vec<Value> {
    out.lines().map(|l| serde_json::from_str(l).unwrap_or_else(|e| panic!("not JSON ({e}): {l}"))).collect()
}

fn same_json(oracle: &Path, args: &[&str]) {
    let mut args = args.to_vec();
    args.push("--json");
    let (c, r) = both(oracle, &args);
    assert_eq!(r.code, c.code, "exit code of {args:?}");
    let (ce, re) = (events(&c.stdout), events(&r.stdout));
    assert_eq!(re.len(), ce.len(), "events of {args:?}");
    for (c, r) in ce.iter().zip(&re) {
        if c["event"] == "error" {
            // Messages may add the system's reason.
            assert_eq!(r["kind"], c["kind"], "{args:?}");
        } else {
            same_value(c, r, &format!("{args:?}"));
        }
    }
}

fn same_file(a: &Path, b: &Path) {
    let (mut x, mut y) = (std::fs::read(a).unwrap(), std::fs::read(b).unwrap());
    // The C++ engine writes OBJ files in text mode, with \r\n on Windows.
    if a.extension().is_some_and(|e| e == "obj") {
        let unix = |v: &[u8]| String::from_utf8_lossy(v).replace("\r\n", "\n").into_bytes();
        (x, y) = (unix(&x), unix(&y));
    }
    assert!(x == y, "{} and {} differ", a.display(), b.display());
}

/// `generate --json` from both engines: the same output file, and the same events apart from progress (throttled by
/// time) and the order fetches interleave with builds.
fn same_generate(oracle: &Path, dir: &Path, name: &str, args: &[&str]) {
    let (cpp, rust) = (dir.join(format!("{name}-cpp.nm")), dir.join(format!("{name}-rust.nm")));
    let mut outs = Vec::new();
    for (exe, out) in [(oracle, &cpp), (Path::new(RUST), &rust)] {
        let mut a = vec!["generate", out.to_str().unwrap(), "--threads", "1", "--json"];
        a.extend_from_slice(args);
        let r = run(exe, &a);
        let mut ev = events(&r.stdout);
        ev.retain(|e| e["event"] != "progress");
        for e in &mut ev {
            for field in ["output", "path"] {
                if let Some(o) = e.get_mut(field) {
                    *o = Value::Null;
                }
            }
        }
        let (mut fetched, rest): (Vec<_>, Vec<_>) = ev.into_iter().partition(|e| e["event"] == "fetched");
        fetched.sort_by_key(|e| (e["z"].as_i64(), e["x"].as_i64()));
        outs.push((r.code, fetched, rest));
    }
    same_file(&cpp, &rust);
    let ((cc, cf, ce), (rc, rf, re)) = (&outs[0], &outs[1]);
    assert_eq!(rc, cc, "{name}: exit code");
    same_value(&Value::Array(cf.clone()), &Value::Array(rf.clone()), &format!("{name} fetched"));
    same_value(&Value::Array(ce.clone()), &Value::Array(re.clone()), &format!("{name} events"));
}

fn write_capture(path: &Path) {
    let mut w = CaptureWriter::create(path, &common::world_params()).unwrap();
    for t in common::tiles() {
        w.add(&t).unwrap();
    }
}

fn s(p: &Path) -> &str {
    p.to_str().unwrap()
}

#[test]
fn navmesh_commands_match_cpp() {
    let Some(oracle) = common::oracle() else { return };
    let dir = common::scratch_dir("cli_navmesh_commands");
    let capture = dir.join("world.dzgeo");
    write_capture(&capture);
    let world = dir.join("world.nm");
    let variant = dir.join("variant.nm");
    run(&oracle, &["generate", s(&world), "--capture", s(&capture)]);
    run(&oracle, &["generate", s(&variant), "--capture", s(&capture), "--doors", "open", "--no-links"]);

    same_text(&oracle, &["capture-info", s(&capture)]);
    for args in [
        vec!["info", s(&world)],
        vec!["tile", s(&world), "1", "1"],
        vec!["tile", s(&world), "0", "2"],
        vec!["verify", s(&world), s(&world)],
        vec!["verify", s(&world), s(&variant), "--ignore-order"],
        vec!["diff", s(&world), s(&variant)],
        vec!["diff", s(&world), s(&variant), "3"],
    ] {
        same_text(&oracle, &args);
        same_json(&oracle, &args);
    }
    same_json(&oracle, &["info", s(&world), "--tiles"]);
    same_json(&oracle, &["tile", s(&world), "7", "7"]);
    same_json(&oracle, &["info", s(&dir.join("missing.nm"))]);
    same_json(&oracle, &["roundtrip", s(&world)]);

    // Commands that write files: the same files and output.
    let out = |engine: &str, name: &str| dir.join(format!("{engine}-{name}"));
    for (name, args) in [
        ("render.png", vec!["render", s(&world), "{out}", "0", "0", "150", "150", "4"]),
        ("render-small.png", vec!["render", s(&world), "{out}", "20.5", "40", "90", "75.25"]),
        ("export.obj", vec!["export-obj", s(&world), "{out}"]),
        ("export-tile.obj", vec!["export-obj", s(&world), "{out}", "1", "1", "1", "1"]),
        ("resave.nm", vec!["resave", s(&world), "{out}"]),
        ("merge.nm", vec!["merge", s(&world), s(&variant), "{out}", "1", "0", "2", "1"]),
        ("islands.nm", vec!["islands", s(&world), "{out}", "130", "10", "130"]),
        ("islands-high.nm", vec!["islands", s(&world), "{out}", "60", "200", "60", "--all-heights"]),
    ] {
        for json in [false, true] {
            let mut runs = Vec::new();
            for (engine, exe) in [("cpp", oracle.as_path()), ("rust", Path::new(RUST))] {
                let target = out(engine, name);
                let mut a: Vec<&str> = args.iter().map(|&a| if a == "{out}" { s(&target) } else { a }).collect();
                if json {
                    a.push("--json");
                }
                let mut r = run(exe, &a);
                let escaped = serde_json::to_string(s(&target)).unwrap();
                r.stdout = r.stdout.replace(&escaped[1..escaped.len() - 1], "{out}").replace(s(&target), "{out}");
                runs.push(r);
            }
            assert_eq!(runs[1].code, runs[0].code, "exit code of {args:?}");
            if json && args[0] != "export-obj" && args[0] != "resave" {
                same_value(
                    &Value::Array(events(&runs[0].stdout)),
                    &Value::Array(events(&runs[1].stdout)),
                    &format!("{args:?}"),
                );
            } else if !json {
                assert_eq!(runs[1].stdout, runs[0].stdout, "output of {args:?}");
            }
            if runs[0].code == Some(0) {
                same_file(&out("cpp", name), &out("rust", name));
            }
        }
    }
}

#[test]
fn generate_matches_cpp() {
    let Some(oracle) = common::oracle() else { return };
    let dir = common::scratch_dir("cli_generate");
    let capture = dir.join("world.dzgeo");
    write_capture(&capture);
    let c = s(&capture);
    same_generate(&oracle, &dir, "whole", &["--capture", c]);
    same_generate(&oracle, &dir, "range", &["--capture", c, "--range", "1", "0", "2", "1", "--doors", "enabled"]);
    same_generate(
        &oracle,
        &dir,
        "options",
        &["--capture", c, "--no-links", "--sea-level", "10.2", "--seed", "20", "10", "130"],
    );
    same_generate(&oracle, &dir, "no-islands", &["--capture", c, "--no-island-filter", "--checkpoint", "2"]);

    // Resuming: build part, then the rest on top of it.
    let part = dir.join("part.nm");
    run(&oracle, &["generate", s(&part), "--capture", c, "--range", "0", "0", "2", "0"]);
    same_generate(&oracle, &dir, "resume", &["--capture", c, "--base", s(&part), "--skip-existing"]);
    same_generate(&oracle, &dir, "replace", &["--capture", c, "--base", s(&part), "--range", "0", "0", "0", "2"]);

    // Recording the fetched geometry.
    for (engine, exe) in [("cpp", oracle.as_path()), ("rust", Path::new(RUST))] {
        let out = dir.join(format!("record-{engine}.nm"));
        let rec = dir.join(format!("record-{engine}.dzgeo"));
        run(exe, &["generate", s(&out), "--capture", c, "--threads", "1", "--record", s(&rec)]);
    }
    same_file(&dir.join("record-cpp.dzgeo"), &dir.join("record-rust.dzgeo"));

    // Bad arguments.
    same_json(&oracle, &["generate", s(&dir.join("x.nm")), "--capture", c, "--bogus"]);
    same_json(&oracle, &["generate", s(&dir.join("x.nm")), "--capture", c, "--skip-existing"]);
    same_json(&oracle, &["generate", s(&dir.join("x.nm")), "--capture", s(&dir.join("missing.dzgeo"))]);
}

struct Server(Child);

impl Drop for Server {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

/// Starts a replay server on the data-server port and waits until it answers.
fn replay_server(exe: &Path, capture: &Path) -> Option<Server> {
    let child = Command::new(exe)
        .args(["replay-server", s(capture)])
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let mut server = Server(child);
    let start = Instant::now();
    while start.elapsed() < Duration::from_secs(10) {
        if let Ok(Some(status)) = server.0.try_wait() {
            eprintln!("replay server exited with {status}; is something else on port 36533?");
            return None;
        }
        if std::net::TcpStream::connect(("127.0.0.1", dznav::net::DATA_SERVER_PORT)).is_ok() {
            return Some(server);
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    None
}

#[test]
fn data_server_commands_match_cpp() {
    let Some(oracle) = common::oracle() else { return };
    let dir = common::scratch_dir("cli_data_server");
    let capture = dir.join("world.dzgeo");
    write_capture(&capture);
    if std::net::TcpStream::connect(("127.0.0.1", dznav::net::DATA_SERVER_PORT)).is_ok() {
        eprintln!("a data server is already running on port 36533; skipping");
        return;
    }
    // Each engine's client against each engine's server.
    for (server_name, server_exe) in [("cpp", oracle.clone()), ("rust", PathBuf::from(RUST))] {
        let Some(_server) = replay_server(&server_exe, &capture) else {
            panic!("{server_name} replay server didn't start")
        };
        same_text(&oracle, &["probe"]);
        same_text(&oracle, &["probe", "127.0.0.1", "1", "1"]);
        same_json(&oracle, &["probe", "127.0.0.1", "1", "1"]);
        same_json(&oracle, &["probe", "127.0.0.1", "5", "5"]);
        for (engine, exe) in [("cpp", oracle.as_path()), ("rust", Path::new(RUST))] {
            let out = dir.join(format!("{server_name}-{engine}.dzgeo"));
            let r = run(exe, &["capture", s(&out), "0", "0", "3", "2", "--json"]);
            assert_eq!(r.code, Some(0));
        }
        same_file(&dir.join(format!("{server_name}-cpp.dzgeo")), &dir.join(format!("{server_name}-rust.dzgeo")));
        same_generate(&oracle, &dir, &format!("{server_name}-server"), &["--server", "127.0.0.1"]);
    }
    // No server.
    same_json(&oracle, &["probe"]);
}

/// The inspector's commands on shipped navmeshes (`DZNAV_SAMPLE_NM`).
#[test]
fn sample_commands_match_cpp() {
    let Some(oracle) = common::oracle() else { return };
    let dir = common::scratch_dir("cli_samples");
    for (n, path) in common::sample_navmeshes().iter().enumerate() {
        let nm = s(path);
        let reader = dznav::NavMeshReader::open(path).unwrap();
        let tiles = reader.tiles();
        let (first, middle) = (tiles[0].header, tiles[tiles.len() / 2].header);
        let (x, z) = (middle.x.to_string(), middle.y.to_string());
        drop(reader);

        let started = Instant::now();
        same_text(&oracle, &["info", nm]);
        same_json(&oracle, &["info", nm, "--tiles"]);
        for (x, z) in [(first.x.to_string(), first.y.to_string()), (x.clone(), z.clone())] {
            same_text(&oracle, &["tile", nm, &x, &z]);
            same_json(&oracle, &["tile", nm, &x, &z]);
        }
        let (bx, bz) = (middle.bmin[0].to_string(), middle.bmin[2].to_string());
        let (ex, ez) = ((middle.bmin[0] + 300.0).to_string(), (middle.bmin[2] + 200.0).to_string());
        for engine in ["cpp", "rust"] {
            let out = dir.join(format!("{n}-{engine}.png"));
            let exe = if engine == "cpp" { oracle.as_path() } else { Path::new(RUST) };
            assert_eq!(run(exe, &["render", nm, s(&out), &bx, &bz, &ex, &ez, "3"]).code, Some(0));
        }
        same_file(&dir.join(format!("{n}-cpp.png")), &dir.join(format!("{n}-rust.png")));
        eprintln!("{nm}: info, tile and render match ({:.1} s)", started.elapsed().as_secs_f64());
    }
}
