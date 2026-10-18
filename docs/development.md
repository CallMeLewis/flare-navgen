# Developing Flare Navgen

Flare Navgen is two parts: the `dznav` engine (Rust, with a C++ core, in [`engine/`](../engine/)) and the desktop app
(Tauri v2, React, TypeScript, Tailwind CSS and shadcn/ui, in [`app/`](../app/)), which runs the engine underneath.

## Build the engine

Requires Rust (the version in `rust-toolchain.toml`) and a C++20 compiler (tested with MSVC 14.51 / Visual Studio
2026), which Cargo finds itself.

```sh
cd engine
cargo build --release      # engine/target/release/dznav.exe
cargo test
```

It is a Cargo workspace:

- `dznav-sys`: the C++ core, which stays C++: tile building (`TileBuilder`, `JumpLinks` in `src/dznav/`) and Detour
  linking on the patched Recast & Detour, built with the `cc` crate behind the C interface in `cpp/dznav_core.h`.
- `dznav`: the library: the navmesh format (with `NavMeshReader`, which maps a file and reads tiles on demand), the
  data-server protocol, captures, generation, the replay server, comparison and previews.
- `dznav-cli`: the `dznav` binary.

Some tests compare with real data and only run when given it. Run them with `--release`:

- `DZNAV_SAMPLE_NM`: shipped `.nm` files, or folders of them, separated like `PATH`. Round trips and relinks them.
- `DZNAV_ORACLE`: the C++ engine's `dznav` executable. Runs both engines on the same inputs (a synthetic world, a
  replay server, and the samples) and requires the same files and output.

### The C++ engine

The engine was C++ before it was ported to Rust. Its build stays, as the reference the Rust engine is checked against,
until the Rust engine has been in use for a while. Requires CMake 3.24+.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build        # build/src/cli/dznav.exe
ctest --test-dir build
```

## Run the app

See [app/README.md](../app/README.md). It needs a built engine, and the shared
[flare-common](https://github.com/CallMeLewis/flare-common) repo checked out beside this one.

## Third-party

`third_party/recastnavigation` is [Recast & Detour](https://github.com/recastnavigation/recastnavigation)
(zlib licence). Recast is at upstream `6d1f971`, the revision DayZ's matches; Detour is newer.
Both are patched for DayZ, and every change is marked `DAYZ:`.

## Releasing

CI (`.github/workflows/ci.yml`) builds the engine and the installer on every push to `main` and every pull request,
runs the engine's tests (against the C++ engine too), and checks that the installed app starts. To release:

1. In `app/`, run `pnpm bump <version>`, then write the version's notes in [`CHANGELOG.md`](../CHANGELOG.md) following
   [release-notes.md](release-notes.md).
2. Commit and push to `main`, then run `pnpm release`. It waits for CI to pass on the commit, then tags it
   `v<version>`.
3. The tag starts the Release workflow. It builds and signs the installer, publishes it as a GitHub release, then
   updates the feed the app checks, `https://updates.darkzone.dev/flare-navgen/latest.json`.

The app checks that feed at start and every few hours (Settings > About), and the button at the bottom of the
sidebar downloads and installs what it offers. Each installer is signed with the update signing key, and the app
refuses one that doesn't match the public key in `app/src-tauri/tauri.conf.json`. The key was made with
`node scripts/release.ts keygen` (in `app/`) and is kept in `~/.flare-navgen/`. Keep a backup: without it, no further
updates can be shipped.

The repository needs a `release` environment that only `main` and `v*` tags can use, holding these secrets:

- `CERTUM_USERNAME` and `CERTUM_TOTP_SECRET`: the code signing certificate in Certum's cloud (the SimplySign
  account's email, and the secret from the `otpauth://` link SimplySign gave when the account was activated).
- `TAURI_SIGNING_PRIVATE_KEY`: the contents of the update signing key.
- `CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID`: for writing the feed to the R2 bucket behind
  `updates.darkzone.dev`.

To check code signing without releasing, run CI from the Actions tab with "Sign the Windows build".
