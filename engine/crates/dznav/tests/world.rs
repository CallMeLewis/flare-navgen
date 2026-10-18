//! The data-server wire format and capture files.

mod common;

use dznav::world::{CellClass, TileInput, WorldParams};
use dznav::{Capture, CaptureWriter, Error};

fn wire_error(r: Result<impl std::fmt::Debug, Error>) -> String {
    match r {
        Err(Error::Wire(m)) => m,
        other => panic!("expected a wire error, got {other:?}"),
    }
}

#[test]
fn world_params_round_trip() {
    let p = common::world_params();
    let body = p.serialize();
    assert_eq!(body.len(), 0x54 + 4 + 2 * 16 + 4 + 0x34);
    assert_eq!(WorldParams::parse(&body).unwrap(), p);
}

#[test]
fn world_params_are_checked() {
    let p = common::world_params();
    let mut extra = p.serialize();
    extra.push(0);
    assert_eq!(wire_error(WorldParams::parse(&extra)), "unexpected bytes after world params");
    assert_eq!(wire_error(WorldParams::parse(&p.serialize()[..0x50])), "message truncated");

    let mut bad = p.clone();
    bad.block.tiles_x = 0;
    assert_eq!(wire_error(WorldParams::parse(&bad.serialize())), "invalid tile grid");
    let mut bad = p.clone();
    bad.block.tile_height = f32::NAN;
    assert_eq!(wire_error(WorldParams::parse(&bad.serialize())), "invalid tile size");
    let mut bad = p.clone();
    bad.cell_classes.push(CellClass { cells: 0, ..Default::default() });
    assert_eq!(wire_error(WorldParams::parse(&bad.serialize())), "invalid cell class");
    let mut bad = p.serialize();
    bad[0x54..0x58].copy_from_slice(&65u32.to_le_bytes());
    assert_eq!(wire_error(WorldParams::parse(&bad)), "implausible cell class count 65");
}

#[test]
fn tile_input_round_trip() {
    let t = common::tile_input(1, 1);
    let body = t.serialize();
    assert_eq!(TileInput::parse(&body).unwrap(), t);
    let mut two = body.clone();
    two.extend_from_slice(&body);
    let (first, consumed) = TileInput::parse_prefix(&two).unwrap();
    assert_eq!((first, consumed), (t, body.len()));
    assert_eq!(wire_error(TileInput::parse(&two)), "unexpected bytes after tile record");
}

#[test]
fn tile_input_is_checked() {
    let mut t = common::tile_input(0, 0);
    let door = t.groups.iter().position(|g| !g.parts.is_empty()).unwrap();
    t.groups[door].parts[0].first_vertex = 1 << 20;
    assert_eq!(wire_error(TileInput::parse(&t.serialize())), "part vertex range out of bounds");

    let mut t = common::tile_input(0, 0);
    t.groups[0].verts[4] = f32::INFINITY;
    assert_eq!(wire_error(TileInput::parse(&t.serialize())), "non-finite vertex");

    let body = common::tile_input(0, 0).serialize();
    assert_eq!(wire_error(TileInput::parse(&body[..body.len() - 2])), "message truncated");
}

#[test]
fn capture_round_trip() {
    let dir = common::scratch_dir("capture_round_trip");
    let path = dir.join("world.dzgeo");
    let params = common::world_params();
    let tiles = common::tiles();
    let mut w = CaptureWriter::create(&path, &params).unwrap();
    for t in &tiles {
        w.add(t).unwrap();
    }
    drop(w);
    let c = Capture::read(&path).unwrap();
    assert_eq!(c, Capture { params, tiles });
}

#[test]
fn cut_short_capture_keeps_complete_tiles() {
    let dir = common::scratch_dir("cut_short_capture");
    let path = dir.join("world.dzgeo");
    let mut w = CaptureWriter::create(&path, &common::world_params()).unwrap();
    w.add(&common::tile_input(0, 0)).unwrap();
    w.add(&common::tile_input(1, 0)).unwrap();
    drop(w);
    let bytes = std::fs::read(&path).unwrap();
    let c = Capture::parse(&bytes[..bytes.len() - 10]).unwrap();
    assert_eq!(c.tiles, [common::tile_input(0, 0)]);

    assert_eq!(wire_error(Capture::parse(b"DZGC\x02\0\0\0\0\0\0\0")), "unsupported capture version");
    assert_eq!(wire_error(Capture::parse(b"NOPE\x01\0\0\0\0\0\0\0")), "not a capture file");
    assert_eq!(wire_error(Capture::parse(&bytes[..40])), "capture truncated");
}
