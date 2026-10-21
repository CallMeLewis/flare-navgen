//! Commands that read navmeshes: info, roundtrip, tile, export-obj, render, verify, diff.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::process::ExitCode;
use std::time::Instant;

use dznav::compare::{compare_navmeshes, verify_tiles};
use dznav::format::{SET_VERSION, ref_salt, ref_tile};
use dznav::preview::{PreviewOptions, render_area};
use dznav::summary::{navmesh_info, part_polys, tile_detail};
use dznav::{Error, NavMeshFile, NavMeshReader};
use serde_json::{Map, Value, json};

use crate::{CmdResult, emit, emit_serialized, json};

/// A float as C's `%g` (6 significant digits) or C++ stream output prints it.
pub fn fmt_g(v: f32) -> String {
    let v = f64::from(v);
    if v == 0.0 {
        return if v.is_sign_negative() { "-0".into() } else { "0".into() };
    }
    if !v.is_finite() {
        return if v.is_nan() {
            "nan".into()
        } else if v > 0.0 {
            "inf".into()
        } else {
            "-inf".into()
        };
    }
    let sci = format!("{v:.5e}");
    let (mantissa, exp) = sci.split_once('e').unwrap();
    let exp: i32 = exp.parse().unwrap();
    let trim = |s: &str| {
        if s.contains('.') { s.trim_end_matches('0').trim_end_matches('.').to_string() } else { s.to_string() }
    };
    if !(-4..6).contains(&exp) {
        format!("{}e{}{:02}", trim(mantissa), if exp < 0 { '-' } else { '+' }, exp.abs())
    } else {
        trim(&format!("{v:.*}", (5 - exp) as usize))
    }
}

pub fn info(path: &str, list_tiles: bool) -> CmdResult {
    let reader = NavMeshReader::open(path)?;
    let h = reader.header();
    if !json() {
        println!(
            "tiles: {} ({} x {}), tile size {:.2} x {:.2}, origin ({:.2}, {:.2}, {:.2})",
            h.num_tiles, h.tiles_x, h.tiles_z, h.tile_width, h.tile_height, h.orig[0], h.orig[1], h.orig[2]
        );
        println!("maxTiles {}, maxPolys/tile {}", h.max_tiles, h.max_polys);
    }
    let info = navmesh_info(&reader, path, list_tiles && json())?;
    if json() {
        emit_serialized(&info);
        return Ok(ExitCode::SUCCESS);
    }
    let i = &info;
    println!(
        "polys {}, verts {}, off-mesh links {}, door parts {} ({} polys), obstacles {}, detail meshes {}",
        i.polys, i.verts, i.links, i.door_parts, i.door_polys, i.obstacles, i.detail_meshes
    );
    if let Some(a) = i.agent {
        println!("agent: height {:.2} radius {:.2} climb {:.2}", a.height, a.radius, a.climb);
    }
    let mut line = String::from("cell sizes:");
    for (cs, n) in &i.cell_sizes {
        write!(line, " {cs:.3} x{n}").unwrap();
    }
    line.push_str("\nareas:");
    for (a, n) in &i.areas {
        write!(line, " {a}:{n}").unwrap();
    }
    line.push_str("\nflags:");
    for (f, n) in &i.flags {
        write!(line, " {f:#x}:{n}").unwrap();
    }
    println!("{line}");
    Ok(ExitCode::SUCCESS)
}

pub fn roundtrip(path: &str) -> CmdResult {
    let t0 = Instant::now();
    let original =
        std::fs::read(path).map_err(|source| Error::Io { context: format!("cannot open {path}"), source })?;
    let file = NavMeshFile::parse(&original)?;
    let t1 = Instant::now();
    let mut rewritten = file.serialize()?;
    let t2 = Instant::now();
    let ms = |a: Instant, b: Instant| (b - a).as_secs_f64() * 1000.0;
    println!("parsed {} tiles in {:.0} ms, serialized in {:.0} ms", file.tiles.len(), ms(t0, t1), ms(t1, t2));
    // Older files are saved with the current set version; compare everything else.
    if file.header.version != SET_VERSION {
        println!("set version {} saved as {SET_VERSION}", file.header.version);
        rewritten[4..8].copy_from_slice(&original[4..8]);
    }
    if original.len() != rewritten.len() {
        println!("FAIL: size {} vs {}", original.len(), rewritten.len());
        return Ok(ExitCode::FAILURE);
    }
    if let Some(i) = original.iter().zip(&rewritten).position(|(a, b)| a != b) {
        println!("FAIL: first difference at byte {i}");
        return Ok(ExitCode::FAILURE);
    }
    println!("OK: {} bytes identical", original.len());
    Ok(ExitCode::SUCCESS)
}

pub fn tile(path: &str, x: i32, z: i32) -> CmdResult {
    let reader = NavMeshReader::open(path)?;
    let t = reader.tile_at(x, z)?.ok_or_else(|| Error::Format(format!("no tile ({x},{z}) in {path}")))?;
    let h = &t.header;
    if json() {
        emit_serialized(&tile_detail(&t));
        return Ok(ExitCode::SUCCESS);
    }
    println!(
        "tile ({},{}) ref 0x{:016x} salt {} index {}",
        h.x,
        h.y,
        t.tile_ref,
        ref_salt(t.tile_ref),
        ref_tile(t.tile_ref)
    );
    println!(
        "bounds ({:.2} {:.2} {:.2}) - ({:.2} {:.2} {:.2}), cell {:.3}",
        h.bmin[0],
        h.bmin[1],
        h.bmin[2],
        h.bmax[0],
        h.bmax[1],
        h.bmax[2],
        1.0 / h.bv_quant_factor
    );
    println!(
        "polys {} verts {} links {} bv {} offmesh {} (base {}) parts {} partPolys {} obstacles {}",
        h.poly_count,
        h.vert_count,
        h.max_link_count,
        h.bv_node_count,
        h.off_mesh_con_count,
        h.off_mesh_base,
        t.parts.len(),
        t.part_polys.len(),
        t.obstacles.len()
    );
    let mut out = String::new();
    for (i, p) in t.polys.iter().enumerate() {
        write!(out, "  poly {i}: area {} type {} flags 0x{:04x} verts", p.area(), p.poly_type(), p.flags).unwrap();
        for v in &p.verts[..p.vert_count as usize] {
            write!(out, " {v}").unwrap();
        }
        out.push('\n');
    }
    for (i, c) in t.off_mesh_cons.iter().enumerate() {
        let p = &c.pos;
        writeln!(
            out,
            "  link {i}: class {} ({:.2} {:.2} {:.2}) -> ({:.2} {:.2} {:.2}) centre {:.2} reserved {:.1} rad {:.2} poly {} flags {} side {}",
            c.user_id, p[0], p[1], p[2], p[3], p[4], p[5], c.center_height, c.reserved, c.rad, c.poly, c.flags, c.side
        )
        .unwrap();
    }
    for p in &t.parts {
        write!(out, "  part object 0x{:016x} index {} polys:", p.object_id, p.part_index).unwrap();
        for poly in part_polys(&t, p.first_poly) {
            write!(out, " {poly}").unwrap();
        }
        out.push('\n');
    }
    for o in &t.obstacles {
        writeln!(out, "  obstacle ({:.2} {:.2} {:.2}) r {:.2}", o.pos[0], o.pos[1], o.pos[2], o.radius).unwrap();
    }
    print!("{out}");
    Ok(ExitCode::SUCCESS)
}

pub fn export_obj(path: &str, out_path: &str, [x0, z0, x1, z1]: [i32; 4]) -> CmdResult {
    let reader = NavMeshReader::open(path)?;
    let mut o = format!("# dznav export of {path}\n");
    let mut base = 1usize;
    for (i, e) in reader.tiles().iter().enumerate() {
        if e.x() < x0 || e.x() > x1 || e.z() < z0 || e.z() > z1 {
            continue;
        }
        let t = reader.tile(i)?;
        writeln!(o, "o tile_{}_{}", e.x(), e.z()).unwrap();
        for v in t.verts.chunks(3) {
            writeln!(o, "v {} {} {}", fmt_g(v[0]), fmt_g(v[1]), fmt_g(-v[2])).unwrap();
        }
        for p in t.polys.iter().filter(|p| p.poly_type() == 0) {
            o.push('f');
            for &v in &p.verts[..p.vert_count as usize] {
                write!(o, " {}", base + v as usize).unwrap();
            }
            o.push('\n');
        }
        base += t.verts.len() / 3;
    }
    std::fs::write(out_path, o).map_err(|source| Error::Io { context: format!("cannot create {out_path}"), source })?;
    println!("wrote {out_path}");
    Ok(ExitCode::SUCCESS)
}

pub fn render(path: &str, out: &str, [min_x, min_z, max_x, max_z]: [f32; 4], pixels_per_meter: f32) -> CmdResult {
    let o = PreviewOptions { min_x, min_z, max_x, max_z, pixels_per_meter, ..Default::default() };
    let image = render_area(&NavMeshReader::open(path)?, &o)?;
    image.write_png(out)?;
    if json() {
        emit(json!({ "event": "render", "output": out, "width": image.width(), "height": image.height() }));
    } else {
        println!("wrote {out}");
    }
    Ok(ExitCode::SUCCESS)
}

pub fn verify(reference: &str, candidate: &str, ignore_order: bool) -> CmdResult {
    let r = verify_tiles(&NavMeshFile::read(reference)?, &NavMeshFile::read(candidate)?, ignore_order);
    let code = if r.different.is_empty() { ExitCode::SUCCESS } else { ExitCode::FAILURE };
    if json() {
        let different: Vec<Value> = r.different.iter().map(|&(x, z)| json!([x, z])).collect();
        emit(json!({
            "event": "verify", "compared": r.compared, "identical": r.identical,
            "ignoredIsolated": r.ignored_isolated, "ignoredOrder": r.ignored_order, "different": different,
        }));
        return Ok(code);
    }
    println!(
        "{} of {} common tiles identical (ignoring links, snapped endpoints, obstacle Y, door state{}{})",
        r.identical,
        r.compared,
        if r.ignored_isolated { ", isolated flag: the reference has none" } else { "" },
        if r.ignored_order { ", object order" } else { "" }
    );
    for (x, z) in r.different.iter().take(20) {
        println!("  differs: tile ({x},{z})");
    }
    if r.different.len() > 20 {
        println!("  ... and {} more", r.different.len() - 20);
    }
    Ok(code)
}

pub fn diff(path_a: &str, path_b: &str, top: i32) -> CmdResult {
    let (a, b) = (NavMeshFile::read(path_a)?, NavMeshFile::read(path_b)?);
    let r = compare_navmeshes(&a, &b);
    if !json() {
        println!("tiles compared {}, only in A {}, only in B {}", r.tiles.len(), r.only_in_a, r.only_in_b);
    }
    let (mut polys, mut off, mut obs, mut parts) = ([0i64; 2], [0i64; 2], [0i64; 2], [0i64; 2]);
    let (mut same_cell, mut delta, mut total) = (0, 0.0, 0.0);
    for d in &r.tiles {
        for (k, s) in [&d.a, &d.b].into_iter().enumerate() {
            polys[k] += i64::from(s.polys);
            off[k] += i64::from(s.off_mesh);
            obs[k] += s.obstacles as i64;
            parts[k] += s.parts as i64;
        }
        same_cell += usize::from(d.a.cell_size == d.b.cell_size);
        delta += d.area_delta;
        total += d.a.walkable_area().max(d.b.walkable_area());
    }
    let ids: std::collections::BTreeSet<u8> = r.total_area_a.keys().chain(r.total_area_b.keys()).copied().collect();
    let mut worst: Vec<_> = r.tiles.iter().collect();
    worst.sort_by(|x, y| y.area_delta.partial_cmp(&x.area_delta).unwrap_or(std::cmp::Ordering::Equal));
    let worst = &worst[..(top.max(0) as usize).min(worst.len())];
    let mismatch = if total > 0.0 { 100.0 * delta / total } else { 0.0 };
    let area = |m: &BTreeMap<u8, f64>, id: u8| m.get(&id).copied().unwrap_or(0.0);
    if json() {
        let area_m2: Map<String, Value> = ids
            .iter()
            .map(|&id| (id.to_string(), json!([area(&r.total_area_a, id), area(&r.total_area_b, id)])))
            .collect();
        let worst: Vec<Value> = worst
            .iter()
            .map(|d| {
                json!({
                    "x": d.a.x, "z": d.a.z, "areaDelta": d.area_delta, "polys": [d.a.polys, d.b.polys],
                    "links": [d.a.off_mesh, d.b.off_mesh], "cellSize": [d.a.cell_size, d.b.cell_size],
                })
            })
            .collect();
        emit(json!({
            "event": "diff", "compared": r.tiles.len(), "onlyInA": r.only_in_a, "onlyInB": r.only_in_b,
            "sameCellSize": same_cell,
            "a": { "polys": polys[0], "links": off[0], "obstacles": obs[0], "doorParts": parts[0] },
            "b": { "polys": polys[1], "links": off[1], "obstacles": obs[1], "doorParts": parts[1] },
            "areaM2": area_m2, "mismatchPercent": mismatch, "worst": worst,
        }));
        return Ok(ExitCode::SUCCESS);
    }
    println!("{:<12} {:>14} {:>14}", "", "A", "B");
    println!("{:<12} {:>14} {:>14}", "polys", polys[0], polys[1]);
    println!("{:<12} {:>14} {:>14}", "off-mesh", off[0], off[1]);
    println!("{:<12} {:>14} {:>14}", "obstacles", obs[0], obs[1]);
    println!("{:<12} {:>14} {:>14}", "door parts", parts[0], parts[1]);
    println!("same cell size: {same_cell} of {} tiles", r.tiles.len());
    println!("area by type (km^2):");
    for id in ids {
        println!("  area {id:2} {:12.4} {:12.4}", area(&r.total_area_a, id) / 1e6, area(&r.total_area_b, id) / 1e6);
    }
    println!("area mismatch: {mismatch:.3}% of walkable area");
    for d in worst {
        println!(
            "  tile ({},{}): area delta {:.1} m^2, polys {}/{}, links {}/{}, cell {:.2}/{:.2}",
            d.a.x, d.a.z, d.area_delta, d.a.polys, d.b.polys, d.a.off_mesh, d.b.off_mesh, d.a.cell_size, d.b.cell_size
        );
    }
    Ok(ExitCode::SUCCESS)
}
