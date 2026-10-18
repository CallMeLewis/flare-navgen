//! Geometry capture files (.dzgeo): world parameters plus captured tile inputs, so generation can be re-run offline
//! without DayZDiag.
//!
//! Layout: "DZGC", u32 version (1), u32 size + world params body, then repeated u32 size + tile body (bodies use the
//! data-server wire format).

use std::fs::File;
use std::io::Write;
use std::path::{Path, PathBuf};

use crate::error::{Result, io_err, wire_err};
use crate::world::{TileInput, WorldParams};

const MAGIC: &[u8; 4] = b"DZGC";
const VERSION: u32 = 1;

/// Records tile inputs as they arrive. Each tile is written straight through, so a capture cut short keeps every
/// complete tile.
pub struct CaptureWriter {
    file: File,
    path: PathBuf,
}

fn block(out: &mut Vec<u8>, body: &[u8]) {
    out.extend_from_slice(&(body.len() as u32).to_le_bytes());
    out.extend_from_slice(body);
}

impl CaptureWriter {
    pub fn create(path: impl AsRef<Path>, params: &WorldParams) -> Result<Self> {
        let path = path.as_ref().to_path_buf();
        let mut file = File::create(&path).map_err(io_err("create", &path))?;
        let mut head = Vec::new();
        head.extend_from_slice(MAGIC);
        head.extend_from_slice(&VERSION.to_le_bytes());
        block(&mut head, &params.serialize());
        file.write_all(&head).map_err(io_err("write", &path))?;
        Ok(Self { file, path })
    }

    pub fn add(&mut self, tile: &TileInput) -> Result<()> {
        let mut out = Vec::new();
        block(&mut out, &tile.serialize());
        self.file.write_all(&out).map_err(io_err("write", &self.path))
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Capture {
    pub params: WorldParams,
    pub tiles: Vec<TileInput>,
}

impl Capture {
    pub fn read(path: impl AsRef<Path>) -> Result<Capture> {
        let path = path.as_ref();
        Self::parse(&std::fs::read(path).map_err(io_err("read", path))?)
    }

    pub fn parse(bytes: &[u8]) -> Result<Capture> {
        if bytes.len() < 12 || &bytes[..4] != MAGIC {
            return wire_err("not a capture file");
        }
        if u32::from_le_bytes(bytes[4..8].try_into().unwrap()) != VERSION {
            return wire_err("unsupported capture version");
        }
        // The next block, or None if it isn't all there.
        let block_at = |pos: usize| -> Option<&[u8]> {
            let n = u32::from_le_bytes(bytes.get(pos..pos + 4)?.try_into().unwrap()) as usize;
            bytes.get(pos + 4..pos + 4 + n)
        };
        let Some(params) = block_at(8) else {
            return wire_err("capture truncated");
        };
        let mut capture = Capture { params: WorldParams::parse(params)?, tiles: Vec::new() };
        // A capture interrupted mid-write keeps every complete tile.
        let mut pos = 12 + params.len();
        while let Some(body) = block_at(pos) {
            capture.tiles.push(TileInput::parse(body)?);
            pos += 4 + body.len();
        }
        Ok(capture)
    }
}
