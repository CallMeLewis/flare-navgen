# DayZDiag data-server protocol

Status: the client side is complete. World params and tile requests are **verified
live** against `DayZDiag_x64 -server -startNavmeshDataServer` (DayZ 1.28).

## Transport

- TCP, IPv4. The data server listens on **36533**.
- Framing: `u32 length` (bytes after itself), `u32 messageId`, payload. Little-endian, raw
  structs, no compression or checksum.
- One request is outstanding per connection at a time, so fetching geometry from a single server
  is serial; fetch from several servers to go faster.

## Messages

| Id | Direction | Payload |
|---|---|---|
| 1 | generator → server | empty: finished; the server shuts down (`-kill`) |
| 2 | generator → server | empty: request world params |
| 3 | generator → server | request tile: `i32 x, i32 z, f32 minX, minZ, maxX, maxZ` (tile bounds) |
| 4 | server → generator | world params: `u32 version (10)`, world block, cell classes, link classes |
| 5 | server → generator | one tile's input geometry |

## World params (id 4)

### World block (0x54 bytes)

The server sends its terrain's settings, so the values vary from terrain to terrain.

| Offset | Type | Field |
|---|---|---|
| 0x00 | f32 | world size X (m) |
| 0x04 | f32 | world size Z (m) |
| 0x08 | f32 | tile width |
| 0x0C | f32 | tile height |
| 0x10 | i32 | tiles X |
| 0x14 | i32 | tiles Z |
| 0x18 | f32 | deep-sea height (m): top of the `waterdeepsea` box |
| 0x1C | f32 | deep-water depth (m): water deeper than this becomes `waterdeep` |
| 0x20 | f32 | contour max error (m) |
| 0x24 | u8 + 3 pad | obstacles: turn area-10 parts into cylinder obstacles |
| 0x28 | i32 | scheduler block size in tiles |
| 0x2C | u8 + 3 pad | filterIsolatedIslands |
| 0x30 | f32[3] | seedPosition |
| 0x3C | f32 × 6 | Agent: standHeight, crouchHeight, proneHeight, diameter, maxStepHeight, maxSlope |

Then `u32 n` cell classes (16 bytes each: `i32 cellsX, i32 cellsZ, f32 width, f32 height`;
cell size = width / cellsX) and `u32 m` link classes (52 bytes each):

| Offset | Type | Field |
|---|---|---|
| 0x00 | u32 | class index → off-mesh `userId` |
| 0x04 | u32 | class name hash |
| 0x08 | i32 | typeId (−1 if unset) |
| 0x0C | f32 | jumpHeight |
| 0x10 | f32 | jumpLength |
| 0x14 | f32 | jumpDropdownMin |
| 0x18 | f32 | jumpDropdownMax |
| 0x1C | f32 | minCenterHeight |
| 0x20 | f32 | agentHeight |
| 0x24 | f32 | maxStepHeight |
| 0x28 | f32 | agentRadius |
| 0x2C | u16 | poly flags |
| 0x2E | u8 | area id |
| 0x2F | u8 | 0 |
| 0x30 | i32 | debug colour |

## Tile record (id 5)

```
i32 x, i32 z
u32 cellClass                 index into the cell classes
u32 groupCount
f32 bmin[3], f32 bmax[3]      tile bounds; Y spans the geometry
groupCount × {
  u32 area
  u32 partCount
  partCount × { u32 partIndex, u64 objectId, u32 firstVertex }
  i32 vertexCount
  f32 verts[vertexCount][3]    triangle soup (x, y up, z); ladders: segment pairs
}
```

Parts split a group's vertices into objects (doors, trees). Part *i* covers vertices
`[parts[i].firstVertex, parts[i+1].firstVertex)`.

Observed input areas: 1 terrain (always the first group, on the terrain grid, padded about 25 m
around the tile), 7, 8, 9, 11 building geometry, 10 obstacle parts (one tree collision mesh per
part, object id 0), 12/13 door opened/closed parts, 15 ladders, 2 water.
