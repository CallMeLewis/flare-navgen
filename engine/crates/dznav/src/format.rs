//! DayZ navmesh file format (.nm), as read by the game.
//!
//! The container is the RecastDemo "tile mesh set" layout extended with a grid size and a tile offset table. Tiles are
//! Detour tile blobs built with DT_POLYREF64 (16-byte links), with a DayZ-specific tile version (12), wider off-mesh
//! connections and a trailing "extra" section holding door/part polygon lists and cylinder obstacles. See
//! docs/format/nm-format.md.
//!
//! Every record type is `#[repr(C)]` with no padding, so it reads and writes as its little-endian bytes.

use std::path::Path;

use bytemuck::{Pod, Zeroable};

use crate::bytes::{Cursor, put, put_slice};
use crate::error::{Result, format_err, io_err};

/// Bytes "TESM".
pub const SET_MAGIC: u32 = u32::from_be_bytes(*b"MSET");
/// DayZ 1.30+; the game loads no other version.
pub const SET_VERSION: i32 = 9;
/// DayZ 1.29 and earlier: the same layout, so still read (and saved as [`SET_VERSION`]).
pub const SET_VERSION_OLD: i32 = 7;
/// Bytes "VAND".
pub const TILE_MAGIC: u32 = u32::from_be_bytes(*b"DNAV");
pub const TILE_VERSION: i32 = 12;
pub const VERTS_PER_POLY: usize = 6;
pub const EXT_LINK: u16 = 0x8000;
/// Poly flag: unreachable from the island filter seed.
pub const ISOLATED_FLAG: u16 = 0x8000;
pub const NULL_LINK: u32 = 0xffff_ffff;
/// Offset table entry for a tile slot that has no tile (partial navmeshes).
pub const NO_TILE: u32 = 0xffff_ffff;

// 64-bit poly ref layout (Detour DT_POLYREF64 defaults).
pub const SALT_BITS: u32 = 16;
pub const TILE_BITS: u32 = 28;
pub const POLY_BITS: u32 = 20;

pub const fn encode_poly_ref(salt: u32, tile_index: u32, poly_index: u32) -> u64 {
    ((salt as u64) << (POLY_BITS + TILE_BITS)) | ((tile_index as u64) << POLY_BITS) | poly_index as u64
}
pub const fn ref_salt(r: u64) -> u32 {
    (r >> (POLY_BITS + TILE_BITS)) as u32 & ((1 << SALT_BITS) - 1)
}
pub const fn ref_tile(r: u64) -> u32 {
    (r >> POLY_BITS) as u32 & ((1 << TILE_BITS) - 1)
}
pub const fn ref_poly(r: u64) -> u32 {
    r as u32 & ((1 << POLY_BITS) - 1)
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
pub struct SetHeader {
    pub magic: u32,
    pub version: i32,
    pub num_tiles: i32,
    /// Grid width in tiles.
    pub tiles_x: i32,
    /// Grid height in tiles.
    pub tiles_z: i32,
    // dtNavMeshParams
    pub orig: [f32; 3],
    pub tile_width: f32,
    pub tile_height: f32,
    pub max_tiles: i32,
    pub max_polys: i32,
}

/// Per-tile record header. Matches RecastDemo's `NavMeshTileHeader { dtTileRef; int dataSize; }` with 64-bit refs,
/// including the 4 bytes of struct padding (0 in shipped maps, other values in some saves; the game ignores it).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
pub struct TileRecordHeader {
    pub tile_ref: u64,
    pub data_size: i32,
    pub pad: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
pub struct MeshHeader {
    pub magic: u32,
    pub version: i32,
    pub x: i32,
    /// Grid row; world Z = orig[2] + y * tile_height.
    pub y: i32,
    pub layer: i32,
    pub user_id: u32,
    pub poly_count: i32,
    pub vert_count: i32,
    pub max_link_count: i32,
    pub detail_mesh_count: i32,
    pub detail_vert_count: i32,
    pub detail_tri_count: i32,
    pub bv_node_count: i32,
    pub off_mesh_con_count: i32,
    pub off_mesh_base: i32,
    pub walkable_height: f32,
    pub walkable_radius: f32,
    pub walkable_climb: f32,
    pub bmin: [f32; 3],
    pub bmax: [f32; 3],
    /// 1 / cell size used for this tile.
    pub bv_quant_factor: f32,
    /// DayZ: byte size of the trailing extra section.
    pub extra_data_size: i32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
pub struct Poly {
    pub first_link: u32,
    pub verts: [u16; VERTS_PER_POLY],
    pub neis: [u16; VERTS_PER_POLY],
    pub flags: u16,
    pub vert_count: u8,
    /// Low 6 bits area, high 2 bits type (0 ground, 1 off-mesh).
    pub area_and_type: u8,
}

impl Poly {
    pub fn area(&self) -> u8 {
        self.area_and_type & 0x3f
    }
    pub fn poly_type(&self) -> u8 {
        self.area_and_type >> 6
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
pub struct Link {
    pub poly_ref: u64,
    pub next: u32,
    pub edge: u8,
    pub side: u8,
    pub bmin: u8,
    pub bmax: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
pub struct PolyDetail {
    pub vert_base: u32,
    pub tri_base: u32,
    pub vert_count: u8,
    pub tri_count: u8,
    pub pad: [u8; 2],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
pub struct BvNode {
    pub bmin: [u16; 3],
    pub bmax: [u16; 3],
    /// >= 0 leaf poly index, < 0 escape offset.
    pub i: i32,
}

/// Stock Detour is 36 bytes; DayZ has three floats after `pos`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
pub struct OffMeshConnection {
    pub pos: [f32; 6],
    /// Measured obstacle height at the link midpoint (can be < 0); not used by Detour.
    pub center_height: f32,
    /// 0 when built, set to 1.0 by addTile.
    pub reserved: f32,
    /// Endpoint snap radius.
    pub rad: f32,
    pub poly: u16,
    /// 1 = bidirectional.
    pub flags: u8,
    /// 0xff = internal, else neighbour tile side.
    pub side: u8,
    /// Index of the link class in GenParams/Links.
    pub user_id: u32,
}

/// Extra section: per-object part (door) polygon lists.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
pub struct TilePart {
    pub object_id: u64,
    /// 2 * part + state.
    pub part_index: u32,
    /// Index into `part_polys`, -1 if none.
    pub first_poly: i32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
pub struct TilePartPoly {
    pub poly: u32,
    /// Index into `part_polys`, -1 terminates.
    pub next: i32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
pub struct Obstacle {
    pub pos: [f32; 3],
    pub radius: f32,
}

const _: () = {
    assert!(size_of::<SetHeader>() == 48);
    assert!(size_of::<TileRecordHeader>() == 16);
    assert!(size_of::<MeshHeader>() == 104);
    assert!(size_of::<Poly>() == 32);
    assert!(size_of::<Link>() == 16);
    assert!(size_of::<PolyDetail>() == 12);
    assert!(size_of::<BvNode>() == 16);
    assert!(size_of::<OffMeshConnection>() == 44);
    assert!(size_of::<TilePart>() == 16);
    assert!(size_of::<TilePartPoly>() == 8);
    assert!(size_of::<Obstacle>() == 16);
};

/// A fully decoded tile. Serializing a parsed tile reproduces the original bytes.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Tile {
    pub tile_ref: u64,
    /// Record header padding, kept for exact round trips.
    pub record_pad: u32,
    pub header: MeshHeader,
    /// 3 per vertex.
    pub verts: Vec<f32>,
    pub polys: Vec<Poly>,
    pub links: Vec<Link>,
    pub detail_meshes: Vec<PolyDetail>,
    pub detail_verts: Vec<f32>,
    /// 4 per triangle.
    pub detail_tris: Vec<u8>,
    pub bv_tree: Vec<BvNode>,
    pub off_mesh_cons: Vec<OffMeshConnection>,
    pub parts: Vec<TilePart>,
    pub part_polys: Vec<TilePartPoly>,
    pub obstacles: Vec<Obstacle>,
}

fn count(n: i32, what: &str) -> Result<usize> {
    if n < 0 {
        return format_err(format!("negative {what}"));
    }
    Ok(n as usize)
}

impl Tile {
    pub fn extra_data_size(&self) -> usize {
        12 + size_of_val(&self.parts[..]) + size_of_val(&self.part_polys[..]) + size_of_val(&self.obstacles[..])
    }

    pub fn data_size(&self) -> usize {
        size_of::<MeshHeader>()
            + size_of_val(&self.verts[..])
            + size_of_val(&self.polys[..])
            + size_of_val(&self.links[..])
            + size_of_val(&self.detail_meshes[..])
            + size_of_val(&self.detail_verts[..])
            + self.detail_tris.len()
            + size_of_val(&self.bv_tree[..])
            + size_of_val(&self.off_mesh_cons[..])
            + self.extra_data_size()
    }

    /// Recomputes the header's counts and extra section size from the vectors.
    pub fn sync_header(&mut self) {
        let extra_data_size = self.extra_data_size() as i32;
        let h = &mut self.header;
        h.magic = TILE_MAGIC;
        h.version = TILE_VERSION;
        h.vert_count = (self.verts.len() / 3) as i32;
        h.poly_count = self.polys.len() as i32;
        h.max_link_count = self.links.len() as i32;
        h.detail_mesh_count = self.detail_meshes.len() as i32;
        h.detail_vert_count = (self.detail_verts.len() / 3) as i32;
        h.detail_tri_count = (self.detail_tris.len() / 4) as i32;
        h.bv_node_count = self.bv_tree.len() as i32;
        h.off_mesh_con_count = self.off_mesh_cons.len() as i32;
        h.extra_data_size = extra_data_size;
    }

    /// Parses one tile blob (the bytes after the record header).
    pub fn parse(data: &[u8], tile_ref: u64) -> Result<Tile> {
        let mut r = Cursor::new(data, crate::Error::Format);
        let header: MeshHeader = r.read()?;
        check_tile_header(&header)?;
        let h = header;
        let verts = count(h.vert_count, "vertCount")?;
        let polys = count(h.poly_count, "polyCount")?;
        let links = count(h.max_link_count, "maxLinkCount")?;
        let detail_meshes = count(h.detail_mesh_count, "detailMeshCount")?;
        let detail_verts = count(h.detail_vert_count, "detailVertCount")?;
        let detail_tris = count(h.detail_tri_count, "detailTriCount")?;
        let bv_nodes = count(h.bv_node_count, "bvNodeCount")?;
        let off_mesh_cons = count(h.off_mesh_con_count, "offMeshConCount")?;
        let extra_size = count(h.extra_data_size, "extraDataSize")?;

        let mut t = Tile { tile_ref, header, ..Default::default() };
        t.verts = r.read_vec(verts * 3)?;
        t.polys = r.read_vec(polys)?;
        t.links = r.read_vec(links)?;
        t.detail_meshes = r.read_vec(detail_meshes)?;
        t.detail_verts = r.read_vec(detail_verts * 3)?;
        t.detail_tris = r.read_vec(detail_tris * 4)?;
        t.bv_tree = r.read_vec(bv_nodes)?;
        t.off_mesh_cons = r.read_vec(off_mesh_cons)?;

        let extra_start = r.pos();
        let part_count: u32 = r.read()?;
        let part_poly_count: u32 = r.read()?;
        t.parts = r.read_vec(part_count as usize)?;
        t.part_polys = r.read_vec(part_poly_count as usize)?;
        let obstacle_count: u32 = r.read()?;
        t.obstacles = r.read_vec(obstacle_count as usize)?;
        if r.pos() - extra_start != extra_size {
            return format_err("extra section size mismatch");
        }
        if r.remaining() != 0 {
            return format_err("trailing bytes after tile data");
        }
        Ok(t)
    }

    /// Serializes the tile blob. Updates no header fields: callers keep counts consistent (see `sync_header`).
    pub fn serialize(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(self.data_size());
        self.serialize_into(&mut out);
        out
    }

    pub fn serialize_into(&self, out: &mut Vec<u8>) {
        put(out, &self.header);
        put_slice(out, &self.verts);
        put_slice(out, &self.polys);
        put_slice(out, &self.links);
        put_slice(out, &self.detail_meshes);
        put_slice(out, &self.detail_verts);
        put_slice(out, &self.detail_tris);
        put_slice(out, &self.bv_tree);
        put_slice(out, &self.off_mesh_cons);
        put(out, &(self.parts.len() as u32));
        put(out, &(self.part_polys.len() as u32));
        put_slice(out, &self.parts);
        put_slice(out, &self.part_polys);
        put(out, &(self.obstacles.len() as u32));
        put_slice(out, &self.obstacles);
    }
}

pub(crate) fn check_tile_header(h: &MeshHeader) -> Result<()> {
    if h.magic != TILE_MAGIC {
        return format_err("bad tile magic");
    }
    if h.version != TILE_VERSION {
        return format_err(format!("unsupported tile version {}", h.version));
    }
    Ok(())
}

/// A whole navmesh in memory. For large files that only need a few tiles, see [`crate::NavMeshReader`].
///
/// The offset table has `max_tiles` entries indexed by tile slot (`ref_tile(ref)`); slots without a tile hold
/// [`NO_TILE`]. Records follow the table back to back, `num_tiles` of them; the game reads them sequentially and
/// ignores the table.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct NavMeshFile {
    pub header: SetHeader,
    /// In file order.
    pub tiles: Vec<Tile>,
}

impl NavMeshFile {
    pub fn parse(bytes: &[u8]) -> Result<NavMeshFile> {
        crate::NavMeshReader::from_bytes(bytes)?.read_all()
    }

    pub fn read(path: impl AsRef<Path>) -> Result<NavMeshFile> {
        let path = path.as_ref();
        let bytes = std::fs::read(path).map_err(io_err("read", path))?;
        Self::parse(&bytes)
    }

    pub fn serialize(&self) -> Result<Vec<u8>> {
        let mut h = self.header;
        h.magic = SET_MAGIC;
        h.version = SET_VERSION;
        h.num_tiles = self.tiles.len() as i32;
        if (h.max_tiles as i64) < self.tiles.len() as i64 {
            return format_err("more tiles than maxTiles");
        }
        let mut table = vec![NO_TILE; h.max_tiles as usize];
        let mut offset: u64 = 0;
        for t in &self.tiles {
            let slot = ref_tile(t.tile_ref) as usize;
            if slot >= table.len() {
                return format_err("tile ref outside maxTiles");
            }
            if offset > 0xffff_fffe {
                return format_err("navmesh larger than 4 GB");
            }
            table[slot] = offset as u32;
            offset += (size_of::<TileRecordHeader>() + t.data_size()) as u64;
        }

        let mut out = Vec::with_capacity(size_of::<SetHeader>() + size_of_val(&table[..]) + offset as usize);
        put(&mut out, &h);
        put_slice(&mut out, &table);
        for t in &self.tiles {
            let record = TileRecordHeader { tile_ref: t.tile_ref, data_size: t.data_size() as i32, pad: t.record_pad };
            put(&mut out, &record);
            let start = out.len();
            t.serialize_into(&mut out);
            debug_assert_eq!(out.len() - start, record.data_size as usize);
        }
        Ok(out)
    }

    pub fn write(&self, path: impl AsRef<Path>) -> Result<()> {
        let path = path.as_ref();
        std::fs::write(path, self.serialize()?).map_err(io_err("write", path))
    }

    /// The tile at a grid cell, if any.
    pub fn tile_at(&self, x: i32, z: i32) -> Option<&Tile> {
        self.tiles.iter().find(|t| t.header.x == x && t.header.y == z)
    }
}
