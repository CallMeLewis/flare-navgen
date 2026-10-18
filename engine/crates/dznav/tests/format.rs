//! The .nm format and the navmesh reader, on hand-made data.

use dznav::format::*;
use dznav::{Error, NavMeshFile, NavMeshReader, Tile};

fn make_tile(x: i32, y: i32) -> Tile {
    let mut t = Tile {
        tile_ref: encode_poly_ref(1, (y * 2 + x) as u32, 0),
        header: MeshHeader {
            x,
            y,
            walkable_height: 1.5,
            walkable_radius: 0.3,
            walkable_climb: 0.45,
            bv_quant_factor: 4.0,
            off_mesh_base: 1,
            ..Default::default()
        },
        verts: vec![0.0, 0.0, 0.0, 50.0, 0.0, 0.0, 50.0, 0.0, 50.0],
        polys: vec![Poly {
            verts: [0, 1, 2, 0, 0, 0],
            vert_count: 3,
            flags: 1,
            area_and_type: 1,
            ..Default::default()
        }],
        links: vec![Link::default(); 3],
        bv_tree: vec![BvNode::default(); 2],
        parts: vec![TilePart { object_id: 0x8000_f000_0501_f61f, part_index: 0, first_poly: 0 }],
        part_polys: vec![TilePartPoly { poly: 0, next: -1 }],
        obstacles: vec![Obstacle { pos: [10.0, 0.0, 10.0], radius: 0.3 }],
        ..Default::default()
    };
    t.sync_header();
    t
}

fn make_file() -> NavMeshFile {
    NavMeshFile {
        header: SetHeader {
            magic: SET_MAGIC,
            version: SET_VERSION,
            num_tiles: 2,
            tiles_x: 2,
            tiles_z: 1,
            tile_width: 50.0,
            tile_height: 50.0,
            max_tiles: 2,
            max_polys: 1 << POLY_BITS,
            ..Default::default()
        },
        tiles: vec![make_tile(0, 0), make_tile(1, 0)],
    }
}

fn format_error(r: Result<impl std::fmt::Debug, Error>) -> String {
    match r {
        Err(Error::Format(m)) => m,
        other => panic!("expected a format error, got {other:?}"),
    }
}

#[test]
fn poly_refs() {
    assert_eq!(ref_salt(encode_poly_ref(1, 5, 7)), 1);
    assert_eq!(ref_tile(encode_poly_ref(1, 5, 7)), 5);
    assert_eq!(ref_poly(encode_poly_ref(1, 5, 7)), 7);
    assert_eq!(encode_poly_ref(1, 1, 0), 0x0001_0000_0010_0000);
}

#[test]
fn round_trip() {
    let bytes = make_file().serialize().unwrap();
    assert_eq!(&bytes[..4], b"TESM");
    assert_eq!(&bytes[48 + 8 + 16..][..4], b"VAND");
    let g = NavMeshFile::parse(&bytes).unwrap();
    assert_eq!(g.tiles.len(), 2);
    assert_eq!(g.tiles[1].header.x, 1);
    assert_eq!(g.tiles[0].obstacles.len(), 1);
    assert_eq!(g.tiles[0].parts[0].object_id, 0x8000_f000_0501_f61f);
    assert_eq!(g.serialize().unwrap(), bytes);
    assert_eq!(g.tiles[0].header.extra_data_size, 12 + 16 + 8 + 16);
}

#[test]
fn offset_table_points_at_records_by_slot() {
    let mut f = make_file();
    f.header.max_tiles = 4;
    f.tiles[0].tile_ref = encode_poly_ref(1, 3, 0);
    f.tiles[1].tile_ref = encode_poly_ref(1, 1, 0);
    let bytes = f.serialize().unwrap();
    let table: Vec<u32> = bytes[48..48 + 16].chunks(4).map(|c| u32::from_le_bytes(c.try_into().unwrap())).collect();
    let record = 16 + f.tiles[0].data_size() as u32;
    assert_eq!(table, [NO_TILE, record, NO_TILE, 0]);
}

#[test]
fn record_padding_survives() {
    let mut f = make_file();
    f.tiles[1].record_pad = 0xdead_beef;
    let bytes = f.serialize().unwrap();
    assert_eq!(NavMeshFile::parse(&bytes).unwrap().tiles[1].record_pad, 0xdead_beef);
}

#[test]
fn reader_indexes_tiles() {
    let f = make_file();
    let bytes = f.serialize().unwrap();
    let reader = NavMeshReader::from_bytes(&bytes).unwrap();
    assert_eq!(reader.tiles().len(), 2);
    assert_eq!(reader.find(1, 0), Some(1));
    assert_eq!(reader.find(0, 1), None);
    let e = reader.tiles()[1];
    assert_eq!((e.x(), e.z(), e.tile_ref), (1, 0, f.tiles[1].tile_ref));
    assert_eq!(e.header, f.tiles[1].header);
    assert_eq!(reader.tile_bytes(1), f.tiles[1].serialize());
    assert_eq!(reader.tile_at(1, 0).unwrap().unwrap(), f.tiles[1]);
    assert_eq!(reader.read_all().unwrap(), f);
}

#[test]
fn reader_opens_files() {
    let path = std::path::PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("reader_opens_files.nm");
    let f = make_file();
    f.write(&path).unwrap();
    let reader = NavMeshReader::open(&path).unwrap();
    assert_eq!(reader.read_all().unwrap(), f);
    drop(reader);
    std::fs::write(&path, b"").unwrap();
    assert_eq!(format_error(NavMeshReader::open(&path).map(|_| ())), "unexpected end of data");
}

#[test]
fn rejects_bad_files() {
    let bytes = make_file().serialize().unwrap();
    let with = |offset: usize, value: &[u8]| {
        let mut b = bytes.clone();
        b[offset..offset + value.len()].copy_from_slice(value);
        NavMeshFile::parse(&b)
    };
    let tile = 48 + 8 + 16;
    assert_eq!(format_error(with(0, b"XXXX")), "Invalid header format (bad magic)");
    assert_eq!(format_error(with(4, &8i32.to_le_bytes())), "Invalid version 8");
    assert_eq!(format_error(with(8, &3i32.to_le_bytes())), "numTiles exceeds maxTiles");
    assert_eq!(format_error(with(48 + 8, &0u64.to_le_bytes())), "empty tile record (the game rejects these)");
    assert_eq!(format_error(with(48 + 8 + 8, &i32::MAX.to_le_bytes())), "tile data out of range");
    assert_eq!(format_error(with(tile, b"XXXX")), "bad tile magic");
    assert_eq!(format_error(with(tile + 4, &11i32.to_le_bytes())), "unsupported tile version 11");
    assert_eq!(format_error(with(tile + 24, &(-1i32).to_le_bytes())), "negative polyCount");
    assert_eq!(format_error(with(tile + 100, &99i32.to_le_bytes())), "extra section size mismatch");
    assert_eq!(format_error(NavMeshFile::parse(&bytes[..bytes.len() - 1])), "tile data out of range");
    assert_eq!(format_error(NavMeshFile::parse(&bytes[..20])), "unexpected end of data");
    assert_eq!(
        format_error(Tile::parse(&[bytes[tile..].to_vec(), vec![0]].concat(), 1)),
        "trailing bytes after tile data"
    );
}

#[test]
fn serialize_checks_slots() {
    let mut f = make_file();
    f.tiles[1].tile_ref = encode_poly_ref(1, 2, 0);
    assert_eq!(format_error(f.serialize()), "tile ref outside maxTiles");
    f.header.max_tiles = 1;
    assert_eq!(format_error(f.serialize()), "more tiles than maxTiles");
}
