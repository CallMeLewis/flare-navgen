//! Top-down navmesh preview images: a minimal RGBA raster with PNG output.

use std::path::Path;

use crate::error::{Result, io_err};
use crate::format::NavMeshFile;
use crate::reader::NavMeshReader;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Rgba {
    pub r: u8,
    pub g: u8,
    pub b: u8,
    pub a: u8,
}

const fn rgba(r: u8, g: u8, b: u8, a: u8) -> Rgba {
    Rgba { r, g, b, a }
}

pub struct Raster {
    width: i32,
    height: i32,
    px: Vec<u8>,
}

impl Raster {
    pub fn new(width: i32, height: i32, fill: Rgba) -> Self {
        let px = [fill.r, fill.g, fill.b, fill.a].repeat(width.max(0) as usize * height.max(0) as usize);
        Self { width, height, px }
    }

    pub fn width(&self) -> i32 {
        self.width
    }

    pub fn height(&self) -> i32 {
        self.height
    }

    /// The pixels, RGBA, row by row from the top.
    pub fn pixels(&self) -> &[u8] {
        &self.px
    }

    pub fn blend(&mut self, x: i32, y: i32, c: Rgba) {
        if x < 0 || y < 0 || x >= self.width || y >= self.height {
            return;
        }
        let i = (y as usize * self.width as usize + x as usize) * 4;
        let p = &mut self.px[i..i + 4];
        let a = u32::from(c.a);
        let mix = |src: u8, dst: u8| ((u32::from(src) * a + u32::from(dst) * (255 - a)) / 255) as u8;
        p[0] = mix(c.r, p[0]);
        p[1] = mix(c.g, p[1]);
        p[2] = mix(c.b, p[2]);
        p[3] = 255;
    }

    /// Fills a convex polygon given in pixel coordinates (x, y pairs).
    pub fn fill_convex(&mut self, xy: &[f32], c: Rgba) {
        let count = xy.len() / 2;
        if count == 0 {
            return;
        }
        let (mut ymin, mut ymax) = (xy[1], xy[1]);
        for i in 1..count {
            ymin = ymin.min(xy[i * 2 + 1]);
            ymax = ymax.max(xy[i * 2 + 1]);
        }
        let y0 = 0.max((ymin - 0.5).ceil() as i32);
        let y1 = (self.height - 1).min((ymax - 0.5).floor() as i32);
        for y in y0..=y1 {
            let sy = y as f32 + 0.5;
            let (mut xl, mut xr) = (1e30f32, -1e30f32);
            let mut j = count - 1;
            for i in 0..count {
                let (ax, ay, bx, by) = (xy[j * 2], xy[j * 2 + 1], xy[i * 2], xy[i * 2 + 1]);
                if (ay <= sy && by > sy) || (by <= sy && ay > sy) {
                    let x = ax + (sy - ay) / (by - ay) * (bx - ax);
                    xl = xl.min(x);
                    xr = xr.max(x);
                }
                j = i;
            }
            if xl > xr {
                continue;
            }
            let x0 = 0.max((xl - 0.5).ceil() as i32);
            let x1 = (self.width - 1).min((xr - 0.5).floor() as i32);
            for x in x0..=x1 {
                self.blend(x, y, c);
            }
        }
    }

    pub fn line(&mut self, x0: f32, y0: f32, x1: f32, y1: f32, c: Rgba) {
        let (dx, dy) = (x1 - x0, y1 - y0);
        let steps = 1.max(dx.abs().max(dy.abs()) as i32);
        for i in 0..=steps {
            let t = i as f32 / steps as f32;
            self.blend((x0 + dx * t) as i32, (y0 + dy * t) as i32, c);
        }
    }

    pub fn disc(&mut self, cx: f32, cy: f32, r: f32, c: Rgba) {
        let (x0, x1) = ((cx - r).floor() as i32, (cx + r).ceil() as i32);
        let (y0, y1) = ((cy - r).floor() as i32, (cy + r).ceil() as i32);
        for y in y0..=y1 {
            for x in x0..=x1 {
                let (ddx, ddy) = (x as f32 + 0.5 - cx, y as f32 + 0.5 - cy);
                if ddx * ddx + ddy * ddy <= r * r {
                    self.blend(x, y, c);
                }
            }
        }
    }

    /// Encodes the image as a PNG (8-bit RGBA, stored deflate blocks).
    pub fn to_png(&self) -> Vec<u8> {
        let mut out = vec![0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a];
        let mut ihdr = Vec::new();
        ihdr.extend_from_slice(&(self.width as u32).to_be_bytes());
        ihdr.extend_from_slice(&(self.height as u32).to_be_bytes());
        ihdr.extend_from_slice(&[8, 6, 0, 0, 0]);
        png_chunk(&mut out, b"IHDR", &ihdr);

        let row = self.width as usize * 4;
        let mut raw = Vec::with_capacity((row + 1) * self.height as usize);
        for y in 0..self.height as usize {
            raw.push(0);
            raw.extend_from_slice(&self.px[y * row..(y + 1) * row]);
        }
        // A zlib stream of stored (uncompressed) deflate blocks.
        let mut z = Vec::with_capacity(raw.len() + raw.len() / 65535 * 5 + 16);
        z.extend_from_slice(&[0x78, 0x01]);
        let (mut s1, mut s2) = (1u32, 0u32);
        for &b in &raw {
            s1 = (s1 + u32::from(b)) % 65521;
            s2 = (s2 + s1) % 65521;
        }
        let mut pos = 0;
        loop {
            let n = (raw.len() - pos).min(65535);
            let last = pos + n >= raw.len();
            z.push(u8::from(last));
            z.extend_from_slice(&(n as u16).to_le_bytes());
            z.extend_from_slice(&(!(n as u16)).to_le_bytes());
            z.extend_from_slice(&raw[pos..pos + n]);
            pos += n;
            if last {
                break;
            }
        }
        z.extend_from_slice(&((s2 << 16) | s1).to_be_bytes());
        // Split IDAT so each chunk stays well below the 2^31 limit.
        for chunk in z.chunks(1 << 26) {
            png_chunk(&mut out, b"IDAT", chunk);
        }
        png_chunk(&mut out, b"IEND", &[]);
        out
    }

    pub fn write_png(&self, path: impl AsRef<Path>) -> Result<()> {
        let path = path.as_ref();
        std::fs::write(path, self.to_png()).map_err(io_err("create", path))
    }
}

fn crc32(data: &[u8]) -> u32 {
    static TABLE: std::sync::OnceLock<[u32; 256]> = std::sync::OnceLock::new();
    let table = TABLE.get_or_init(|| {
        let mut t = [0u32; 256];
        for (i, e) in t.iter_mut().enumerate() {
            let mut c = i as u32;
            for _ in 0..8 {
                c = if c & 1 != 0 { 0xedb8_8320 ^ (c >> 1) } else { c >> 1 };
            }
            *e = c;
        }
        t
    });
    !data.iter().fold(!0u32, |crc, &b| table[((crc ^ u32::from(b)) & 0xff) as usize] ^ (crc >> 8))
}

fn png_chunk(out: &mut Vec<u8>, kind: &[u8; 4], data: &[u8]) {
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    let start = out.len();
    out.extend_from_slice(kind);
    out.extend_from_slice(data);
    let crc = crc32(&out[start..]);
    out.extend_from_slice(&crc.to_be_bytes());
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PreviewOptions {
    /// World-space XZ rectangle to draw.
    pub min_x: f32,
    pub min_z: f32,
    pub max_x: f32,
    pub max_z: f32,
    pub pixels_per_meter: f32,
    pub edges: bool,
    pub links: bool,
    pub obstacles: bool,
}

impl Default for PreviewOptions {
    fn default() -> Self {
        Self {
            min_x: 0.0,
            min_z: 0.0,
            max_x: 0.0,
            max_z: 0.0,
            pixels_per_meter: 1.0,
            edges: true,
            links: true,
            obstacles: true,
        }
    }
}

pub fn area_color(area: u8) -> Rgba {
    match area {
        1 => rgba(96, 156, 72, 255),      // terrain
        2 => rgba(120, 180, 230, 255),    // water
        3 => rgba(50, 110, 200, 255),     // waterdeep
        4 => rgba(90, 200, 210, 255),     // watersea
        5 => rgba(25, 55, 120, 255),      // waterdeepsea
        8 => rgba(214, 140, 60, 255),     // building
        12 => rgba(240, 220, 70, 255),    // dooropened
        13 => rgba(220, 60, 50, 255),     // doorclosed
        15 => rgba(220, 80, 220, 255),    // ladder
        16 => rgba(150, 90, 200, 255),    // crawl
        17 => rgba(230, 140, 190, 255),   // crouch
        48.. => rgba(255, 255, 255, 255), // jump links
        _ => rgba(160, 160, 160, 255),
    }
}

/// Draws the tiles of `file` that overlap the options' rectangle.
pub fn render_preview<'a>(tiles: impl IntoIterator<Item = &'a crate::Tile>, o: &PreviewOptions) -> Raster {
    let s = o.pixels_per_meter;
    let w = 1.max(((o.max_x - o.min_x) * s) as i32);
    let h = 1.max(((o.max_z - o.min_z) * s) as i32);
    let mut img = Raster::new(w, h, rgba(18, 18, 22, 255));
    let px = |x: f32| (x - o.min_x) * s;
    let py = |z: f32| (o.max_z - z) * s;

    let mut pts = Vec::new();
    for t in tiles {
        let hd = &t.header;
        if hd.bmax[0] < o.min_x || hd.bmin[0] > o.max_x || hd.bmax[2] < o.min_z || hd.bmin[2] > o.max_z {
            continue;
        }
        let vert = |i: u16| &t.verts[i as usize * 3..i as usize * 3 + 3];
        for p in t.polys.iter().filter(|p| p.poly_type() == 0) {
            pts.clear();
            for &v in &p.verts[..p.vert_count as usize] {
                let v = vert(v);
                pts.push(px(v[0]));
                pts.push(py(v[2]));
            }
            let mut c = area_color(p.area());
            if p.flags & 0x8000 != 0 {
                c = rgba(c.r / 3 + 60, c.g / 3, c.b / 3, 255);
            }
            if p.flags & 0x0002 != 0 {
                c = rgba(c.r / 2, c.g / 2, c.b / 2, 255);
            }
            img.fill_convex(&pts, c);
        }
        if o.edges && s >= 2.0 {
            for p in t.polys.iter().filter(|p| p.poly_type() == 0) {
                let n = p.vert_count as usize;
                for k in 0..n {
                    let (a, b) = (vert(p.verts[k]), vert(p.verts[(k + 1) % n]));
                    let c = if p.neis[k] == 0 { rgba(0, 0, 0, 200) } else { rgba(0, 0, 0, 60) };
                    img.line(px(a[0]), py(a[2]), px(b[0]), py(b[2]), c);
                }
            }
        }
        if o.obstacles {
            for ob in &t.obstacles {
                img.disc(px(ob.pos[0]), py(ob.pos[2]), (ob.radius * s).max(1.0), rgba(30, 30, 30, 220));
            }
        }
        if o.links {
            for c in &t.off_mesh_cons {
                let lc = if c.flags & 1 != 0 { rgba(255, 255, 255, 200) } else { rgba(255, 230, 0, 200) };
                img.line(px(c.pos[0]), py(c.pos[2]), px(c.pos[3]), py(c.pos[5]), lc);
                img.disc(px(c.pos[3]), py(c.pos[5]), (0.15 * s).max(1.0), lc);
            }
        }
    }
    img
}

/// Renders a whole in-memory navmesh.
pub fn render_file(file: &NavMeshFile, o: &PreviewOptions) -> Raster {
    render_preview(&file.tiles, o)
}

/// Renders a navmesh file, reading only the tiles that overlap the options' rectangle.
pub fn render_area<B: AsRef<[u8]>>(reader: &NavMeshReader<B>, o: &PreviewOptions) -> Result<Raster> {
    let tiles = reader
        .tiles()
        .iter()
        .enumerate()
        .filter(|(_, e)| {
            let h = &e.header;
            !(h.bmax[0] < o.min_x || h.bmin[0] > o.max_x || h.bmax[2] < o.min_z || h.bmin[2] > o.max_z)
        })
        .map(|(i, _)| reader.tile(i))
        .collect::<Result<Vec<_>>>()?;
    Ok(render_preview(&tiles, o))
}
