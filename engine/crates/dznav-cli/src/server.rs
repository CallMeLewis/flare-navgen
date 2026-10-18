//! Data-server commands: probe, capture, capture-info, replay-server.

use std::collections::HashMap;
use std::io::Write as _;
use std::process::ExitCode;
use std::time::Instant;

use dznav::net::{DATA_SERVER_PORT, DataServerClient};
use dznav::replay::{ReplayOptions, serve_capture};
use dznav::{Capture, CaptureWriter, Error, TileInput, WorldParams};
use serde_json::{Value, json};

use crate::{CmdResult, atof, atoi, bad_args, cancel_flag, emit, json, stop_on_ctrl_c};

pub fn print_world_params(p: &WorldParams) {
    let b = &p.block;
    println!(
        "world {:.1} x {:.1} m, tiles {} x {} of {:.1} x {:.1} m",
        b.world_size_x, b.world_size_z, b.tiles_x, b.tiles_z, b.tile_width, b.tile_height
    );
    println!(
        "deep sea height {:.2}, deep water depth {:.2}, contour error {:.3}, obstacles {}, block {}",
        b.deep_sea_height, b.deep_water_depth, b.contour_max_error, b.obstacles, b.block_size
    );
    let s = b.seed_position;
    println!("filter islands {}, seed ({:.1}, {:.1}, {:.1})", b.filter_isolated_islands, s[0], s[1], s[2]);
    println!(
        "agent: stand {:.2} crouch {:.2} prone {:.2} diameter {:.2} step {:.2} slope {:.1}",
        b.stand_height, b.crouch_height, b.prone_height, b.diameter, b.max_step_height, b.max_slope
    );
    for (i, c) in p.cell_classes.iter().enumerate() {
        println!(
            "cell class {i}: {} cells over {:.2} m (cs {:.3}) [unk {} {}]",
            c.cells,
            c.tile_width,
            c.tile_width / c.cells as f32,
            c.unk04,
            c.unk0c
        );
    }
    for l in &p.links {
        println!(
            "link {}: typeId {} len {:.2} height {:.2} centre {:.2} dropdown {:.2}..{:.2} agent h {:.2} step {:.2} r {:.2} flags {:#x} area {}",
            l.index,
            l.type_id,
            l.jump_length,
            l.jump_height,
            l.min_center_height,
            l.jump_dropdown_min,
            l.jump_dropdown_max,
            l.agent_height,
            l.max_step_height,
            l.agent_radius,
            l.flags,
            l.area
        );
    }
}

fn print_tile_input(t: &TileInput) {
    println!(
        "tile ({},{}) cell class {} bounds ({:.2} {:.2} {:.2})-({:.2} {:.2} {:.2}), {} groups",
        t.x,
        t.z,
        t.cell_class,
        t.bmin[0],
        t.bmin[1],
        t.bmin[2],
        t.bmax[0],
        t.bmax[1],
        t.bmax[2],
        t.groups.len()
    );
    for g in &t.groups {
        println!("  area {:2}: {} parts, {} verts", g.area, g.parts.len(), g.vertex_count());
    }
}

pub fn world_params_json(p: &WorldParams) -> Value {
    let b = &p.block;
    let cell_classes: Vec<Value> = p
        .cell_classes
        .iter()
        .map(|c| json!({ "cells": c.cells, "width": c.tile_width, "cellSize": c.tile_width / c.cells as f32 }))
        .collect();
    let link_classes: Vec<Value> = p
        .links
        .iter()
        .map(|l| {
            json!({
                "index": l.index, "typeId": l.type_id, "jumpLength": l.jump_length, "jumpHeight": l.jump_height,
                "minCenterHeight": l.min_center_height, "dropdownMin": l.jump_dropdown_min,
                "dropdownMax": l.jump_dropdown_max, "agentHeight": l.agent_height, "maxStepHeight": l.max_step_height,
                "agentRadius": l.agent_radius, "flags": l.flags, "area": l.area,
            })
        })
        .collect();
    json!({
        "worldSizeX": b.world_size_x, "worldSizeZ": b.world_size_z, "tilesX": b.tiles_x, "tilesZ": b.tiles_z,
        "tileWidth": b.tile_width, "tileHeight": b.tile_height, "seaLevel": b.deep_sea_height,
        "deepWaterDepth": b.deep_water_depth, "contourMaxError": b.contour_max_error, "obstacles": b.obstacles != 0,
        "blockSize": b.block_size, "filterIsolatedIslands": b.filter_isolated_islands != 0, "seed": b.seed_position,
        "agent": {
            "standHeight": b.stand_height, "crouchHeight": b.crouch_height, "proneHeight": b.prone_height,
            "diameter": b.diameter, "maxStepHeight": b.max_step_height, "maxSlope": b.max_slope,
        },
        "cellClasses": cell_classes, "linkClasses": link_classes,
    })
}

pub fn probe(host: &str, tile: Option<(i32, i32)>) -> CmdResult {
    let mut client = DataServerClient::new(host, DATA_SERVER_PORT);
    let p = client.request_world_params()?;
    if !json() {
        print_world_params(&p);
        if let Some((x, z)) = tile {
            print_tile_input(&client.request_tile(x, z, &p)?);
        }
        return Ok(ExitCode::SUCCESS);
    }
    let mut j = json!({ "event": "world", "host": host, "params": world_params_json(&p) });
    if let Some((x, z)) = tile {
        let t = client.request_tile(x, z, &p)?;
        let groups: Vec<Value> = t
            .groups
            .iter()
            .map(|g| json!({ "area": g.area, "parts": g.parts.len(), "verts": g.vertex_count() }))
            .collect();
        j["tile"] = json!({
            "x": t.x, "z": t.z, "cellClass": t.cell_class, "bmin": t.bmin, "bmax": t.bmax, "groups": groups,
        });
    }
    emit(j);
    Ok(ExitCode::SUCCESS)
}

pub fn capture(out: &str, [x0, z0, x1, z1]: [i32; 4], host: &str) -> CmdResult {
    let mut client = DataServerClient::new(host, DATA_SERVER_PORT);
    let p = client.request_world_params()?;
    let mut w = CaptureWriter::create(out, &p)?;
    let t0 = Instant::now();
    let total = (x1 - x0 + 1) * (z1 - z0 + 1);
    let mut n = 0;
    for z in z0..=z1 {
        for x in x0..=x1 {
            w.add(&client.request_tile(x, z, &p)?)?;
            n += 1;
            let s = t0.elapsed().as_secs_f64();
            let rate = f64::from(n) / s.max(1e-3);
            if json() {
                emit(json!({ "event": "progress", "done": n, "total": total, "elapsed": s, "rate": rate }));
            } else {
                print!("\rcaptured {n}/{total} tiles ({rate:.1} tiles/s)");
                let _ = std::io::stdout().flush();
            }
        }
    }
    if json() {
        emit(json!({ "event": "done", "output": out, "tiles": n }));
    } else {
        println!("\nwrote {out}");
    }
    Ok(ExitCode::SUCCESS)
}

pub fn capture_info(path: &str) -> CmdResult {
    let c = Capture::read(path)?;
    print_world_params(&c.params);
    println!("{} tiles", c.tiles.len());
    for t in c.tiles.iter().take(5) {
        print_tile_input(t);
    }
    Ok(ExitCode::SUCCESS)
}

pub fn replay_server(args: &[&str]) -> CmdResult {
    let mut o = ReplayOptions::default();
    let mut capture = Capture::default();
    let mut index: HashMap<(i32, i32), usize> = HashMap::new();
    let mut it = args.iter();
    while let Some(&a) = it.next() {
        let mut next = || it.next().copied().ok_or_else(|| bad_args(format!("missing value for {a}")));
        match a {
            "--bind" => o.address = next()?.to_string(),
            "--port" => o.port = atoi(next()?) as u16,
            "--crop" => o.crop = true,
            "--sea-level" => o.sea_level = Some(atof(next()?) as f32),
            "--no-island-filter" => o.island_filter = false,
            "--once" => o.exit_after_client = true,
            _ if a.starts_with("--") => return Err(bad_args(format!("unknown option {a}"))),
            path => {
                let c = Capture::read(path)?;
                if index.is_empty() {
                    capture.params = c.params;
                }
                // A tile in several captures is served from the last one.
                for t in c.tiles {
                    match index.get(&(t.x, t.z)) {
                        Some(&i) => capture.tiles[i] = t,
                        None => {
                            index.insert((t.x, t.z), capture.tiles.len());
                            capture.tiles.push(t);
                        }
                    }
                }
            }
        }
    }
    if index.is_empty() {
        return Err(Error::Format("no capture given".into()).into());
    }
    stop_on_ctrl_c();
    let st = serve_capture(&capture, &o, Some(cancel_flag()), &|line| println!("{line}"))?;
    println!("served {} clients: {} captured tiles, {} empty", st.clients, st.captured, st.empty);
    Ok(ExitCode::SUCCESS)
}
