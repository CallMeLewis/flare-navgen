//! Builds the C++ core: Recast & Detour (patched for DayZ, in third_party/), the tile builder from src/dznav/ and the
//! C interface in cpp/.

use std::path::{Path, PathBuf};

fn sources(dir: &Path) -> Vec<PathBuf> {
    let mut files: Vec<PathBuf> = std::fs::read_dir(dir)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", dir.display()))
        .map(|entry| entry.unwrap().path())
        .filter(|p| p.extension().is_some_and(|e| e == "cpp"))
        .collect();
    files.sort();
    files
}

fn base(msvc: bool) -> cc::Build {
    let mut b = cc::Build::new();
    b.cpp(true).std("c++20").define("DT_POLYREF64", None);
    if msvc {
        b.flag("/utf-8").flag("/permissive-");
        b.define("_CRT_SECURE_NO_WARNINGS", None).define("NOMINMAX", None).define("WIN32_LEAN_AND_MEAN", None);
    }
    // Recast's asserts are for development; the CMake release build defines NDEBUG too.
    if std::env::var("OPT_LEVEL").is_ok_and(|level| level != "0") {
        b.define("NDEBUG", None);
    }
    b
}

fn main() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let root = manifest.join("../../..");
    let recast = root.join("third_party/recastnavigation");
    let engine = root.join("src/dznav");
    let msvc = std::env::var("CARGO_CFG_TARGET_ENV").is_ok_and(|env| env == "msvc");
    let includes = [recast.join("Recast/Include"), recast.join("Detour/Include"), engine.join("include")];

    let mut core = base(msvc);
    core.includes(&includes);
    if msvc {
        core.flag("/W4");
    }
    core.files(["TileBuilder.cpp", "JumpLinks.cpp", "WorldInput.cpp"].map(|f| engine.join(f)));
    core.file(manifest.join("cpp/core.cpp"));
    core.compile("dznav_core");

    // Linked after the core, which uses it (single-pass linkers resolve in order).
    let mut third_party = base(msvc);
    third_party.includes(&includes[..2]).warnings(false);
    third_party.files(sources(&recast.join("Recast/Source"))).files(sources(&recast.join("Detour/Source")));
    third_party.compile("recastdetour");

    for dir in [recast.join("Recast"), recast.join("Detour"), engine, manifest.join("cpp")] {
        println!("cargo::rerun-if-changed={}", dir.display());
    }
}
