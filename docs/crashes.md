# Crash fixes

Inputs that can crash a navmesh generator built the way DayZ's navmeshes are, and how dznav
handles them.

## Door part lookup with a tree id

Each polygon is attached to its door part with `parts[subAreas[i]]`. The span part id field is
shared by door parts and tree obstacles, and the sea pass relabels tree-tagged spans below sea
level as `waterdeepsea` without clearing the id. On a tile that has both such trees and at least
one door, an area-5 polygon carries a tree index larger than the door part count, so an unchecked
lookup reads past the table. Shorelines with trees and buildings have these tiles.

dznav ignores out-of-range part ids: those polygons get no door part, and `dznav generate` says
how many tiles had them. Every other polygon comes out the same.

## Navmeshes that can't be read

A `.nm` that's missing, has the wrong version or is truncated stops with an error that names the
problem (bad magic, unsupported version, tile data out of range), before anything uses it.

## Network input

What the data server sends is checked before it's used: a frame with an invalid length, a tile
answered for other coordinates than the ones asked for, or a tile with an unknown cell class ends
the fetch with an error instead of indexing past an array.
