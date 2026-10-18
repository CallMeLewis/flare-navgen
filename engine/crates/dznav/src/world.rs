//! Generation input as served by DayZDiag's navmesh data server (`-startNavmeshDataServer`): world parameters and
//! per-tile triangle soup. See docs/protocol/data-server.md.

use bytemuck::{Pod, Zeroable};

use crate::bytes::{Cursor, put, put_slice};
use crate::error::{Error, Result, wire_err};

/// Tile cell resolution class. Cell size = `tile_width / cells`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
pub struct CellClass {
    pub cells: i32,
    pub unk04: u32,
    pub tile_width: f32,
    pub unk0c: u32,
}

/// One GenParams/Links class (off-mesh jump/climb link type), in config order.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
pub struct LinkClass {
    /// Stored as the off-mesh connection's user id.
    pub index: u32,
    pub name_hash: u32,
    /// -1 when not set.
    pub type_id: i32,
    pub jump_height: f32,
    pub jump_length: f32,
    pub jump_dropdown_min: f32,
    pub jump_dropdown_max: f32,
    pub min_center_height: f32,
    pub agent_height: f32,
    pub max_step_height: f32,
    pub agent_radius: f32,
    pub flags: u16,
    pub area: u8,
    pub pad: u8,
    pub color: i32,
}

/// The fixed 0x54-byte block of world parameters (wire order).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
pub struct WorldParamsBlock {
    pub world_size_x: f32,
    pub world_size_z: f32,
    pub tile_width: f32,
    pub tile_height: f32,
    pub tiles_x: i32,
    pub tiles_z: i32,
    /// Top of the waterdeepsea box (area 5).
    pub deep_sea_height: f32,
    /// Depth at which water becomes waterdeep (area 3).
    pub deep_water_depth: f32,
    /// Metres.
    pub contour_max_error: f32,
    /// Extract area-10 parts as cylinder obstacles.
    pub obstacles: u8,
    pub pad25: [u8; 3],
    /// Scheduler block size in tiles.
    pub block_size: i32,
    pub filter_isolated_islands: u8,
    pub pad2d: [u8; 3],
    pub seed_position: [f32; 3],
    // GenParams/Agent
    pub stand_height: f32,
    pub crouch_height: f32,
    pub prone_height: f32,
    pub diameter: f32,
    pub max_step_height: f32,
    /// Degrees.
    pub max_slope: f32,
}

const _: () = {
    assert!(size_of::<CellClass>() == 16);
    assert!(size_of::<LinkClass>() == 0x34);
    assert!(size_of::<WorldParamsBlock>() == 0x54);
};

#[derive(Clone, Debug, Default, PartialEq)]
pub struct WorldParams {
    pub block: WorldParamsBlock,
    pub cell_classes: Vec<CellClass>,
    pub links: Vec<LinkClass>,
}

/// Area ids used by the data server for input geometry groups (a superset of navmesh areas).
pub mod area {
    pub const TERRAIN: u32 = 1;
    pub const WATER: u32 = 2;
    pub const WATER_DEEP: u32 = 3;
    pub const WATER_SEA: u32 = 4;
    pub const WATER_DEEP_SEA: u32 = 5;
    pub const BUILDING: u32 = 8;
    /// Tree trunks etc.; per part.
    pub const OBSTACLE: u32 = 10;
    pub const DOOR_OPENED: u32 = 12;
    pub const DOOR_CLOSED: u32 = 13;
    /// Vertex pairs, not triangles.
    pub const LADDER: u32 = 15;
}

/// One object part of a group. The layout matches the C++ core's `DznInputPart`, so parts pass to it as they are.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Pod, Zeroable)]
pub struct InputPart {
    /// 2 * k + state in the .nm.
    pub part_index: u32,
    pub first_vertex: u32,
    pub object_id: u64,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct InputGroup {
    pub area: u32,
    /// Partition the vertex array.
    pub parts: Vec<InputPart>,
    /// xyz triangle soup (or segment pairs for ladders).
    pub verts: Vec<f32>,
}

impl InputGroup {
    pub fn vertex_count(&self) -> usize {
        self.verts.len() / 3
    }

    /// Vertex range of part `i`.
    pub fn part_range(&self, i: usize) -> std::ops::Range<u32> {
        let first = self.parts[i].first_vertex;
        let end = self.parts.get(i + 1).map_or(self.vertex_count() as u32, |p| p.first_vertex);
        first..end
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct TileInput {
    pub x: i32,
    pub z: i32,
    pub cell_class: u32,
    pub bmin: [f32; 3],
    pub bmax: [f32; 3],
    pub groups: Vec<InputGroup>,
}

// Upper bounds that reject corrupt or hostile data instead of allocating gigabytes.
const MAX_CELL_CLASSES: u32 = 64;
const MAX_LINKS: u32 = 256;
const MAX_GROUPS: u32 = 1 << 16;
const MAX_PARTS: u32 = 1 << 20;
const MAX_VERTS: u32 = 1 << 26;

fn cursor(body: &[u8]) -> Cursor<'_> {
    Cursor::new(body, Error::Wire).truncated_message("message truncated")
}

/// False for NaN too.
fn positive(v: f32) -> bool {
    v > 0.0
}

fn read_count(r: &mut Cursor, max: u32, what: &str) -> Result<usize> {
    let n: u32 = r.read()?;
    if n > max {
        return wire_err(format!("implausible {what} count {n}"));
    }
    Ok(n as usize)
}

impl WorldParams {
    /// Parses a world params message body (id 4, the payload after the protocol version).
    pub fn parse(body: &[u8]) -> Result<WorldParams> {
        let mut r = cursor(body);
        let block: WorldParamsBlock = r.read()?;
        let n = read_count(&mut r, MAX_CELL_CLASSES, "cell class")?;
        let cell_classes: Vec<CellClass> = r.read_vec(n)?;
        let n = read_count(&mut r, MAX_LINKS, "link")?;
        let links = r.read_vec(n)?;
        if r.remaining() != 0 {
            return wire_err("unexpected bytes after world params");
        }

        let b = &block;
        if b.tiles_x <= 0 || b.tiles_z <= 0 || i64::from(b.tiles_x) * i64::from(b.tiles_z) > 1 << 28 {
            return wire_err("invalid tile grid");
        }
        if !positive(b.tile_width) || !positive(b.tile_height) {
            return wire_err("invalid tile size");
        }
        if cell_classes.iter().any(|c| c.cells <= 0 || !positive(c.tile_width)) {
            return wire_err("invalid cell class");
        }
        Ok(WorldParams { block, cell_classes, links })
    }

    pub fn serialize(&self) -> Vec<u8> {
        let mut out = Vec::new();
        put(&mut out, &self.block);
        put(&mut out, &(self.cell_classes.len() as u32));
        put_slice(&mut out, &self.cell_classes);
        put(&mut out, &(self.links.len() as u32));
        put_slice(&mut out, &self.links);
        out
    }
}

impl TileInput {
    /// Parses a tile message body (id 5), which must hold exactly one tile.
    pub fn parse(body: &[u8]) -> Result<TileInput> {
        let (tile, consumed) = Self::parse_prefix(body)?;
        if consumed != body.len() {
            return wire_err("unexpected bytes after tile record");
        }
        Ok(tile)
    }

    /// Parses a tile at the start of `body` and returns it with the number of bytes it took.
    pub fn parse_prefix(body: &[u8]) -> Result<(TileInput, usize)> {
        let mut r = cursor(body);
        let mut t = TileInput { x: r.read()?, z: r.read()?, cell_class: r.read()?, ..Default::default() };
        let groups = read_count(&mut r, MAX_GROUPS, "group")?;
        t.bmin = r.read()?;
        t.bmax = r.read()?;
        t.groups.reserve(groups);
        for _ in 0..groups {
            let area: u32 = r.read()?;
            let n = read_count(&mut r, MAX_PARTS, "part")?;
            let mut parts = Vec::with_capacity(n);
            for _ in 0..n {
                let part_index: u32 = r.read()?;
                let object_id: u64 = r.read()?;
                let first_vertex: u32 = r.read()?;
                parts.push(InputPart { part_index, first_vertex, object_id });
            }
            let n_verts: i32 = r.read()?;
            if n_verts < 0 || n_verts as u32 > MAX_VERTS {
                return wire_err("implausible vertex count");
            }
            let g = InputGroup { area, parts, verts: r.read_vec(n_verts as usize * 3)? };
            for i in 0..g.parts.len() {
                let range = g.part_range(i);
                if range.start > range.end || range.end > n_verts as u32 {
                    return wire_err("part vertex range out of bounds");
                }
            }
            if g.verts.iter().any(|v| !v.is_finite()) {
                return wire_err("non-finite vertex");
            }
            t.groups.push(g);
        }
        Ok((t, r.pos()))
    }

    pub fn serialize(&self) -> Vec<u8> {
        let mut out = Vec::new();
        put(&mut out, &self.x);
        put(&mut out, &self.z);
        put(&mut out, &self.cell_class);
        put(&mut out, &(self.groups.len() as u32));
        put(&mut out, &self.bmin);
        put(&mut out, &self.bmax);
        for g in &self.groups {
            put(&mut out, &g.area);
            put(&mut out, &(g.parts.len() as u32));
            for p in &g.parts {
                put(&mut out, &p.part_index);
                put(&mut out, &p.object_id);
                put(&mut out, &p.first_vertex);
            }
            put(&mut out, &(g.vertex_count() as i32));
            put_slice(&mut out, &g.verts);
        }
        out
    }
}
