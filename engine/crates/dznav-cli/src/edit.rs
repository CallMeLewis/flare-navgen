//! Commands that rewrite navmeshes through Detour: resave, merge, islands.

use std::process::ExitCode;

use dznav::format::{Link, NULL_LINK};
use dznav::{Error, NavMeshAssembly, NavMeshFile};
use serde_json::json;

use crate::{CmdResult, emit, json};

pub fn resave(input: &str, output: &str) -> CmdResult {
    let mesh = NavMeshAssembly::load(&NavMeshFile::read(input)?)?;
    mesh.to_file()?.write(output)?;
    println!("wrote {output}");
    Ok(ExitCode::SUCCESS)
}

pub fn merge(base_path: &str, patch_path: &str, out: &str, [x0, z0, x1, z1]: [i32; 4]) -> CmdResult {
    let base = NavMeshFile::read(base_path)?;
    let patch = NavMeshFile::read(patch_path)?;
    let (b, p) = (&base.header, &patch.header);
    if b.tiles_x != p.tiles_x || b.tiles_z != p.tiles_z || b.tile_width != p.tile_width {
        return Err(Error::Format("tile grids differ".into()).into());
    }
    let mut mesh = NavMeshAssembly::load(&base)?;
    let mut replaced = 0;
    for t in &patch.tiles {
        if t.header.x < x0 || t.header.x > x1 || t.header.y < z0 || t.header.y > z1 {
            continue;
        }
        // Links are rebuilt when the tile is added.
        let mut fresh = t.clone();
        fresh.links.fill(Link::default());
        for poly in &mut fresh.polys {
            poly.first_link = NULL_LINK;
        }
        mesh.set_tile(&fresh.serialize())?;
        replaced += 1;
    }
    mesh.to_file()?.write(out)?;
    if json() {
        emit(json!({ "event": "merge", "replaced": replaced, "output": out }));
    } else {
        println!("replaced {replaced} tiles, wrote {out}");
    }
    Ok(ExitCode::SUCCESS)
}

pub fn islands(input: &str, output: &str, seed: [f32; 3], all_heights: bool) -> CmdResult {
    let mut mesh = NavMeshAssembly::load(&NavMeshFile::read(input)?)?;
    let r = mesh.filter_isolated_islands(seed, all_heights)?;
    if !r.seed_found {
        return Err(Error::Format("no polygon near the seed (try --all-heights)".into()).into());
    }
    mesh.to_file()?.write(output)?;
    if json() {
        emit(json!({
            "event": "islands", "seedFound": true, "seedPoint": r.seed_point, "reachable": r.reachable,
            "isolated": r.isolated, "output": output,
        }));
        return Ok(ExitCode::SUCCESS);
    }
    let p = r.seed_point;
    println!(
        "seed snapped to ({:.2} {:.2} {:.2}): {} reachable, {} isolated polys",
        p[0], p[1], p[2], r.reachable, r.isolated
    );
    println!("wrote {output}");
    Ok(ExitCode::SUCCESS)
}
