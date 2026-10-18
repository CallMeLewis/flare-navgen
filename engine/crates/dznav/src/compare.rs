//! Tile-by-tile comparison of two navmeshes, used to validate generated output against shipped maps.

use std::collections::{BTreeMap, BTreeSet, HashMap};

use crate::format::{ISOLATED_FLAG, Link, NavMeshFile, Tile, TilePart, TilePartPoly};

#[derive(Clone, Debug, Default, PartialEq)]
pub struct TileSummary {
    pub x: i32,
    pub z: i32,
    pub polys: i32,
    pub verts: i32,
    pub off_mesh: i32,
    pub obstacles: usize,
    pub parts: usize,
    pub cell_size: f32,
    /// XZ area (m²) of ground polys per area id.
    pub area_by_type: BTreeMap<u8, f64>,
}

impl TileSummary {
    pub fn walkable_area(&self) -> f64 {
        self.area_by_type.values().sum()
    }
}

pub fn summarize_tile(t: &Tile) -> TileSummary {
    let h = &t.header;
    let mut s = TileSummary {
        x: h.x,
        z: h.y,
        polys: h.off_mesh_base,
        verts: h.vert_count,
        off_mesh: h.off_mesh_con_count,
        obstacles: t.obstacles.len(),
        parts: t.parts.len(),
        cell_size: if h.bv_quant_factor > 0.0 { 1.0 / h.bv_quant_factor } else { 0.0 },
        area_by_type: BTreeMap::new(),
    };
    for p in t.polys.iter().filter(|p| p.poly_type() == 0) {
        let n = p.vert_count as usize;
        let mut a = 0.0f64;
        for k in 0..n {
            let u = &t.verts[p.verts[k] as usize * 3..];
            let v = &t.verts[p.verts[(k + 1) % n] as usize * 3..];
            a += f64::from(u[0]) * f64::from(v[2]) - f64::from(v[0]) * f64::from(u[2]);
        }
        *s.area_by_type.entry(p.area()).or_default() += a.abs() * 0.5;
    }
    s
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct TileDiff {
    pub a: TileSummary,
    pub b: TileSummary,
    /// Sum over area ids of |a - b|.
    pub area_delta: f64,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct CompareResult {
    /// Tiles present in both files.
    pub tiles: Vec<TileDiff>,
    pub only_in_a: usize,
    pub only_in_b: usize,
    pub total_area_a: BTreeMap<u8, f64>,
    pub total_area_b: BTreeMap<u8, f64>,
}

fn by_cell(file: &NavMeshFile) -> HashMap<(i32, i32), &Tile> {
    // Like std::map assignment in the C++ engine: a later tile at the same cell wins.
    file.tiles.iter().map(|t| ((t.header.x, t.header.y), t)).collect()
}

pub fn compare_navmeshes(a: &NavMeshFile, b: &NavMeshFile) -> CompareResult {
    let mut r = CompareResult::default();
    let b_tiles = by_cell(b);
    let mut seen = BTreeSet::new();
    for ta in &a.tiles {
        let key = (ta.header.x, ta.header.y);
        let Some(tb) = b_tiles.get(&key) else {
            r.only_in_a += 1;
            continue;
        };
        seen.insert(key);
        let (sa, sb) = (summarize_tile(ta), summarize_tile(tb));
        let mut areas = BTreeSet::new();
        for (&id, &v) in &sa.area_by_type {
            areas.insert(id);
            *r.total_area_a.entry(id).or_default() += v;
        }
        for (&id, &v) in &sb.area_by_type {
            areas.insert(id);
            *r.total_area_b.entry(id).or_default() += v;
        }
        let area_delta = areas
            .iter()
            .map(|id| {
                (sa.area_by_type.get(id).copied().unwrap_or(0.0) - sb.area_by_type.get(id).copied().unwrap_or(0.0))
                    .abs()
            })
            .sum();
        r.tiles.push(TileDiff { a: sa, b: sb, area_delta });
    }
    r.only_in_b = b.tiles.len() - seen.len();
    r
}

/// Sorts tree obstacles and door parts (keeping each part's polygon list), so tiles whose objects were listed in a
/// different order compare equal.
fn sort_objects(t: &mut Tile) {
    t.obstacles.sort_by(|a, b| {
        (a.pos[0], a.pos[2], a.radius).partial_cmp(&(b.pos[0], b.pos[2], b.radius)).unwrap_or(std::cmp::Ordering::Equal)
    });
    let mut parts: Vec<(u64, u32, Vec<u32>)> = t
        .parts
        .iter()
        .map(|p| {
            let mut polys = Vec::new();
            let mut k = p.first_poly;
            while k >= 0 && (k as usize) < t.part_polys.len() {
                polys.push(t.part_polys[k as usize].poly);
                k = t.part_polys[k as usize].next;
            }
            (p.object_id, p.part_index, polys)
        })
        .collect();
    parts.sort_by_key(|p| (p.0, p.1));
    t.parts.clear();
    t.part_polys.clear();
    for (object_id, part_index, polys) in parts {
        let first_poly = if polys.is_empty() { -1 } else { t.part_polys.len() as i32 };
        t.parts.push(TilePart { object_id, part_index, first_poly });
        for (i, &poly) in polys.iter().enumerate() {
            let next = if i + 1 < polys.len() { t.part_polys.len() as i32 + 1 } else { -1 };
            t.part_polys.push(TilePartPoly { poly, next });
        }
    }
}

/// A tile without what depends on neighbouring tiles or load order (links, firstLink, snapped off-mesh endpoints, the
/// reserved off-mesh float, obstacle Y) and the door "disabled" bit (random in reference files); optionally without
/// the isolated flag, and with objects in a canonical order.
pub fn canonical_tile(tile: &Tile, ignore_isolated: bool, ignore_order: bool) -> Tile {
    let mut t = tile.clone();
    t.tile_ref = 0;
    t.record_pad = 0;
    t.links.fill(Link::default());
    for p in &mut t.polys {
        p.first_link = 0;
        if p.area() == 12 || p.area() == 13 {
            p.flags &= !2;
        }
        if ignore_isolated {
            p.flags &= !ISOLATED_FLAG;
        }
    }
    for c in &mut t.off_mesh_cons {
        let p = t.polys[c.poly as usize];
        let (a, b) = (p.verts[0] as usize * 3, p.verts[1] as usize * 3);
        t.verts[a..a + 3].copy_from_slice(&c.pos[..3]);
        t.verts[b..b + 3].copy_from_slice(&c.pos[3..]);
        c.reserved = 0.0;
    }
    for o in &mut t.obstacles {
        o.pos[1] = 0.0;
    }
    if ignore_order {
        sort_objects(&mut t);
    }
    t
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct VerifyResult {
    pub compared: usize,
    pub identical: usize,
    pub ignored_isolated: bool,
    pub ignored_order: bool,
    /// Tile coordinates.
    pub different: Vec<(i32, i32)>,
}

/// Byte comparison of tiles present in both files, in canonical form (see [`canonical_tile`]). If the reference has
/// no isolated polygons (a partial save), the isolated flag is ignored too. With `ignore_order`, tree obstacles and
/// door parts compare as sets (a new DayZDiag session can list objects in another order).
pub fn verify_tiles(reference: &NavMeshFile, candidate: &NavMeshFile, ignore_order: bool) -> VerifyResult {
    let refs = by_cell(reference);
    // The island filter runs only after the last tile, so a partial save has no isolated polygons; compare those
    // without the flag.
    let ignored_isolated = !reference.tiles.iter().any(|t| t.polys.iter().any(|p| p.flags & ISOLATED_FLAG != 0));
    let mut r = VerifyResult { ignored_isolated, ignored_order: ignore_order, ..Default::default() };
    for t in &candidate.tiles {
        let Some(rt) = refs.get(&(t.header.x, t.header.y)) else { continue };
        r.compared += 1;
        let canonical = |t: &Tile| canonical_tile(t, ignored_isolated, ignore_order).serialize();
        if canonical(rt) == canonical(t) {
            r.identical += 1;
        } else {
            r.different.push((t.header.x, t.header.y));
        }
    }
    r
}
