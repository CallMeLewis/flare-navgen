//! Raw bindings to the C++ core (`cpp/dznav_core.h`): tile building on the DayZ-patched Recast, and the patched Detour
//! navmesh tiles are linked in. The safe API is in the `dznav` crate.
#![allow(non_camel_case_types)]

use std::ffi::c_int;

#[repr(C)]
#[derive(Debug)]
pub struct DznBytes {
    pub data: *mut u8,
    pub len: usize,
}

impl DznBytes {
    pub const fn empty() -> Self {
        Self { data: std::ptr::null_mut(), len: 0 }
    }
}

#[repr(C)]
pub struct DznWorldParams {
    /// `WorldParamsBlock`, 0x54 bytes.
    pub block: *const u8,
    pub cell_classes: *const u8,
    pub cell_class_count: u32,
    pub links: *const u8,
    pub link_count: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DznInputPart {
    pub part_index: u32,
    pub first_vertex: u32,
    pub object_id: u64,
}

#[repr(C)]
pub struct DznInputGroup {
    pub area: u32,
    pub part_count: u32,
    pub parts: *const DznInputPart,
    /// xyz
    pub verts: *const f32,
    /// Vertices, not floats.
    pub vert_count: usize,
}

#[repr(C)]
pub struct DznTileInput {
    pub x: i32,
    pub z: i32,
    pub cell_class: u32,
    pub group_count: u32,
    pub bmin: [f32; 3],
    pub bmax: [f32; 3],
    pub groups: *const DznInputGroup,
}

pub const DZN_DOORS_CLOSED: u32 = 0;
pub const DZN_DOORS_OPEN: u32 = 1;
pub const DZN_DOORS_ENABLED: u32 = 2;

#[repr(C)]
pub struct DznTileBuildOptions {
    pub jump_links: u32,
    pub doors: u32,
    /// NaN keeps the server's.
    pub sea_level: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct DznTileBuildStats {
    pub data_size: i32,
    pub polys: i32,
    pub verts: i32,
    pub off_mesh: i32,
    pub obstacles: i32,
    pub stray_part_ids: i32,
}

#[repr(C)]
pub struct DznNavMesh {
    _private: [u8; 0],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct DznNavMeshParams {
    pub orig: [f32; 3],
    pub tile_width: f32,
    pub tile_height: f32,
    pub max_tiles: i32,
    pub max_polys: i32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct DznIslandResult {
    pub seed_found: u32,
    pub seed_point: [f32; 3],
    pub reachable: u64,
    pub isolated: u64,
}

unsafe extern "C" {
    pub fn dzn_bytes_free(bytes: DznBytes);

    pub fn dzn_build_tile(
        params: *const DznWorldParams,
        input: *const DznTileInput,
        options: *const DznTileBuildOptions,
        tile: *mut DznBytes,
        stats: *mut DznTileBuildStats,
        error: *mut DznBytes,
    ) -> c_int;

    pub fn dzn_navmesh_new(params: *const DznNavMeshParams, error: *mut DznBytes) -> *mut DznNavMesh;
    pub fn dzn_navmesh_free(mesh: *mut DznNavMesh);
    pub fn dzn_navmesh_params(mesh: *const DznNavMesh, out: *mut DznNavMeshParams);
    pub fn dzn_navmesh_add_tile(
        mesh: *mut DznNavMesh,
        data: *const u8,
        len: usize,
        tile_ref: u64,
        out_ref: *mut u64,
        error: *mut DznBytes,
    ) -> c_int;
    pub fn dzn_navmesh_set_tile(
        mesh: *mut DznNavMesh,
        data: *const u8,
        len: usize,
        out_ref: *mut u64,
        replaced_ref: *mut u64,
        error: *mut DznBytes,
    ) -> c_int;
    pub fn dzn_navmesh_remove_tile(mesh: *mut DznNavMesh, tile_ref: u64) -> c_int;
    pub fn dzn_navmesh_tile_ref_at(mesh: *const DznNavMesh, x: i32, z: i32) -> u64;
    pub fn dzn_navmesh_max_tiles(mesh: *const DznNavMesh) -> i32;
    pub fn dzn_navmesh_tile(
        mesh: *const DznNavMesh,
        index: i32,
        tile_ref: *mut u64,
        data: *mut *const u8,
        len: *mut usize,
    ) -> c_int;
    pub fn dzn_navmesh_filter_islands(
        mesh: *mut DznNavMesh,
        seed: *const f32,
        search_all_heights: c_int,
        out: *mut DznIslandResult,
        error: *mut DznBytes,
    ) -> c_int;
}
