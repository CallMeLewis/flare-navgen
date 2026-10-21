# Tile generation pipeline

How one tile of data-server geometry becomes a navmesh tile, as in DayZ's navmeshes, implemented in
`src/dznav/TileBuilder.cpp`, `src/dznav/JumpLinks.cpp` and `third_party/recastnavigation`
(changes marked `DAYZ:`).

**Validation.** Built from the same geometry, our tiles are byte-identical to reference
navmeshes: 52 tiles built from replayed captures, plus a 49-tile fixture (castle, doors,
fences, ponds, shoreline). Comparison ignores only the door "disabled" bit (random in reference
files, see below) and neighbour-dependent link data. Against reference saves of 26,160 tiles,
everything matches except tiles around animated objects and the object order a new DayZDiag
session can produce.
The island filter reproduces shipped Chernarus byte for byte. See [validation.md](validation.md).

## Per-tile configuration

All arithmetic is single precision; `ceil` is `ceilf`. `CC` is the tile's cell class from world
params (chosen by the data server: 0 for terrain/water/roads only, 1 for objects/trees/walls,
2 for buildings/doors/ladders).

| Parameter | Value |
|---|---|
| cell size `cs` | `CC.width / CC.cellsX` (0.25 or 0.1 on shipped maps) |
| cell height `ch` | 0.1 (fixed) |
| walkable height | `ceil(standHeight / 0.1)` = 15 |
| crouch / prone height | `ceil(h / 0.1) - 2` = 8 / 3 |
| walkable climb | `ceil(maxStepHeight / 0.1)` = 5 |
| walkable radius | `ceil(diameter * 0.5 / cs)` = 2 or 3 |
| border | `max(10, radius - (int)(-3 / cs))` = 14 or 33 |
| bounds | tile bounds ± border·cs; y from the input's min/max vertex |
| max edge length | 120 voxels |
| simplification error | `contourMaxError / cs` horizontally (0.3 m), 8 voxels vertically |
| min region area | `(int)(1/cs)²` per area, unlimited for 10/12/13/14 |
| merge region area | 400 |
| verts per poly | 6, no detail mesh |

## Steps

1. **Terrain.** Group 0 is rasterised with a slope test (`cos(maxSlope)` against the normal
   computed with Enfusion's RSQRTSS approximation). Input triangles are wound opposite to
   Recast, so all rasterisation uses `(v0, v2, v1)`.
2. **Mode filter.** A 21×21 majority vote over the lowest span's area per column, ignoring area 0.
3. **Objects**, in group order. Every span carries a 16-bit *part id* (`subArea`) next to its area:
   - 6, 7, 8, 9, 11: added with a slope test;
   - 2 (water): two passes painting spans deep enough under the surface as water (2), then deep
     water (3);
   - 10 (trees): each tree paints the ground around its trunk with area 10 and part = tree index;
   - 12/13 (door open/closed footprints) and 14 (hide parts): each part paints the spans below it
     with part = running door counter. Parts are clipped the older Recast way (the first row and
     column are clamped to 0, so their piece keeps geometry beyond the heightfield's min edge),
     and a cell piece with any vertex beyond the min x/z edge is skipped: a part crossing that
     edge never paints row or column 0 there;
   - 15 (ladders): become off-mesh links; 18 ("WallWithHole"): ceilings for crawl/crouch.
4. **Filters.** A multi-height low-height filter (clearance ≤ 3/8/15 voxels under an area-18
   ceiling gives unwalkable/crawl 16/crouch 17; any other ceiling ≤ 15 voxels makes the span
   unwalkable), the stock low-hanging filter (also copying the part id), and the old ledge filter
   with terrain exempt.
5. **Compact heightfield** with the prone height (3 voxels) as clearance.
6. **Post-compact areas.** Sea box (`waterdeepsea`, 5) up to the server's sea level (a live tide
   value); dilate doors (0.8·radius) and hide parts/crawl (radius); extract tree obstacles (trees
   with > 2 m clearance and ≤ 3 m footprint become walkable plus a cylinder record, others become
   holes); erode by the agent radius; remap 7, 9 → 1 and 11 → 8; topmost building spans open to
   the sky → 1.
7. **Jump links** (see below), computed on this heightfield.
8. **Regions** (watershed), **contours** and **poly mesh**, all treating `(area, part)` as the
   region identity so every door part gets its own polygons. Border vertices are never removed.
9. **Finalize.** Flags from areas (terrain 0x1, water 0x11, deep water 0x10, sea 0x20, building
   0x9, doors 0x5, crawl 0x400, crouch 0x800); door parts collect their polygons by part id and set
   the disabled flag (0x2) from the door state; obstacles and parts go to the extra section;
   Detour tile version 12.

## Jump links

For each link class in config order:

1. Contours of the final heightfield (monotone regions) give candidate edges at walkable borders.
   The monotone pass uses a 1-cell border, which erosion always separates from walkable spans,
   so every group of connected regions is subject to the minimum-size filter, with the
   threshold taken from the group's lowest-numbered region. When that region is a door, tree or
   hide part (threshold `INT_MAX`), the whole group is dropped and gets no links. DayZ's
   navmeshes do the same, and it depends on exactly which cells along the tile's min edges door
   parts paint (step 3).
   For every edge, the facing edge across an obstacle (within 1.8 m on shipped configs) is found
   and the midline between them becomes a centreline.
2. A jump profile is swept along each centreline, once jumping each way across it (DayZ 1.30;
   1.29 and earlier only swept one way): step height at the ends, jump height in the
   middle, plus the agent height, with take-off/landing within the class's drop-down limits, no
   water at either end, and the obstacle height at the midline ≥ `minCenterHeight` (≤ when
   negative). Runs of valid samples longer than the agent diameter become spans. Sample points
   sit on cell boundaries often enough that float association matters: the far end is
   `dir·t + (n·h + A)`, with `n·h + A` computed once per edge.
3. A span and its reverse (ends swapped, every end within 5 cells of its counterpart) merge into
   one bidirectional span, the longer of the two. Each span merges at most once; the rest stay
   one-way.
4. Links are placed every 2 m along each span, centred.

## Island filter

After all tiles are linked: flood fill from the poly nearest the seed (half extents 30, 30, 3)
along every link, and flag unreached polys 0x8000.

## Intentional differences

- **Door state.** The data server computes each door part's state but never sends it, so in
  reference files about 30 % of door parts get the disabled flag at random. We bake a deterministic state (`--doors closed|open|enabled`, default closed). The
  game updates door parts at runtime anyway.
- **Part lookup bounds check.** On coastal tiles with trees below sea level and doors, a polygon
  can carry a tree index where a door part index belongs (see [crashes.md](crashes.md)). We
  ignore out-of-range part ids, which leaves every other polygon unchanged.
