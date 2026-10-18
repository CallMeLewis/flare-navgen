// C interface to the C++ core: tile building (TileBuilder, JumpLinks on the patched Recast) and the
// patched Detour navmesh that tiles are linked in. Everything else lives in the Rust crates.
//
// No C++ exception crosses this boundary: failures return 0 and describe themselves in an error buffer.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bytes allocated by the core. Release them with dzn_bytes_free.
typedef struct DznBytes {
  uint8_t* data;
  size_t len;
} DznBytes;

void dzn_bytes_free(DznBytes bytes);

// ---- Tile building ----

// World parameters in their wire layout (see WorldInput.h).
typedef struct DznWorldParams {
  const void* block;         // WorldParamsBlock, 0x54 bytes
  const void* cell_classes;  // CellClass[cell_class_count]
  uint32_t cell_class_count;
  const void* links;  // LinkClass[link_count]
  uint32_t link_count;
} DznWorldParams;

typedef struct DznInputPart {
  uint32_t part_index;
  uint32_t first_vertex;
  uint64_t object_id;
} DznInputPart;

typedef struct DznInputGroup {
  uint32_t area;
  uint32_t part_count;
  const DznInputPart* parts;
  const float* verts;  // xyz
  size_t vert_count;   // vertices, not floats
} DznInputGroup;

typedef struct DznTileInput {
  int32_t x;
  int32_t z;
  uint32_t cell_class;
  uint32_t group_count;
  float bmin[3];
  float bmax[3];
  const DznInputGroup* groups;
} DznTileInput;

enum { DZN_DOORS_CLOSED = 0, DZN_DOORS_OPEN = 1, DZN_DOORS_ENABLED = 2 };

typedef struct DznTileBuildOptions {
  uint32_t jump_links;  // 0 or 1
  uint32_t doors;       // DZN_DOORS_*
  float sea_level;      // NaN keeps the server's
} DznTileBuildOptions;

typedef struct DznTileBuildStats {
  int32_t data_size;
  int32_t polys;
  int32_t verts;
  int32_t off_mesh;
  int32_t obstacles;
  int32_t stray_part_ids;
} DznTileBuildStats;

// Builds one tile blob (tile version 12, links not yet built) into *tile, which is left empty when the tile has no
// walkable surface. Returns 1, or 0 with the reason in *error.
int dzn_build_tile(const DznWorldParams* params, const DznTileInput* input, const DznTileBuildOptions* options,
                   DznBytes* tile, DznTileBuildStats* stats, DznBytes* error);

// ---- Navmesh (patched Detour) ----

typedef struct DznNavMesh DznNavMesh;

typedef struct DznNavMeshParams {
  float orig[3];
  float tile_width;
  float tile_height;
  int32_t max_tiles;
  int32_t max_polys;
} DznNavMeshParams;

// Returns null with the reason in *error.
DznNavMesh* dzn_navmesh_new(const DznNavMeshParams* params, DznBytes* error);
void dzn_navmesh_free(DznNavMesh* mesh);
void dzn_navmesh_params(const DznNavMesh* mesh, DznNavMeshParams* out);

// Adds a copy of a tile blob and links it. With ref 0 Detour picks a free slot; otherwise the tile gets that ref
// (slot and salt). Returns 1 with the new ref in *out_ref, or 0 with the reason in *error.
int dzn_navmesh_add_tile(DznNavMesh* mesh, const uint8_t* data, size_t len, uint64_t ref, uint64_t* out_ref,
                         DznBytes* error);
// Adds a copy of a tile blob, replacing any tile at the same grid cell. The replaced tile's slot is reused (with a new
// salt) so the tile order of the file is kept. Returns 1 with the new ref in *out_ref and the replaced tile's ref (or
// 0) in *replaced_ref, or 0 with the reason in *error.
int dzn_navmesh_set_tile(DznNavMesh* mesh, const uint8_t* data, size_t len, uint64_t* out_ref, uint64_t* replaced_ref,
                         DznBytes* error);
// Removes a tile. Returns 1, or 0 if there is no such tile.
int dzn_navmesh_remove_tile(DznNavMesh* mesh, uint64_t ref);
// The ref of the tile at a grid cell (layer 0), or 0.
uint64_t dzn_navmesh_tile_ref_at(const DznNavMesh* mesh, int32_t x, int32_t z);

int32_t dzn_navmesh_max_tiles(const DznNavMesh* mesh);
// The tile in slot `index`: returns 1 with its ref and bytes (owned by the mesh and valid until it changes), or 0 if
// the slot is empty.
int dzn_navmesh_tile(const DznNavMesh* mesh, int32_t index, uint64_t* ref, const uint8_t** data, size_t* len);

typedef struct DznIslandResult {
  uint32_t seed_found;
  float seed_point[3];
  uint64_t reachable;
  uint64_t isolated;
} DznIslandResult;

// Flood fills from the poly nearest `seed` along every link and flags unreached polys 0x8000 (clearing earlier
// 0x8000 flags first). The seed snaps with half extents {30, 30, 3}; search_all_heights widens the vertical extent to
// the whole world. Returns 1, or 0 with the reason in *error.
int dzn_navmesh_filter_islands(DznNavMesh* mesh, const float seed[3], int search_all_heights, DznIslandResult* out,
                               DznBytes* error);

#ifdef __cplusplus
}
#endif
