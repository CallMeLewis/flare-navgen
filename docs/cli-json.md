# `dznav --json`

With `--json` (anywhere on the command line), `dznav` writes **one JSON object per line** to stdout,
each with an `"event"` field, and nothing else. It's how the desktop app drives the engine, and
it's handy for scripts. Supported by `generate`, `probe`, `capture`, `info`, `tile`, `render`,
`verify`, `diff`, `merge` and `islands`. Other commands reject `--json` with an `error` event.

Numbers are plain JSON numbers (floats up to 9 significant digits); a non-finite value is `null`.
Consumers should ignore fields and events they don't know, as more may be added.

## Errors

Any command can end with:

```json
{"event":"error","kind":"network","message":"cannot connect to 127.0.0.1:36533"}
```

`kind` is `usage`, `file` (bad or missing file, invalid data), `network` or `protocol` (the data
server sent something unexpected). The exit code is 1.

## `generate`

Events, in order:

| Event | When | Fields |
|---|---|---|
| `start` | once, after connecting and loading `--base` | `output`, `tiles` (tiles to process, after `--skip-existing`), `range` `[x0,z0,x1,z1]` (inclusive, clamped to the world), `world` `{tilesX,tilesZ,tileWidth,tileHeight}`, `server` or `capture`, `base` + `skipExisting` if given |
| `fetched` | a tile's geometry has arrived and is queued for building | `x`, `z` |
| `tile` | a tile has been built and linked into the mesh | `x`, `z`, `outcome` (`built`, `empty` = no walkable surface, `failed`), `ms`; when built: `bytes`, `polys`, `verts`, `links`, `obstacles`; `strayPartIds` if non-zero; `error` when failed |
| `progress` | at most 4 times a second, and once at the end | `done` (tiles finished, including empty and failed), `total`, `fetched`, `failed`, `elapsed` (s), `rate` (tiles/s), `eta` (s) |
| `saved` | after each `--checkpoint` save | `path`, `checkpoint: true` |
| `islands` | after the isolated-island filter (skipped when stopped) | `seedFound`, `seed` `[x,y,z]`; when found: `seedPoint`, `reachable`, `isolated` (polygon counts) |
| `done` | once, at the end | `output`, `saved`, `cancelled`, `built`, `empty`, `failed`, `seconds`, `strayTiles`, `resumable`, `errorCount`, `errors` (first 100 messages) |

`fetched` events come from the fetcher threads, so a tile's `fetched` always precedes its `tile`
event, but the two streams interleave.

**Stopping.** Write `stop` followed by a newline to stdin. Tiles already being built finish,
nothing new starts, the output is saved (unless no tile was finished), and `done` reports
`cancelled: true` with exit code 130. Ctrl+C does the same in a terminal. Closing stdin does
**not** stop the run.

**Resuming.** When `done.resumable` is true (stopped, or the data server went away), run the same
command again with `--base <output> --skip-existing`.

Exit codes: 0 success, 1 any tile failed or a fetch error ended the run early, 130 stopped.

Example:

```json
{"event":"start","output":"out.nm","tiles":49,"range":[107,10,113,16],"world":{"tilesX":327,"tilesZ":327,"tileWidth":50,"tileHeight":50},"server":"127.0.0.1"}
{"event":"fetched","x":107,"z":10}
{"event":"tile","x":107,"z":10,"outcome":"built","bytes":27664,"polys":162,"verts":417,"links":0,"obstacles":0,"ms":583.5}
{"event":"progress","done":1,"total":49,"fetched":6,"failed":0,"elapsed":1.9,"rate":0.53,"eta":91}
{"event":"done","output":"out.nm","saved":true,"cancelled":false,"built":49,"empty":0,"failed":0,"seconds":26.7,"strayTiles":0,"resumable":false,"errorCount":0,"errors":[]}
```

## `probe [host] [x z]`

```json
{"event":"world","host":"127.0.0.1","params":{...},"tile":{...}}
```

`params`: `worldSizeX`, `worldSizeZ`, `tilesX`, `tilesZ`, `tileWidth`, `tileHeight`, `seaLevel` (the
server's current tide), `deepWaterDepth`, `contourMaxError`, `obstacles`, `blockSize`,
`filterIsolatedIslands`, `seed`, `agent` `{standHeight, crouchHeight, proneHeight, diameter,
maxStepHeight, maxSlope}`, `cellClasses` `[{cells, width, cellSize}]`, `linkClasses` `[{index,
typeId, jumpLength, jumpHeight, minCenterHeight, dropdownMin, dropdownMax, agentHeight,
maxStepHeight, agentRadius, flags, area}]`.

`tile` (only with `x z`): `x`, `z`, `cellClass`, `bmin`, `bmax`, `groups` `[{area, parts, verts}]`.

## `capture`

`progress` events (`done`, `total`, `elapsed`, `rate`) after every tile, then
`{"event":"done","output":...,"tiles":N}`.

## `info <file.nm> [--tiles]`

One `info` event: `path`, `tiles`, `tilesX`, `tilesZ`, `tileWidth`, `tileHeight`, `origin`,
`maxTiles`, `maxPolys`, `polys`, `verts`, `links`, `doorParts`, `doorPolys`, `obstacles`, `agent`
`{height, radius, climb}`, `cellSizes` `[[cellSize, tiles]]`, `areas` `{areaId: polys}`, `flags`
`{flags: polys}` (keys are decimal strings).

`--tiles` adds `tileList`: one `[x, z, bytes, polys, links, obstacles, doorParts]` array per tile.
For a whole map this line is a few MB.

## `tile <file.nm> <x> <z>`

One `tileDetail` event: `x`, `z`, `ref` (hex), `bmin`, `bmax`, `cellSize`, `bytes`, `polys` and
`verts` (including off-mesh link polys), `areas` and `flags` (ground polys only, as in `info`),
`links` `[{class, bidirectional, from: [x,y,z], to: [x,y,z], centerHeight, radius, area, flags}]`,
`doors` `[{objectId (hex), part, state (0/1), polys}]`, `obstacles` `[[x, y, z, radius]]`.

## `render <file.nm> <out.png> <minX> <minZ> <maxX> <maxZ> [px/m]`

`{"event":"render","output":"out.png","width":200,"height":200}`

## `verify <reference.nm> <ours.nm> [--ignore-order]`

`{"event":"verify","compared":49,"identical":49,"ignoredIsolated":true,"ignoredOrder":false,"different":[]}`.
Tiles are compared in canonical form (see `canonicalTile` and [validation.md](validation.md));
`ignoredIsolated` is true when the reference has no isolated polygons (a partial save) and the
flag was therefore ignored; `ignoredOrder` echoes `--ignore-order` (tree obstacles and door parts
compared as sets). Exit code 1 if any tile differs.

## `diff <a.nm> <b.nm> [top N]`

One `diff` event: `compared`, `onlyInA`, `onlyInB`, `sameCellSize`, `a` and `b` `{polys, links,
obstacles, doorParts}`, `areaM2` `{areaId: [a, b]}` (walkable area in m²), `mismatchPercent`,
`worst` `[{x, z, areaDelta, polys: [a,b], links: [a,b], cellSize: [a,b]}]` (the N tiles with the
largest area difference).

## `merge`, `islands`

`{"event":"merge","replaced":12,"output":"out.nm"}`

`{"event":"islands","seedFound":true,"seedPoint":[x,y,z],"reachable":N,"isolated":M,"output":"out.nm"}`
(no polygon near the seed is an `error` event).
