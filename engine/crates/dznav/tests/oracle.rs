//! Checks against the C++ engine, when `DZNAV_ORACLE` names its dznav binary.

mod common;

use std::path::Path;
use std::process::Command;

use dznav::{CaptureWriter, NavMeshAssembly, NavMeshFile, TileBuildOptions, build_tile};

fn run(oracle: &Path, args: &[&dyn AsRef<std::ffi::OsStr>]) -> String {
    let out = Command::new(oracle).args(args.iter().map(|a| a.as_ref())).output().unwrap();
    let text = format!("{}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
    assert!(out.status.success(), "dznav {:?} failed:\n{text}", args.iter().map(|a| a.as_ref()).collect::<Vec<_>>());
    text
}

fn write_capture(path: &Path) {
    let mut w = CaptureWriter::create(path, &common::world_params()).unwrap();
    for t in common::tiles() {
        w.add(&t).unwrap();
    }
}

#[test]
fn cpp_reads_our_captures() {
    let Some(oracle) = common::oracle() else { return };
    let dir = common::scratch_dir("cpp_reads_our_captures");
    let capture = dir.join("world.dzgeo");
    write_capture(&capture);
    let info = run(&oracle, &[&"capture-info", &capture]);
    assert!(info.contains("9 tiles"), "{info}");
}

/// Builds the synthetic world with the C++ engine's `generate` and through dznav-sys, and compares the files byte for
/// byte: tile building, linking and the island filter all have to agree.
#[test]
fn generated_navmesh_matches_cpp() {
    let Some(oracle) = common::oracle() else { return };
    let dir = common::scratch_dir("generated_navmesh_matches_cpp");
    let (capture, theirs, ours) = (dir.join("world.dzgeo"), dir.join("cpp.nm"), dir.join("rust.nm"));
    write_capture(&capture);
    // One builder thread adds tiles in planned order, so slots (and so links) are deterministic.
    run(&oracle, &[&"generate", &theirs, &"--capture", &capture, &"--threads", &"1"]);

    let params = common::world_params();
    let b = &params.block;
    let mut mesh = NavMeshAssembly::new(b.tiles_x, b.tiles_z, b.tile_width, b.tile_height, [0.0; 3]).unwrap();
    let (mut links, mut parts, mut obstacles) = (0, 0, 0);
    for input in common::tiles() {
        let built = build_tile(&params, &input, &TileBuildOptions::default()).unwrap();
        let data = built.data.expect("every synthetic tile has walkable ground");
        mesh.set_tile(&data).unwrap();
        links += built.stats.off_mesh;
        obstacles += built.stats.obstacles;
        parts += dznav::Tile::parse(&data, 0).unwrap().part_polys.len();
    }
    let islands = mesh.filter_isolated_islands(b.seed_position, false).unwrap();
    eprintln!("{links} off-mesh links, {obstacles} obstacles, {parts} door polys, {islands:?}");
    assert!(links > 0 && obstacles > 0 && parts > 0 && islands.seed_found, "the synthetic world should cover these");
    mesh.to_file().unwrap().write(&ours).unwrap();

    let (a, b) = (std::fs::read(&theirs).unwrap(), std::fs::read(&ours).unwrap());
    if a != b {
        let verify = Command::new(&oracle).arg("verify").arg(&theirs).arg(&ours).output().unwrap();
        let (fa, fb) = (NavMeshFile::parse(&a).unwrap(), NavMeshFile::parse(&b).unwrap());
        for (ta, tb) in fa.tiles.iter().zip(&fb.tiles) {
            if ta != tb {
                eprintln!("tile ({}, {}) differs", ta.header.x, ta.header.y);
            }
        }
        panic!("navmeshes differ; dznav verify says:\n{}", String::from_utf8_lossy(&verify.stdout));
    }
}
