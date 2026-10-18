//! Shared test helpers: a small synthetic world, and the optional external inputs (shipped navmeshes, the C++ build).
#![allow(dead_code)]

use std::path::PathBuf;

use dznav::world::{CellClass, InputGroup, InputPart, LinkClass, TileInput, WorldParams, WorldParamsBlock, area};

pub const TILES: i32 = 3;
pub const TILE_SIZE: f32 = 50.0;
/// The tile with the buildings, at the finer cell class.
pub const TOWN: (i32, i32) = (1, 1);

/// A 3 x 3 tile world with agent and link settings like a shipped map's.
pub fn world_params() -> WorldParams {
    let block = WorldParamsBlock {
        world_size_x: TILES as f32 * TILE_SIZE,
        world_size_z: TILES as f32 * TILE_SIZE,
        tile_width: TILE_SIZE,
        tile_height: TILE_SIZE,
        tiles_x: TILES,
        tiles_z: TILES,
        deep_sea_height: 0.75,
        deep_water_depth: 1.5,
        contour_max_error: 0.3,
        obstacles: 1,
        block_size: 4,
        filter_isolated_islands: 1,
        seed_position: [60.0, 10.0, 60.0],
        stand_height: 1.5,
        crouch_height: 1.0,
        prone_height: 0.5,
        diameter: 0.6,
        max_step_height: 0.45,
        max_slope: 45.0,
        ..Default::default()
    };
    let cell_classes = vec![
        CellClass { cells: 200, unk04: 200, tile_width: TILE_SIZE, unk0c: 0 },
        CellClass { cells: 500, unk04: 500, tile_width: TILE_SIZE, unk0c: 0 },
    ];
    let links = vec![LinkClass {
        index: 0,
        name_hash: 0x1234_5678,
        type_id: -1,
        jump_height: 1.2,
        jump_length: 1.8,
        jump_dropdown_min: 0.0,
        jump_dropdown_max: 0.0,
        min_center_height: 0.4,
        agent_height: 1.5,
        max_step_height: 0.45,
        agent_radius: 0.3,
        flags: 0x100,
        area: 48,
        pad: 0,
        color: -1,
    }];
    WorldParams { block, cell_classes, links }
}

fn ground(x: f32, z: f32) -> f32 {
    // Rolling ground at about 10 m, with a pond hollow around (30, 105).
    let pond = ((x - 30.0).powi(2) + (z - 105.0).powi(2)).sqrt();
    10.0 + 1.5 * (x * 0.05).sin() * (z * 0.07).cos() - if pond < 12.0 { 3.0 * (1.0 - pond / 12.0) } else { 0.0 }
}

/// Appends the triangle (a, b, c) in DayZ's winding.
fn tri(v: &mut Vec<f32>, a: [f32; 3], b: [f32; 3], c: [f32; 3]) {
    v.extend(a);
    v.extend(b);
    v.extend(c);
}

/// A horizontal-ish quad from corners at (x0, z0), (x1, z0), (x0, z1), (x1, z1), facing up.
fn quad(v: &mut Vec<f32>, p00: [f32; 3], p10: [f32; 3], p01: [f32; 3], p11: [f32; 3]) {
    tri(v, p00, p10, p01);
    tri(v, p10, p11, p01);
}

/// An axis-aligned box, every face facing out.
fn cuboid(v: &mut Vec<f32>, min: [f32; 3], max: [f32; 3]) {
    let p = |x: usize, y: usize, z: usize| [[min[0], max[0]][x], [min[1], max[1]][y], [min[2], max[2]][z]];
    quad(v, p(0, 1, 0), p(1, 1, 0), p(0, 1, 1), p(1, 1, 1)); // top
    quad(v, p(0, 0, 0), p(0, 0, 1), p(1, 0, 0), p(1, 0, 1)); // bottom
    quad(v, p(0, 0, 0), p(1, 0, 0), p(0, 1, 0), p(1, 1, 0)); // -z
    quad(v, p(1, 0, 1), p(0, 0, 1), p(1, 1, 1), p(0, 1, 1)); // +z
    quad(v, p(0, 0, 1), p(0, 0, 0), p(0, 1, 1), p(0, 1, 0)); // -x
    quad(v, p(1, 0, 0), p(1, 0, 1), p(1, 1, 0), p(1, 1, 1)); // +x
}

/// A six-sided prism standing on the ground.
fn trunk(v: &mut Vec<f32>, cx: f32, cz: f32, radius: f32, height: f32) {
    let y0 = ground(cx, cz) - 0.5;
    let at = |k: usize, y: f32| {
        let a = k as f32 * std::f32::consts::TAU / 6.0;
        [cx + radius * a.cos(), y, cz + radius * a.sin()]
    };
    for k in 0..6 {
        quad(v, at(k + 1, y0), at(k, y0), at(k + 1, y0 + height), at(k, y0 + height));
    }
}

fn group(area: u32, verts: Vec<f32>) -> InputGroup {
    InputGroup { area, parts: Vec::new(), verts }
}

/// A group whose parts are the given vertex lists.
fn parts_group(area: u32, parts: Vec<(u32, u64, Vec<f32>)>) -> InputGroup {
    let mut g = group(area, Vec::new());
    for (part_index, object_id, verts) in parts {
        g.parts.push(InputPart { part_index, first_vertex: g.vertex_count() as u32, object_id });
        g.verts.extend(verts);
    }
    g
}

/// The input of tile (x, z), as a data server would send it: geometry around the tile, 10 m beyond its edges.
pub fn tile_input(x: i32, z: i32) -> TileInput {
    let (x0, z0) = (x as f32 * TILE_SIZE, z as f32 * TILE_SIZE);
    let mut groups = Vec::new();

    let mut terrain = Vec::new();
    let step = 2.0;
    let n = ((TILE_SIZE + 20.0) / step) as i32;
    for i in 0..n {
        for k in 0..n {
            let (ax, az) = (x0 - 10.0 + i as f32 * step, z0 - 10.0 + k as f32 * step);
            let (bx, bz) = (ax + step, az + step);
            let p = |px: f32, pz: f32| [px, ground(px, pz), pz];
            quad(&mut terrain, p(ax, az), p(bx, az), p(ax, bz), p(bx, bz));
        }
    }
    groups.push(group(area::TERRAIN, terrain));

    let mut pond = Vec::new();
    let w = 9.5;
    quad(
        &mut pond,
        [30.0 - w, 9.6, 105.0 - w],
        [30.0 + w, 9.6, 105.0 - w],
        [30.0 - w, 9.6, 105.0 + w],
        [30.0 + w, 9.6, 105.0 + w],
    );
    groups.push(group(area::WATER, pond));

    // A crate to climb by ladder, and a low wall to jump.
    let mut objects = Vec::new();
    cuboid(&mut objects, [64.0, 8.0, 64.0], [70.0, 12.5, 70.0]);
    cuboid(&mut objects, [55.0, 8.0, 80.0], [65.0, 10.9, 80.3]);
    groups.push(group(9, objects));

    // A room (walls only) with a doorway in its south wall.
    let mut walls = Vec::new();
    let (rx0, rz0, rx1, rz1, floor, top) = (80.0, 60.0, 90.0, 68.0, 9.0, 13.0);
    cuboid(&mut walls, [rx0, floor, rz0], [84.5, top, rz0 + 0.2]);
    cuboid(&mut walls, [85.5, floor, rz0], [rx1, top, rz0 + 0.2]);
    cuboid(&mut walls, [rx0, floor, rz1 - 0.2], [rx1, top, rz1]);
    cuboid(&mut walls, [rx0, floor, rz0], [rx0 + 0.2, top, rz1]);
    cuboid(&mut walls, [rx1 - 0.2, floor, rz0], [rx1, top, rz1]);
    let mut room_floor = Vec::new();
    quad(&mut room_floor, [rx0, 10.6, rz0], [rx1, 10.6, rz0], [rx0, 10.6, rz1], [rx1, 10.6, rz1]);
    walls.extend(room_floor);
    groups.push(group(area::BUILDING, walls));

    let door_id = 0x8000_f000_0501_f61f;
    let mut closed = Vec::new();
    cuboid(&mut closed, [84.5, 10.6, rz0], [85.5, 12.6, rz0 + 0.1]);
    let mut open = Vec::new();
    cuboid(&mut open, [84.5, 10.6, rz0], [84.6, 12.6, rz0 + 1.0]);
    groups.push(parts_group(area::DOOR_OPENED, vec![(0, door_id, open)]));
    groups.push(parts_group(area::DOOR_CLOSED, vec![(1, door_id, closed)]));

    let mut trees = Vec::new();
    for (i, (tx, tz)) in
        [(20.0, 20.0), (40.0, 70.0), (110.0, 30.0), (120.0, 120.0), (75.0, 130.0)].into_iter().enumerate()
    {
        let mut v = Vec::new();
        trunk(&mut v, tx, tz, 0.35, 6.0);
        trees.push((0, 0x100 + i as u64, v));
    }
    groups.push(parts_group(area::OBSTACLE, trees));

    // A ladder up the crate's west face: one bottom-top vertex pair.
    groups.push(group(area::LADDER, vec![63.8, ground(63.8, 67.0), 67.0, 63.8, 12.5, 67.0]));

    let mut t = TileInput {
        x,
        z,
        cell_class: if (x, z) == TOWN { 1 } else { 0 },
        bmin: [x0, f32::MAX, z0],
        bmax: [x0 + TILE_SIZE, f32::MIN, z0 + TILE_SIZE],
        groups,
    };
    for g in &t.groups {
        for y in g.verts.iter().skip(1).step_by(3) {
            t.bmin[1] = t.bmin[1].min(*y);
            t.bmax[1] = t.bmax[1].max(*y);
        }
    }
    t
}

/// Every tile, in the generator's order for this world (one block, row by row).
pub fn tiles() -> Vec<TileInput> {
    (0..TILES).flat_map(|z| (0..TILES).map(move |x| tile_input(x, z))).collect()
}

/// A fresh, empty directory for one test's files.
pub fn scratch_dir(name: &str) -> PathBuf {
    let dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

/// Shipped navmeshes to test against, from `DZNAV_SAMPLE_NM`: a list of .nm files and folders of them (separated
/// like PATH).
pub fn sample_navmeshes() -> Vec<PathBuf> {
    let Some(list) = std::env::var_os("DZNAV_SAMPLE_NM") else {
        eprintln!("DZNAV_SAMPLE_NM is not set; skipping");
        return Vec::new();
    };
    let mut files = Vec::new();
    for path in std::env::split_paths(&list) {
        if path.is_dir() {
            let mut found: Vec<PathBuf> = std::fs::read_dir(&path)
                .unwrap()
                .map(|e| e.unwrap().path())
                .filter(|p| p.extension().is_some_and(|e| e == "nm"))
                .collect();
            found.sort();
            files.extend(found);
        } else {
            files.push(path);
        }
    }
    files
}

/// The C++ dznav binary to compare against, from `DZNAV_ORACLE`.
pub fn oracle() -> Option<PathBuf> {
    let path = std::env::var_os("DZNAV_ORACLE").map(PathBuf::from);
    if path.is_none() {
        eprintln!("DZNAV_ORACLE is not set; skipping");
    }
    path
}
