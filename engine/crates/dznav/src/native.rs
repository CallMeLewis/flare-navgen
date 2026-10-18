//! The C++ core: building tiles on the DayZ-patched Recast, and linking them in the patched Detour.

use std::collections::HashMap;
use std::ptr::NonNull;

use dznav_sys as sys;

use crate::error::{Error, Result};
use crate::format::{NavMeshFile, Obstacle, POLY_BITS, SET_MAGIC, SET_VERSION, SetHeader, Tile};
use crate::world::{TileInput, WorldParams};

/// Takes an error message from the core.
fn take_error(bytes: sys::DznBytes) -> Error {
    let message = if bytes.data.is_null() {
        "unknown error in the navmesh core".to_string()
    } else {
        // SAFETY: the core allocated `len` bytes at `data`.
        String::from_utf8_lossy(unsafe { std::slice::from_raw_parts(bytes.data, bytes.len) }).into_owned()
    };
    // SAFETY: allocated by the core, released once.
    unsafe { sys::dzn_bytes_free(bytes) };
    Error::Native(message)
}

/// Takes bytes from the core into a Vec.
fn take_bytes(bytes: sys::DznBytes) -> Vec<u8> {
    if bytes.data.is_null() {
        return Vec::new();
    }
    // SAFETY: the core allocated `len` bytes at `data`.
    let v = unsafe { std::slice::from_raw_parts(bytes.data, bytes.len) }.to_vec();
    // SAFETY: allocated by the core, released once.
    unsafe { sys::dzn_bytes_free(bytes) };
    v
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum DoorStatePolicy {
    /// Doors start closed: open-state (area 12) polys get the disabled flag.
    #[default]
    Closed,
    /// Doors start open: closed-state (area 13) polys get the disabled flag.
    Open,
    /// No door poly is disabled.
    Enabled,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TileBuildOptions {
    pub jump_links: bool,
    pub doors: DoorStatePolicy,
    /// Overrides the sea level sent by the server (it is the live tide value).
    pub sea_level: Option<f32>,
}

impl Default for TileBuildOptions {
    fn default() -> Self {
        Self { jump_links: true, doors: DoorStatePolicy::Closed, sea_level: None }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TileBuildStats {
    pub x: i32,
    pub z: i32,
    pub data_size: i32,
    pub polys: i32,
    pub verts: i32,
    pub off_mesh: i32,
    pub obstacles: i32,
    /// Polys whose part id is out of range (ignored).
    pub stray_part_ids: i32,
    /// Set by the generator.
    pub build_ms: f64,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct BuiltTile {
    /// The tile blob (tile version 12, links not yet built), or None when the tile has no walkable surface.
    pub data: Option<Vec<u8>>,
    pub stats: TileBuildStats,
}

/// Builds one DayZ navmesh tile from data-server input, byte for byte as in DayZ's navmeshes. Safe to call from
/// several threads at once.
pub fn build_tile(params: &WorldParams, input: &TileInput, options: &TileBuildOptions) -> Result<BuiltTile> {
    let world = sys::DznWorldParams {
        block: bytemuck::bytes_of(&params.block).as_ptr(),
        cell_classes: params.cell_classes.as_ptr().cast(),
        cell_class_count: params.cell_classes.len() as u32,
        links: params.links.as_ptr().cast(),
        link_count: params.links.len() as u32,
    };
    let groups: Vec<sys::DznInputGroup> = input
        .groups
        .iter()
        .map(|g| sys::DznInputGroup {
            area: g.area,
            part_count: g.parts.len() as u32,
            // InputPart has DznInputPart's layout.
            parts: g.parts.as_ptr().cast(),
            verts: g.verts.as_ptr(),
            vert_count: g.vertex_count(),
        })
        .collect();
    let tile_input = sys::DznTileInput {
        x: input.x,
        z: input.z,
        cell_class: input.cell_class,
        group_count: groups.len() as u32,
        bmin: input.bmin,
        bmax: input.bmax,
        groups: groups.as_ptr(),
    };
    let opts = sys::DznTileBuildOptions {
        jump_links: options.jump_links.into(),
        doors: match options.doors {
            DoorStatePolicy::Closed => sys::DZN_DOORS_CLOSED,
            DoorStatePolicy::Open => sys::DZN_DOORS_OPEN,
            DoorStatePolicy::Enabled => sys::DZN_DOORS_ENABLED,
        },
        sea_level: options.sea_level.unwrap_or(f32::NAN),
    };
    if input.cell_class as usize >= params.cell_classes.len() {
        return Err(Error::Native(format!("tile {} {} has unknown cell class {}", input.x, input.z, input.cell_class)));
    }

    let mut tile = sys::DznBytes::empty();
    let mut stats = sys::DznTileBuildStats::default();
    let mut error = sys::DznBytes::empty();
    // SAFETY: every pointer refers to data borrowed for the duration of the call, with the counts given.
    let ok = unsafe { sys::dzn_build_tile(&world, &tile_input, &opts, &mut tile, &mut stats, &mut error) };
    if ok == 0 {
        return Err(take_error(error));
    }
    let data = take_bytes(tile);
    Ok(BuiltTile {
        data: (!data.is_empty()).then_some(data),
        stats: TileBuildStats {
            x: input.x,
            z: input.z,
            data_size: stats.data_size,
            polys: stats.polys,
            verts: stats.verts,
            off_mesh: stats.off_mesh,
            obstacles: stats.obstacles,
            stray_part_ids: stats.stray_part_ids,
            build_ms: 0.0,
        },
    })
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct IslandFilterResult {
    pub seed_found: bool,
    pub seed_point: [f32; 3],
    pub reachable: u64,
    pub isolated: u64,
}

/// A live navmesh in the patched Detour that tiles are added to, linked, and exported from.
pub struct NavMeshAssembly {
    mesh: NonNull<sys::DznNavMesh>,
    tiles_x: i32,
    tiles_z: i32,
    // Obstacle sections as loaded. Adding a tile re-snaps obstacle Y, which isn't idempotent; untouched tiles keep
    // their original bytes on export.
    loaded_obstacles: HashMap<u64, Vec<Obstacle>>,
}

// SAFETY: the Detour mesh is owned by this value and only reached through it.
unsafe impl Send for NavMeshAssembly {}

impl Drop for NavMeshAssembly {
    fn drop(&mut self) {
        // SAFETY: created by dzn_navmesh_new, freed once.
        unsafe { sys::dzn_navmesh_free(self.mesh.as_ptr()) }
    }
}

impl NavMeshAssembly {
    /// An empty mesh for a `tiles_x` x `tiles_z` grid.
    pub fn new(tiles_x: i32, tiles_z: i32, tile_width: f32, tile_height: f32, orig: [f32; 3]) -> Result<Self> {
        let max_tiles = tiles_x
            .checked_mul(tiles_z)
            .filter(|&n| n > 0)
            .ok_or_else(|| Error::Native(format!("invalid tile grid {tiles_x} x {tiles_z}")))?;
        let params = sys::DznNavMeshParams { orig, tile_width, tile_height, max_tiles, max_polys: 1 << POLY_BITS };
        let mut error = sys::DznBytes::empty();
        // SAFETY: params and error are valid for the call.
        let mesh = unsafe { sys::dzn_navmesh_new(&params, &mut error) };
        let mesh = NonNull::new(mesh).ok_or_else(|| take_error(error))?;
        Ok(Self { mesh, tiles_x, tiles_z, loaded_obstacles: HashMap::new() })
    }

    /// Creates the mesh from a file and adds its tiles in file order, keeping their refs.
    pub fn load(file: &NavMeshFile) -> Result<Self> {
        let h = &file.header;
        let mut mesh = Self::new(h.tiles_x, h.tiles_z, h.tile_width, h.tile_height, h.orig)?;
        let mut data = Vec::new();
        for t in &file.tiles {
            data.clear();
            t.serialize_into(&mut data);
            let tile_ref = mesh.add_tile(&data, t.tile_ref)?;
            mesh.loaded_obstacles.insert(tile_ref, t.obstacles.clone());
        }
        Ok(mesh)
    }

    pub fn tiles_x(&self) -> i32 {
        self.tiles_x
    }

    pub fn tiles_z(&self) -> i32 {
        self.tiles_z
    }

    fn params(&self) -> sys::DznNavMeshParams {
        let mut p = sys::DznNavMeshParams::default();
        // SAFETY: the mesh is valid and p is writable.
        unsafe { sys::dzn_navmesh_params(self.mesh.as_ptr(), &mut p) };
        p
    }

    /// Adds a tile blob and links it, as tile `tile_ref` (slot and salt), or in any free slot with 0. Returns its ref.
    pub fn add_tile(&mut self, data: &[u8], tile_ref: u64) -> Result<u64> {
        let mut out = 0;
        let mut error = sys::DznBytes::empty();
        // SAFETY: data is valid for len bytes; the core copies it.
        let ok = unsafe {
            sys::dzn_navmesh_add_tile(self.mesh.as_ptr(), data.as_ptr(), data.len(), tile_ref, &mut out, &mut error)
        };
        if ok == 0 {
            return Err(take_error(error));
        }
        Ok(out)
    }

    /// Adds a tile blob (tile version 12), replacing any tile at the same grid cell. The previous tile's slot is
    /// reused so the tile order of the file is kept. Returns the new tile's ref.
    pub fn set_tile(&mut self, data: &[u8]) -> Result<u64> {
        let (mut out, mut replaced) = (0, 0);
        let mut error = sys::DznBytes::empty();
        // SAFETY: data is valid for len bytes; the core copies it.
        let ok = unsafe {
            sys::dzn_navmesh_set_tile(
                self.mesh.as_ptr(),
                data.as_ptr(),
                data.len(),
                &mut out,
                &mut replaced,
                &mut error,
            )
        };
        if replaced != 0 {
            self.loaded_obstacles.remove(&replaced);
        }
        if ok == 0 {
            return Err(take_error(error));
        }
        Ok(out)
    }

    /// The ref of the tile at a grid cell, if any.
    pub fn tile_ref_at(&self, x: i32, z: i32) -> Option<u64> {
        // SAFETY: the mesh is valid.
        let r = unsafe { sys::dzn_navmesh_tile_ref_at(self.mesh.as_ptr(), x, z) };
        (r != 0).then_some(r)
    }

    pub fn has_tile(&self, x: i32, z: i32) -> bool {
        self.tile_ref_at(x, z).is_some()
    }

    pub fn remove_tile(&mut self, x: i32, z: i32) -> bool {
        let Some(r) = self.tile_ref_at(x, z) else { return false };
        self.loaded_obstacles.remove(&r);
        // SAFETY: the mesh is valid.
        unsafe { sys::dzn_navmesh_remove_tile(self.mesh.as_ptr(), r) != 0 }
    }

    /// Every tile's ref and current bytes (links included), in tile-index order.
    pub fn tiles(&self) -> impl Iterator<Item = (u64, &[u8])> + '_ {
        // SAFETY: the mesh is valid.
        let max = unsafe { sys::dzn_navmesh_max_tiles(self.mesh.as_ptr()) };
        (0..max).filter_map(move |i| {
            let (mut r, mut data, mut len) = (0u64, std::ptr::null(), 0usize);
            // SAFETY: the mesh is valid; the bytes it returns live until it changes, which needs &mut self.
            unsafe {
                if sys::dzn_navmesh_tile(self.mesh.as_ptr(), i, &mut r, &mut data, &mut len) == 0 {
                    return None;
                }
                Some((r, std::slice::from_raw_parts(data, len)))
            }
        })
    }

    /// Exports every tile in tile-index order (the order DayZ's navmeshes are saved in).
    pub fn to_file(&self) -> Result<NavMeshFile> {
        let p = self.params();
        let mut tiles = Vec::new();
        for (tile_ref, data) in self.tiles() {
            let mut tile = Tile::parse(data, tile_ref)?;
            if let Some(obstacles) = self.loaded_obstacles.get(&tile_ref)
                && obstacles.len() == tile.obstacles.len()
            {
                tile.obstacles = obstacles.clone();
            }
            tiles.push(tile);
        }
        let header = SetHeader {
            magic: SET_MAGIC,
            version: SET_VERSION,
            num_tiles: tiles.len() as i32,
            tiles_x: self.tiles_x,
            tiles_z: self.tiles_z,
            orig: p.orig,
            tile_width: p.tile_width,
            tile_height: p.tile_height,
            max_tiles: p.max_tiles,
            max_polys: p.max_polys,
        };
        Ok(NavMeshFile { header, tiles })
    }

    /// Flood fills from the poly nearest `seed` along every link and flags unreached polys 0x8000 (existing 0x8000
    /// flags are cleared first). The seed snaps with half extents {30, 30, 3}; with `search_all_heights` the vertical
    /// extent covers the whole world.
    pub fn filter_isolated_islands(&mut self, seed: [f32; 3], search_all_heights: bool) -> Result<IslandFilterResult> {
        let mut out = sys::DznIslandResult::default();
        let mut error = sys::DznBytes::empty();
        // SAFETY: the mesh is valid; seed holds 3 floats.
        let ok = unsafe {
            sys::dzn_navmesh_filter_islands(
                self.mesh.as_ptr(),
                seed.as_ptr(),
                search_all_heights.into(),
                &mut out,
                &mut error,
            )
        };
        if ok == 0 {
            return Err(take_error(error));
        }
        Ok(IslandFilterResult {
            seed_found: out.seed_found != 0,
            seed_point: out.seed_point,
            reachable: out.reachable,
            isolated: out.isolated,
        })
    }
}
