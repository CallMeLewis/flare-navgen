# DayZ navmesh file format (`.nm`)

Status: **verified**. `dznav roundtrip` parses and re-serializes the shipped Chernarus, Livonia and
Sakhal navmeshes byte for byte (`DZ\worlds\<world>\navmesh\navmesh.nm`, game versions 1.27–1.30).

All values are little-endian. The layout is RecastDemo's "tile mesh set" extended by Bohemia
Interactive. Detour is compiled with `DT_POLYREF64`.

## File layout

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `'MSET'` (bytes `TESM`) |
| 4 | 4 | set version: `9` since DayZ 1.30, `7` before (see below) |
| 8 | 4 | `numTiles` |
| 12 | 4 | `tilesX` – grid width in tiles |
| 16 | 4 | `tilesZ` – grid height in tiles |
| 20 | 12 | `dtNavMeshParams.orig[3]` (always 0,0,0 in shipped maps) |
| 32 | 4 | `tileWidth` (50.0) |
| 36 | 4 | `tileHeight` (50.0) |
| 40 | 4 | `maxTiles` (= `numTiles`) |
| 44 | 4 | `maxPolys` per tile (`1 << 20`) |
| 48 | 4 × `maxTiles` | tile offset table indexed by tile slot, relative to the end of the table; `0xFFFFFFFF` for empty slots |
| … | … | tile records |

The game's loader skips the offset table (using `maxTiles`) and reads `numTiles` tile records
sequentially. The loader aborts if any record has `tileRef == 0` or `dataSize == 0`.

The game only loads its own set version ("Navmesh could not be loaded: Invalid version"). DayZ 1.30
went from version 7 to 9 without changing the layout: everything below is the same in both. dznav
reads either and always saves version 9.

Shipped maps have a tile in every grid cell, including open sea. Files saved after generating a
range only contain the generated tiles: `numTiles < maxTiles`, and the table
holds `0xFFFFFFFF` for the missing slots.

### Tile record

| Size | Field |
|---|---|
| 8 | `tileRef` – `salt << 48 \| tileIndex << 20` (salt is 1) |
| 4 | `dataSize` |
| 4 | padding – 0 in shipped maps, other values in some saves; ignored |
| `dataSize` | Detour tile blob |

Records are stored in tile-slot order. The slot is the `dtNavMesh` tile index, i.e. the order in
which the tiles were added, so it isn't related to grid position. Each tile's `(x, y)` is
in its header.

## Tile blob (tile version 12)

Sections follow each other with no extra alignment. Sizes are given per element.

| Section | Element size | Count |
|---|---|---|
| `dtMeshHeader` | 104 | 1 |
| vertices (`float[3]`) | 12 | `vertCount` |
| `dtPoly` | 32 | `polyCount` |
| `dtLink` | 16 | `maxLinkCount` |
| `dtPolyDetail` | 12 | `detailMeshCount` (always 0) |
| detail vertices | 12 | `detailVertCount` (always 0) |
| detail triangles | 4 | `detailTriCount` (always 0) |
| `dtBVNode` | 16 | `bvNodeCount` (= 2 × ground polys) |
| `dtOffMeshConnection` | 44 | `offMeshConCount` |
| extra section | – | `extraDataSize` bytes |

Coordinates are absolute world coordinates (x east, y up, z north). Tile `y` in the header is
the grid row along world Z.

Tiles are saved after `dtNavMesh::addTile`, so `dtLink` entries and `firstLink` are populated,
off-mesh start/end vertices are snapped to the mesh, and obstacle Y values are snapped to the
nearest polygon.

### DayZ's Detour

DayZ's Detour links tiles like upstream Detour, with these differences (all reproduced in
`third_party/recastnavigation`, marked `DAYZ:`; `tests/relink.cpp` re-adds every shipped tile with
zeroed links and gets identical bytes):

- Tiles have no detail meshes. Heights come from the polygon's triangle fan using the pre-2016
  `closestPointOnPoly` (clamp to the nearest edge, then height at the original position) and a
  barycentric test with a 1e-3 tolerance.
- Portal limits (`dtLink::bmin/bmax`) are truncated, not rounded: `(u8)(t * 255.0f)`.
- `baseOffMeshLinks` sets the reserved float (+28) to 1.0 and only allocates the land→link back
  link if the forward link was allocated.
- After linking, each obstacle's Y is replaced by the height of the nearest polygon
  (`findNearestPolyInTile`, half extents `{0, 30, 0}`).

### `dtMeshHeader` (104 bytes)

Stock Detour fields (`magic 'DNAV'`, `version 12`, `x`, `y`, `layer`, `userId`, `polyCount`,
`vertCount`, `maxLinkCount`, `detailMeshCount`, `detailVertCount`, `detailTriCount`,
`bvNodeCount`, `offMeshConCount`, `offMeshBase`, `walkableHeight`, `walkableRadius`,
`walkableClimb`, `bmin[3]`, `bmax[3]`, `bvQuantFactor`), followed by:

| Offset | Size | Field |
|---|---|---|
| 100 | 4 | `extraDataSize` – byte size of the extra section |

`bvQuantFactor` is `1 / cellSize` for the tile: shipped maps mix 0.25 m tiles (4.0) and 0.1 m
tiles (10.0), matching `GenParams.cellSize1..3`.

### `dtOffMeshConnection` (44 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 24 | `pos[6]` – start and end point |
| 24 | 4 | centre height – measured obstacle height at the midpoint, relative to the start. ≥ `minCenterHeight` of the link class; negative for gap jumps. Not read by Detour |
| 28 | 4 | reserved – `dtCreateNavMeshData` writes 0, `addTile` overwrites it with 1.0 |
| 32 | 4 | `rad` – endpoint snap radius used by `addTile` |
| 36 | 2 | `poly` – index of the off-mesh poly in this tile |
| 38 | 1 | `flags` – 1 = bidirectional |
| 39 | 1 | `side` – 0xff when both ends are in this tile, else the neighbour side |
| 40 | 4 | `userId` – index of the link class in `GenParams/Links` |

The horizontal length of every shipped link equals its class's `jumpLength`.

### Extra section

```
u32 partCount
u32 partPolyCount
TilePart     parts[partCount]          // 16 bytes each
TilePartPoly partPolys[partPolyCount]  //  8 bytes each
u32 obstacleCount
Obstacle     obstacles[obstacleCount]  // 16 bytes each
```

| Struct | Fields |
|---|---|
| `TilePart` | `u64 objectId`, `u32 partIndex`, `i32 firstPoly` (index into `partPolys`, −1 = none) |
| `TilePartPoly` | `u32 poly`, `i32 next` (−1 terminates) |
| `Obstacle` | `float x, y, z, radius` |

Parts link the polygons of a building's doors to the building object: each door contributes two
entries, `partIndex = 2k` and `2k + 1`, one per door state. Obstacles are vertical cylinders, such
as tree trunks, that are left out of the mesh and avoided at runtime.

## Area ids

| Id | Name | Default poly flags |
|---|---|---|
| 1 | `terrain` | walk |
| 2 | `water` | walk, swim |
| 3 | `waterdeep` | swim |
| 4 | `watersea` | walk, swim |
| 5 | `waterdeepsea` | swimsea |
| 8 | `building` | walk, inside |
| 12 | `dooropened` | walk, door |
| 13 | `doorclosed` | walk, door |
| 15 | `ladder` | – |
| 16 | `crawl` | crawl |
| 17 | `crouch` | crouch |
| 48 + N | `jumpN` (`jump` = `jump0`) | from the link class `flags[]` |

## Poly flags

| Bit | Name |
|---|---|
| 0x0001 | `walk` |
| 0x0002 | `disabled` |
| 0x0004 | `door` |
| 0x0008 | `inside` |
| 0x0010 | `swim` |
| 0x0020 | `swimsea` |
| 0x0040 | `ladder` |
| 0x0080 | `jumpover` |
| 0x0100 | `jumpdown` |
| 0x0180 | `jump` (`jumpover \| jumpdown`) |
| 0x0200 | `climb` |
| 0x0400 | `crawl` |
| 0x0800 | `crouch` |
| 0x8000 | isolated – unreachable from `seedPosition` (set by the island filter) |
