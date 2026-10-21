# Validation

The goal is byte-identical tiles. `dznav verify <reference.nm> <ours.nm>` compares the tiles both
files have, in canonical form: it ignores what depends on neighbouring tiles or load order (links
between tiles, snapped off-mesh endpoints, a reserved off-mesh float, obstacle Y), the door
"disabled" bit (random in reference files) and, when the reference has none (a partial save), the
unreachable-area flag.

## Same input, or not?

The data server sends the world as it is at that moment, so two runs don't always get the same
input:

- **Tide.** The sea level changes over time. Pin it with `--sea-level` to match a reference.
- **Animated objects.** Some objects move, such as large rotating geometry. The server sends them
  in their current pose, so tiles around them differ from run to run.
- **Object order.** A fresh DayZDiag session can list objects in a different order. That changes
  the order of tree obstacles and door parts in a tile, not their contents.

So a difference against a reference made in another DayZDiag session isn't necessarily ours. To
compare on exactly the same input, build both from captured geometry.

## Replaying a capture

```sh
dznav capture area.dzgeo 100 40 120 60                 # from the live server
dznav replay-server area.dzgeo --bind 127.0.0.2 --crop --sea-level 0.65 --no-island-filter --once
                                                       # another client builds reference.nm from it
dznav generate ours.nm --capture area.dzgeo --sea-level 0.65 --no-island-filter
dznav verify reference.nm ours.nm
```

- `--bind 127.0.0.2` lets the replay server run next to a DayZDiag that holds port 36533 on all
  addresses.
- `--crop` shrinks the world to the captured tiles' grid, so a client that generates the whole
  world only requests those. Tiles outside the capture get flat seabed, so none is without
  terrain.
- `--once` stops the server when the client disconnects.

## Results (October 2026)

**Same input.** 52 tiles built from replayed captures (all the tiles that had differed, in every
category) are byte-identical to the reference, as is the 49-tile fixture.

**Against reference saves** of rows 0–79 (26,160 tiles from two earlier DayZDiag sessions, sea
level pinned to each session's tide):

| Rows | Byte-identical | With `--ignore-order` | What's left |
|---|---|---|---|
| 40–79 | 13,069 of 13,080 | 13,080 of 13,080 | nothing |
| 0–39 | 13,040 of 13,080 | 13,046 of 13,080 | 34 tiles around animated objects |

The remaining 34 tiles change from capture to capture, and on identical input they match too.

**DayZ 1.30.** A 49-tile range built by the 1.30 tools from the same DayZDiag session (sea level
pinned to 0.60) is byte-identical, object order included, with every jump link the same.

## Bugs found this way

1. **Jump-link sample points** (about 1 % of tiles). The sampler computes each sample's far end
   as `dir·t + (n·h + A)`, with `n·h + A` computed once per edge. Computing it as
   `(dir·t + n·h) + A` rounds differently, so a sample one float step from a cell boundary could
   land in the neighbouring cell and lengthen or shorten a link by 0.1 m. Found by recomputing a
   reference link bit for bit from our edge.
2. **Door parts at the heightfield's min edge** (rare, but it can remove every link in a
   tile). DayZ's part rasteriser skips any cell piece that has a vertex beyond the heightfield's
   min x/z edge; ours painted column 0 there. The door paint then made the ledge filter drop that
   span, erosion cut one cell deeper, and the link-stage region filter discarded a large group of
   regions, leaving the tile with no links at all (tile 108,76: 0 links instead of 73).
