//! Checks against shipped navmeshes, when `DZNAV_SAMPLE_NM` lists some. They are large: run these with --release.

mod common;

use std::collections::HashMap;

use dznav::format::{Link, MeshHeader, Poly};
use dznav::{NavMeshAssembly, NavMeshFile, NavMeshReader, Tile};

#[test]
fn round_trip_is_byte_identical() {
    for path in common::sample_navmeshes() {
        let bytes = std::fs::read(&path).unwrap();
        let file = NavMeshFile::parse(&bytes).unwrap();
        // Older files are saved with the current set version.
        let mut saved = file.serialize().unwrap();
        saved[4..8].copy_from_slice(&bytes[4..8]);
        assert!(saved == bytes, "{} changed in a round trip", path.display());

        let reader = NavMeshReader::open(&path).unwrap();
        assert_eq!(reader.tiles().len(), file.tiles.len());
        for (i, t) in file.tiles.iter().enumerate() {
            assert_eq!(reader.find(t.header.x, t.header.y), Some(i));
            assert!(reader.tile_bytes(i) == t.serialize());
        }
        eprintln!("{}: {} tiles, {} bytes identical", path.display(), file.tiles.len(), bytes.len());
    }
}

/// Loads every tile into the patched Detour with its links zeroed, lets Detour rebuild them all, and compares the
/// tiles with the file. Identical bytes mean the patch links tiles exactly like DayZ's.
#[test]
fn relinking_matches_shipped_links() {
    for path in common::sample_navmeshes() {
        let reader = NavMeshReader::open(&path).unwrap();
        let h = *reader.header();
        let mut mesh = NavMeshAssembly::new(h.tiles_x, h.tiles_z, h.tile_width, h.tile_height, h.orig).unwrap();
        let mut data = Vec::new();
        for (i, e) in reader.tiles().iter().enumerate() {
            data.clear();
            data.extend_from_slice(reader.tile_bytes(i));
            let mh: MeshHeader = bytemuck::pod_read_unaligned(&data[..size_of::<MeshHeader>()]);
            let links =
                size_of::<MeshHeader>() + mh.vert_count as usize * 12 + mh.poly_count as usize * size_of::<Poly>();
            data[links..links + mh.max_link_count as usize * size_of::<Link>()].fill(0);
            assert_eq!(mesh.add_tile(&data, e.tile_ref).unwrap(), e.tile_ref);
        }

        let relinked: HashMap<u64, &[u8]> = mesh.tiles().collect();
        let (mut differ, mut obstacle_y_only) = (0, 0);
        for (i, e) in reader.tiles().iter().enumerate() {
            let (ours, theirs) = (relinked[&e.tile_ref], reader.tile_bytes(i));
            if ours == theirs {
                continue;
            }
            // Adding a tile snaps obstacle Y to the mesh, and re-snapping an already snapped Y isn't always
            // idempotent: accept tiles whose only difference is obstacle Y.
            let flatten = |b: &[u8]| {
                let mut t = Tile::parse(b, e.tile_ref).unwrap();
                t.obstacles.iter_mut().for_each(|o| o.pos[1] = 0.0);
                t.serialize()
            };
            if flatten(ours) == flatten(theirs) {
                obstacle_y_only += 1;
            } else {
                differ += 1;
                if differ <= 5 {
                    let at = ours.iter().zip(theirs).position(|(a, b)| a != b).unwrap_or(ours.len());
                    eprintln!("tile {i} ({}, {}): first difference at byte {at} of {}", e.x(), e.z(), theirs.len());
                }
            }
        }
        eprintln!(
            "{}: {differ} / {} tiles differ after relinking ({obstacle_y_only} more only in re-snapped obstacle Y)",
            path.display(),
            reader.tiles().len()
        );
        assert_eq!(differ, 0, "{}", path.display());
    }
}

/// `resave` (load into Detour, export) must write the same bytes as the C++ engine.
#[test]
fn resave_matches_cpp() {
    let Some(oracle) = common::oracle() else { return };
    let dir = common::scratch_dir("resave_matches_cpp");
    for path in common::sample_navmeshes() {
        let (theirs, ours) = (dir.join("cpp.nm"), dir.join("rust.nm"));
        let status = std::process::Command::new(&oracle).arg("resave").arg(&path).arg(&theirs).output().unwrap();
        assert!(status.status.success(), "C++ resave failed: {}", String::from_utf8_lossy(&status.stderr));
        let mesh = NavMeshAssembly::load(&NavMeshFile::read(&path).unwrap()).unwrap();
        mesh.to_file().unwrap().write(&ours).unwrap();
        assert!(std::fs::read(&ours).unwrap() == std::fs::read(&theirs).unwrap(), "{}", path.display());
        eprintln!("{}: resaved identically", path.display());
        std::fs::remove_file(theirs).unwrap();
        std::fs::remove_file(ours).unwrap();
    }
}
