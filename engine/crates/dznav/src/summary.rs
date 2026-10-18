//! What Inspect shows: a navmesh's totals with metrics per tile, and one tile's details. They serialise as the `info`
//! and `tileDetail` events of `dznav --json` (docs/cli-json.md), which is also how the app receives them.

use std::collections::BTreeMap;

use serde::Serialize;

use crate::error::Result;
use crate::format::Tile;
use crate::reader::NavMeshReader;

#[derive(Clone, Copy, Debug, PartialEq, Serialize)]
pub struct Agent {
    pub height: f32,
    pub radius: f32,
    pub climb: f32,
}

/// `[x, z, bytes, polys, links, obstacles, door parts]`
pub type TileMetrics = (i32, i32, usize, usize, usize, usize, usize);

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(tag = "event", rename = "info", rename_all = "camelCase")]
pub struct NavMeshInfo {
    pub path: String,
    pub tiles: usize,
    pub tiles_x: i32,
    pub tiles_z: i32,
    pub tile_width: f32,
    pub tile_height: f32,
    pub origin: [f32; 3],
    pub max_tiles: i32,
    pub max_polys: i32,
    pub polys: usize,
    pub verts: usize,
    pub links: usize,
    pub door_parts: usize,
    pub door_polys: usize,
    pub obstacles: usize,
    #[serde(skip)]
    pub detail_meshes: usize,
    /// From the first tile.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub agent: Option<Agent>,
    /// `[cell size, tiles]`, by cell size.
    pub cell_sizes: Vec<(f32, usize)>,
    /// Polys per area id.
    pub areas: BTreeMap<u8, usize>,
    /// Polys per flags value.
    pub flags: BTreeMap<u16, usize>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub tile_list: Option<Vec<TileMetrics>>,
}

/// Totals over every tile, and with `per_tile` the metrics of each. Reads the whole file.
pub fn navmesh_info<B: AsRef<[u8]>>(reader: &NavMeshReader<B>, path: &str, per_tile: bool) -> Result<NavMeshInfo> {
    let h = reader.header();
    let mut info = NavMeshInfo {
        path: path.to_string(),
        tiles: reader.tiles().len(),
        tiles_x: h.tiles_x,
        tiles_z: h.tiles_z,
        tile_width: h.tile_width,
        tile_height: h.tile_height,
        origin: h.orig,
        max_tiles: h.max_tiles,
        max_polys: h.max_polys,
        polys: 0,
        verts: 0,
        links: 0,
        door_parts: 0,
        door_polys: 0,
        obstacles: 0,
        detail_meshes: 0,
        agent: reader.tiles().first().map(|e| Agent {
            height: e.header.walkable_height,
            radius: e.header.walkable_radius,
            climb: e.header.walkable_climb,
        }),
        cell_sizes: Vec::new(),
        areas: BTreeMap::new(),
        flags: BTreeMap::new(),
        tile_list: per_tile.then(Vec::new),
    };
    // Keyed by the float's bits, which order like the floats for positive sizes.
    let mut cell_sizes: BTreeMap<u32, usize> = BTreeMap::new();
    for (i, e) in reader.tiles().iter().enumerate() {
        let t = reader.tile(i)?;
        info.polys += t.polys.len();
        info.verts += t.verts.len() / 3;
        info.links += t.off_mesh_cons.len();
        info.door_parts += t.parts.len();
        info.door_polys += t.part_polys.len();
        info.obstacles += t.obstacles.len();
        info.detail_meshes += t.detail_meshes.len();
        *cell_sizes.entry((1.0 / t.header.bv_quant_factor).to_bits()).or_default() += 1;
        for p in &t.polys {
            *info.areas.entry(p.area()).or_default() += 1;
            *info.flags.entry(p.flags).or_default() += 1;
        }
        if let Some(list) = &mut info.tile_list {
            list.push((
                e.x(),
                e.z(),
                e.data_size,
                t.polys.len(),
                t.off_mesh_cons.len(),
                t.obstacles.len(),
                t.parts.len(),
            ));
        }
    }
    info.cell_sizes = cell_sizes.into_iter().map(|(bits, n)| (f32::from_bits(bits), n)).collect();
    Ok(info)
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LinkDetail {
    /// The link class (index in GenParams/Links).
    pub class: u32,
    pub bidirectional: bool,
    pub from: [f32; 3],
    pub to: [f32; 3],
    pub center_height: f32,
    pub radius: f32,
    pub area: u8,
    pub flags: u16,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DoorDetail {
    /// Hex.
    pub object_id: String,
    pub part: u32,
    /// 0 or 1.
    pub state: u32,
    pub polys: usize,
}

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(tag = "event", rename = "tileDetail", rename_all = "camelCase")]
pub struct TileDetail {
    pub x: i32,
    pub z: i32,
    /// Hex.
    #[serde(rename = "ref")]
    pub tile_ref: String,
    pub bmin: [f32; 3],
    pub bmax: [f32; 3],
    pub cell_size: f32,
    pub bytes: usize,
    /// Including off-mesh link polys.
    pub polys: i32,
    pub verts: i32,
    /// Ground polys per area id (link polys are listed under `links`).
    pub areas: BTreeMap<u8, usize>,
    /// Ground polys per flags value.
    pub flags: BTreeMap<u16, usize>,
    pub links: Vec<LinkDetail>,
    pub doors: Vec<DoorDetail>,
    /// `[x, y, z, radius]`
    pub obstacles: Vec<[f32; 4]>,
}

/// The polys of a door part, following its list in the tile's extra section.
pub fn part_polys(tile: &Tile, first_poly: i32) -> Vec<u32> {
    let mut polys = Vec::new();
    let mut k = first_poly;
    while k >= 0 {
        let Some(pp) = tile.part_polys.get(k as usize) else { break };
        polys.push(pp.poly);
        k = pp.next;
    }
    polys
}

pub fn tile_detail(t: &Tile) -> TileDetail {
    let h = &t.header;
    let mut areas = BTreeMap::new();
    let mut flags = BTreeMap::new();
    for p in t.polys.iter().filter(|p| p.poly_type() == 0) {
        *areas.entry(p.area()).or_default() += 1;
        *flags.entry(p.flags).or_default() += 1;
    }
    let links = t
        .off_mesh_cons
        .iter()
        .map(|c| {
            let p = &t.polys[c.poly as usize];
            LinkDetail {
                class: c.user_id,
                bidirectional: c.flags & 1 != 0,
                from: [c.pos[0], c.pos[1], c.pos[2]],
                to: [c.pos[3], c.pos[4], c.pos[5]],
                center_height: c.center_height,
                radius: c.rad,
                area: p.area(),
                flags: p.flags,
            }
        })
        .collect();
    let doors = t
        .parts
        .iter()
        .map(|part| DoorDetail {
            object_id: format!("0x{:016x}", part.object_id),
            part: part.part_index / 2,
            state: part.part_index % 2,
            polys: part_polys(t, part.first_poly).len(),
        })
        .collect();
    TileDetail {
        x: h.x,
        z: h.y,
        tile_ref: format!("0x{:016x}", t.tile_ref),
        bmin: h.bmin,
        bmax: h.bmax,
        cell_size: 1.0 / h.bv_quant_factor,
        bytes: t.data_size(),
        polys: h.poly_count,
        verts: h.vert_count,
        areas,
        flags,
        links,
        doors,
        obstacles: t.obstacles.iter().map(|o| [o.pos[0], o.pos[1], o.pos[2], o.radius]).collect(),
    }
}
