//! Random access to a navmesh's tiles without loading the whole file.
//!
//! Shipped navmeshes are 400 MB to 1.3 GB. Opening one maps it into memory and walks the record headers to index its
//! tiles (one small read per tile); a tile is only parsed when asked for.

use std::collections::HashMap;
use std::fs::File;
use std::path::Path;
use std::sync::Arc;

use memmap2::Mmap;

use crate::bytes::Cursor;
use crate::error::{Error, Result, format_err, io_err};
use crate::format::{
    MeshHeader, NavMeshFile, SET_MAGIC, SET_VERSION, SET_VERSION_OLD, SetHeader, Tile, TileRecordHeader, check_tile_header,
};

/// Where one tile's record is in the file, with the tile's header.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TileEntry {
    pub tile_ref: u64,
    pub record_pad: u32,
    pub header: MeshHeader,
    /// Byte offset of the tile data (after the record header).
    pub offset: usize,
    pub data_size: usize,
}

impl TileEntry {
    pub fn x(&self) -> i32 {
        self.header.x
    }

    /// The grid row (`y` in the tile header).
    pub fn z(&self) -> i32 {
        self.header.y
    }
}

/// Where every tile of a navmesh file is. It can be kept while the file is closed, and used again with the same
/// bytes (see [`NavMeshReader::with_index`]).
#[derive(Clone, Debug)]
pub struct NavMeshIndex {
    pub header: SetHeader,
    /// Every tile, in file order.
    pub tiles: Vec<TileEntry>,
    /// The file's size.
    pub len: usize,
    by_cell: HashMap<(i32, i32), usize>,
}

impl NavMeshIndex {
    /// Indexes a navmesh's bytes, checking every record and tile header.
    pub fn build(data: &[u8]) -> Result<NavMeshIndex> {
        let mut r = Cursor::new(data, Error::Format);
        let header: SetHeader = r.read()?;
        if header.magic != SET_MAGIC {
            return format_err("Invalid header format (bad magic)");
        }
        if header.version != SET_VERSION && header.version != SET_VERSION_OLD {
            return format_err(format!("Invalid version {}", header.version));
        }
        if header.num_tiles < 0 {
            return format_err("negative numTiles");
        }
        if header.max_tiles < header.num_tiles {
            return format_err("numTiles exceeds maxTiles");
        }
        // The game skips the slot-indexed offset table and reads numTiles records back to back.
        r.take(header.max_tiles as usize * 4)?;

        let mut pos = r.pos();
        let mut tiles = Vec::with_capacity(header.num_tiles as usize);
        let mut by_cell = HashMap::with_capacity(header.num_tiles as usize);
        for _ in 0..header.num_tiles {
            let Some(record) = data.get(pos..pos + size_of::<TileRecordHeader>()) else {
                return format_err("tile record out of range");
            };
            let record: TileRecordHeader = bytemuck::pod_read_unaligned(record);
            if record.tile_ref == 0 || record.data_size <= 0 {
                return format_err("empty tile record (the game rejects these)");
            }
            let offset = pos + size_of::<TileRecordHeader>();
            let data_size = record.data_size as usize;
            if offset + data_size > data.len() {
                return format_err("tile data out of range");
            }
            let mesh: MeshHeader = Cursor::new(&data[offset..offset + data_size], Error::Format).read()?;
            check_tile_header(&mesh)?;
            by_cell.entry((mesh.x, mesh.y)).or_insert(tiles.len());
            tiles.push(TileEntry {
                tile_ref: record.tile_ref,
                record_pad: record.pad,
                header: mesh,
                offset,
                data_size,
            });
            pos = offset + data_size;
        }
        Ok(NavMeshIndex { header, tiles, len: data.len(), by_cell })
    }

    /// The index of the tile at a grid cell (the first one in the file, if there are several).
    pub fn find(&self, x: i32, z: i32) -> Option<usize> {
        self.by_cell.get(&(x, z)).copied()
    }
}

/// A navmesh file indexed by tile. `B` holds the file's bytes: a memory map from [`NavMeshReader::open`], or any byte
/// buffer.
pub struct NavMeshReader<B = Mmap> {
    bytes: B,
    index: Arc<NavMeshIndex>,
}

/// Maps a file for reading.
fn map(path: &Path) -> Result<Mmap> {
    let file = File::open(path).map_err(io_err("open", path))?;
    if file.metadata().map_err(io_err("read", path))?.len() == 0 {
        return format_err("unexpected end of data");
    }
    // SAFETY: the map is only read, and every read is bounds-checked against the length it had when mapped.
    // Windows refuses to truncate a mapped file; if another process rewrites its bytes meanwhile, tiles read
    // after that can be inconsistent but are still validated before use.
    unsafe { Mmap::map(&file) }.map_err(io_err("map", path))
}

impl NavMeshReader<Mmap> {
    /// Maps a navmesh file and indexes its tiles.
    ///
    /// The file stays mapped, and so open, until the reader is dropped. On Windows that also means nothing can replace
    /// or truncate it in the meantime.
    pub fn open(path: impl AsRef<Path>) -> Result<Self> {
        Self::from_bytes(map(path.as_ref())?)
    }

    /// Maps a navmesh file indexed before. Fails if its size has changed since.
    pub fn open_indexed(path: impl AsRef<Path>, index: Arc<NavMeshIndex>) -> Result<Self> {
        Self::with_index(map(path.as_ref())?, index)
    }
}

impl<B: AsRef<[u8]>> NavMeshReader<B> {
    /// Indexes a navmesh held in memory.
    pub fn from_bytes(bytes: B) -> Result<Self> {
        let index = Arc::new(NavMeshIndex::build(bytes.as_ref())?);
        Ok(Self { bytes, index })
    }

    /// Reads bytes indexed before. Fails if their size isn't the indexed file's.
    pub fn with_index(bytes: B, index: Arc<NavMeshIndex>) -> Result<Self> {
        if bytes.as_ref().len() != index.len {
            return format_err("the navmesh changed while it was open");
        }
        Ok(Self { bytes, index })
    }

    pub fn index(&self) -> &Arc<NavMeshIndex> {
        &self.index
    }

    pub fn header(&self) -> &SetHeader {
        &self.index.header
    }

    /// Every tile, in file order.
    pub fn tiles(&self) -> &[TileEntry] {
        &self.index.tiles
    }

    /// The index of the tile at a grid cell (the first one in the file, if there are several).
    pub fn find(&self, x: i32, z: i32) -> Option<usize> {
        self.index.find(x, z)
    }

    /// The raw tile blob of tile `index`.
    pub fn tile_bytes(&self, index: usize) -> &[u8] {
        let e = &self.index.tiles[index];
        &self.bytes.as_ref()[e.offset..e.offset + e.data_size]
    }

    /// Parses tile `index`.
    pub fn tile(&self, index: usize) -> Result<Tile> {
        let e = &self.index.tiles[index];
        let mut t = Tile::parse(self.tile_bytes(index), e.tile_ref)?;
        t.record_pad = e.record_pad;
        Ok(t)
    }

    /// Parses the tile at a grid cell, if there is one.
    pub fn tile_at(&self, x: i32, z: i32) -> Result<Option<Tile>> {
        self.find(x, z).map(|i| self.tile(i)).transpose()
    }

    /// Parses every tile.
    pub fn read_all(&self) -> Result<NavMeshFile> {
        let tiles = (0..self.tiles().len()).map(|i| self.tile(i)).collect::<Result<_>>()?;
        Ok(NavMeshFile { header: *self.header(), tiles })
    }

    /// The whole file.
    pub fn bytes(&self) -> &[u8] {
        self.bytes.as_ref()
    }
}
