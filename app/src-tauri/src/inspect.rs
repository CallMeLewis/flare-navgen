//! Inspect reads navmeshes in the app's own process, through the engine's library, instead of running `dznav` for
//! each query.
//!
//! No file stays open between queries: Windows can't replace a mapped file, and a run may be about to save over the one
//! being inspected. What is slow to work out (the tile index, and the summary that reads every tile) is kept until the
//! file changes.

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::SystemTime;

use dznav::preview::{PreviewOptions, render_area};
use dznav::summary::{NavMeshInfo, TileDetail, navmesh_info, tile_detail};
use dznav::{NavMeshIndex, NavMeshReader};
use tauri::State;
use tauri::ipc::Response;

struct Cached {
    path: PathBuf,
    len: u64,
    modified: Option<SystemTime>,
    index: Arc<NavMeshIndex>,
    info: Option<NavMeshInfo>,
}

#[derive(Clone, Default)]
pub struct Inspector {
    cache: Arc<Mutex<Option<Cached>>>,
}

impl Inspector {
    /// Maps the file and runs `f` on it, using the cached index while the file is unchanged.
    fn read<T>(
        &self,
        path: &str,
        f: impl FnOnce(&NavMeshReader, &mut Cached) -> dznav::Result<T>,
    ) -> Result<T, String> {
        let meta = std::fs::metadata(path).map_err(|e| format!("cannot open {path}: {e}"))?;
        let (len, modified) = (meta.len(), meta.modified().ok());
        let mut cache = self.cache.lock().unwrap();
        let fresh = cache.as_ref().filter(|c| c.path == Path::new(path) && c.len == len && c.modified == modified);
        let reader = match fresh {
            Some(c) => NavMeshReader::open_indexed(path, c.index.clone()),
            None => NavMeshReader::open(path),
        }
        .map_err(|e| e.to_string())?;
        if fresh.is_none() {
            *cache = Some(Cached { path: path.into(), len, modified, index: reader.index().clone(), info: None });
        }
        f(&reader, cache.as_mut().unwrap()).map_err(|e| e.to_string())
    }

    fn info(&self, path: &str) -> Result<NavMeshInfo, String> {
        self.read(path, |reader, cached| {
            if let Some(info) = &cached.info {
                return Ok(info.clone());
            }
            let info = navmesh_info(reader, path, true)?;
            cached.info = Some(info.clone());
            Ok(info)
        })
    }

    fn tile(&self, path: &str, x: i32, z: i32) -> Result<TileDetail, String> {
        self.read(path, |reader, _| {
            let tile =
                reader.tile_at(x, z)?.ok_or_else(|| dznav::Error::Format(format!("no tile ({x},{z}) in {path}")))?;
            Ok(tile_detail(&tile))
        })
    }

    fn render(&self, path: &str, options: &PreviewOptions) -> Result<Vec<u8>, String> {
        self.read(path, |reader, _| Ok(render_area(reader, options)?.to_png()))
    }
}

/// Runs a query on a worker thread: reading a whole map takes about a second.
async fn on_worker<T: Send + 'static>(f: impl FnOnce() -> Result<T, String> + Send + 'static) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(f).await.map_err(|e| e.to_string())?
}

/// A navmesh's totals and per-tile metrics, as `dznav info --tiles --json` reports them.
#[tauri::command]
pub async fn inspect_info(inspector: State<'_, Inspector>, path: String) -> Result<NavMeshInfo, String> {
    let inspector = inspector.inner().clone();
    on_worker(move || inspector.info(&path)).await
}

/// One tile's details, as `dznav tile --json` reports them.
#[tauri::command]
pub async fn inspect_tile(inspector: State<'_, Inspector>, path: String, x: i32, z: i32) -> Result<TileDetail, String> {
    let inspector = inspector.inner().clone();
    on_worker(move || inspector.tile(&path, x, z)).await
}

/// A top-down PNG of a world-space rectangle, as `dznav render` draws it.
#[tauri::command]
pub async fn inspect_render(
    inspector: State<'_, Inspector>,
    path: String,
    min_x: f32,
    min_z: f32,
    max_x: f32,
    max_z: f32,
    pixels_per_meter: f32,
) -> Result<Response, String> {
    let inspector = inspector.inner().clone();
    let options = PreviewOptions { min_x, min_z, max_x, max_z, pixels_per_meter, ..Default::default() };
    on_worker(move || inspector.render(&path, &options)).await.map(Response::new)
}

#[cfg(test)]
mod tests {
    use dznav::NavMeshFile;
    use dznav::format::*;

    use super::*;

    fn navmesh(tiles_x: i32) -> NavMeshFile {
        let tiles = (0..tiles_x)
            .map(|x| {
                let mut t = Tile {
                    tile_ref: encode_poly_ref(1, x as u32, 0),
                    header: MeshHeader {
                        x,
                        bmin: [x as f32 * 50.0, 0.0, 0.0],
                        bmax: [(x + 1) as f32 * 50.0, 1.0, 50.0],
                        bv_quant_factor: 4.0,
                        ..Default::default()
                    },
                    verts: vec![
                        x as f32 * 50.0,
                        0.0,
                        0.0,
                        x as f32 * 50.0 + 50.0,
                        0.0,
                        0.0,
                        x as f32 * 50.0,
                        0.0,
                        50.0,
                    ],
                    polys: vec![Poly {
                        verts: [0, 1, 2, 0, 0, 0],
                        vert_count: 3,
                        area_and_type: 1,
                        ..Default::default()
                    }],
                    ..Default::default()
                };
                t.sync_header();
                t
            })
            .collect();
        NavMeshFile {
            header: SetHeader {
                tiles_x,
                tiles_z: 1,
                tile_width: 50.0,
                tile_height: 50.0,
                max_tiles: tiles_x,
                max_polys: 1 << POLY_BITS,
                ..Default::default()
            },
            tiles,
        }
    }

    #[test]
    fn reads_navmeshes_without_keeping_them_open() {
        let dir = std::env::temp_dir().join(format!("flare-navgen-inspect-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("navmesh.nm");
        let p = path.to_str().unwrap();
        navmesh(2).write(&path).unwrap();

        let inspector = Inspector::default();
        let info = inspector.info(p).unwrap();
        assert_eq!((info.tiles, info.polys, info.tile_list.as_ref().unwrap().len()), (2, 2, 2));
        assert_eq!(inspector.tile(p, 1, 0).unwrap().x, 1);
        assert!(inspector.tile(p, 5, 5).unwrap_err().starts_with("no tile (5,5)"));
        let png = inspector.render(p, &PreviewOptions { max_x: 100.0, max_z: 50.0, ..Default::default() }).unwrap();
        assert_eq!(&png[1..4], b"PNG");

        // A run saves over the file (write elsewhere, then rename), which Windows refuses while it is mapped.
        let next = dir.join("next.nm");
        navmesh(3).write(&next).unwrap();
        std::fs::remove_file(&path).unwrap();
        std::fs::rename(&next, &path).unwrap();
        assert_eq!(inspector.info(p).unwrap().tiles, 3);
        assert_eq!(inspector.tile(p, 2, 0).unwrap().x, 2);
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
