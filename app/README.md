# Flare Navgen: desktop app

Tauri v2 + React + TypeScript + Tailwind CSS v4 + shadcn/ui. It drives the `dznav` engine (in
[../engine/](../engine/)) as a bundled sidecar, through the engine's JSON output
([../docs/cli-json.md](../docs/cli-json.md)), and reads navmeshes for Inspect through the engine's library. Design rules:
[../docs/design.md](../docs/design.md).

## Develop

Needs Node 22+, pnpm, and Rust with a C++ compiler (MSVC on Windows), for the app and the engine.

The updater, much of the interface and the release script are shared by the Flare apps, in the private
[flare-common](https://github.com/CallMeLewis/flare-common) repo, which the app links to from beside this repo. Check it
out next to the repository, on the tag in `../.flare-common-version`, and install its packages first (from `app/`):

```sh
git clone https://github.com/CallMeLewis/flare-common ../../flare-common
git -C ../../flare-common checkout "$(cat ../.flare-common-version)"
pnpm --dir ../../flare-common install
pnpm install
pnpm tauri dev      # builds the engine, copies it into src-tauri/binaries/, then runs the app
pnpm tauri build    # installers in src-tauri/target/release/bundle/
```

A change to the shared code is made in flare-common, and shows in the app straight away through the link. It
reaches the app's releases once flare-common is tagged and `.flare-common-version` moves to the new tag: see
flare-common's README.

`pnpm dev` alone serves the UI in a browser with a simulated engine (marked "Preview: simulated
engine" in the status bar), for working on the interface without the desktop shell.

## Layout

- `src/lib/engine.ts`: engine events and client; `src/lib/mock-engine.ts`: the simulated engine.
- `src/state/`: connection (probe), generation job (per-tile state for the map), settings, theme.
- `src/components/tile-grid.tsx`: the canvas tile map (zoom, pan, range select, keyboard).
- `src/pages/`: Generate, Inspect, Tools; `src/components/settings-dialog.tsx`: Settings;
  `src/components/server.tsx`: connecting to the data server.
- From flare-common: the updater (`tauri-plugin-flare-updater`), the theme, base components, title bar, sidebar,
  settings dialog and update button (`@flare/ui`), and the release script (`@flare/release`, used by
  `scripts/release.ts`).
- `src-tauri/src/engine.rs`: starts `dznav --json`, streams its output, stops (`stop` on stdin) or kills it.
- `src-tauri/src/inspect.rs` and `src/lib/inspector.ts`: Inspect reads navmeshes in process, through the engine's
  library.
