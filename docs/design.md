# Flare Navgen design

The visual rules the app follows. Desktop app (Tauri v2 + TypeScript + Tailwind CSS v4 + shadcn/ui) on top of the `dznav` engine.
Users are DayZ map makers running long generation jobs (30+ minutes per map) and inspecting the
result.

## Direction

- **Style:** Minimalism & Swiss on a dark-first theme; Real-Time Monitor + terminal log.
  Flat surfaces, sharp edges, no gradients or glass.
- **Density:** dense desktop tool (8–32 px spacing scale). Data is the hero: the tile map, live
  numbers and logs.
- **Motion:** minimal. 150–200 ms colour/opacity transitions for hover and state changes, none on
  data updates. Respect `prefers-reduced-motion`. No scroll reveals.
- **Theme:** dark by default; light theme supported through the same semantic tokens.

## Colour tokens

The Flare apps' shared tokens, from `@flare/ui` in the flare-common repo (`packages/ui/src/theme.css`), so the apps
read as one family; change them there and every app gets them. This app's `src/index.css` adds only its own: the sky
blue `--brand` of its icon, the sidebar mapping and the data colours below. Exposed to Tailwind with `@theme inline`.
Components never use raw palette classes. Radius `0.5rem`, Fira Sans and Fira Code (see Typography).

| Token | Dark | Light | Use |
|---|---|---|---|
| `--background` | `#0B1120` | `#FFFFFF` | app background, dialogs |
| `--card` | `#0F1829` | `#F5F7FA` | panels, the sidebar, cards in dialogs |
| `--popover` | `#131C30` | `#FFFFFF` | menus, tooltips |
| `--muted` / `--accent` / `--secondary` | `#18223A` | `#EEF2F6` | inactive surfaces, hover, the chosen sidebar item, secondary buttons |
| `--muted-foreground` | `#9AA8BD` | `#475569` | secondary text |
| `--foreground` | `#F1F5F9` | `#0F172A` | primary text |
| `--border` | `#1F2A40` | `#DFE5EC` | dividers, outlines |
| `--input` | `#56677F` | `#8794A8` | field edges (3:1) |
| `--control` | `#334155` | `#CBD5E1` | switch and other control fills |
| `--primary` / `--ring` | `#22C55E` (run green), text `#04130A` | `#15803D`, text white | primary action (Start), focus ring |
| `--destructive` | `#DC2626` | `#C81E1E` | stop, delete, failures |
| `--success` / `--warning` / `--danger` | `#4ADE80` / `#FBBF24` / `#F87171` | `#166534` / `#92400E` / `#C81E1E` | status text and icons |

### Data colours

Never colour alone: every state also has an icon or label, and maps have a legend.

- **Tile state** (generation map, from the engine's `fetched`/`tile` events): pending `muted`,
  queued (geometry fetched, building) amber `#F59E0B`, built green `#22C55E`, empty slate
  `#475569` (dotted), failed red `#EF4444` (cross), outside the run's range dimmed.
- **Tile metric** (data size, polys, links, build time): sequential colour-blind-safe scale
  (viridis), with a numeric legend.
- **Navmesh areas** (inspector, matches `dznav render`): terrain `#609C48`, water `#78B4E6`,
  deep water `#326EC8`, sea `#193778`, building `#D68C3C`, door open `#F0DC46`, door closed
  `#DC3C32`, ladder `#DC50DC`, crawl `#965AC8`, crouch `#E68CBE`, jump links white (one-way
  amber), obstacles dark discs, isolated polys dimmed and hatched.

## Typography

- **UI:** Fira Sans (400/500/600). **Data:** Fira Code (400/500) for coordinates, counts,
  sizes, IDs and logs, always with `tabular-nums`.
- Scale (desktop, dense): 12 px captions/legends (minimum, never smaller), 13 px table and log
  text, 14 px body, 16 px section titles, 20 px page titles, 28 px key metrics.
- Controls are the shared ones from `@flare/ui` (Flare Launcher's, shadcn's standard sizes): 14 px text in buttons,
  fields, labels, menus and tabs; buttons and fields 36 px tall (32 px small); 18 px dialog titles. The shared sidebar
  is 224 px, folded to 56 px of icons below 1280 px wide, with items 36 px tall and 13 px text.
- Line height 1.5 for prose, 1.3 for tables and logs.
- Bundle the fonts with the app (no Google Fonts requests at runtime).

## Layout

- **Shell:** the Flare apps' shared one (`AppShell` in `@flare/ui`): the title bar with the app's mark and name, the
  sidebar, then the workspace, with a persistent **status bar** at the bottom showing server connection, the active
  job (progress, tiles/s, ETA) and the open file, so a long run is visible from any page.
- **Navigation:** Generate (home) and Inspect; Tools in expert mode. Five items max. A running generation's progress
  shows beside Generate. The bottom of the sidebar holds the Settings gear (left, opens Settings as a dialog) and the
  update button (right), stacked when the sidebar is folded to icons.
- **Audience:** the default view is for server owners who just want a navmesh. The Tools page,
  server details, extra build statistics and the build-time map appear only with **Expert mode**
  (Settings).
- **Map pages** (Generate, Inspect): map canvas fills the workspace; controls in a left panel;
  tile details in a right panel (resizable, collapsible).
- Minimum window 1280×800; panels collapse below 1440 px wide.

## Pages

1. **Generate.** Options panel: data server address with Connect and status (expert: a Server
   details panel with the world, cell classes, agent and link classes), output file, tiles (whole
   world or a range, also by dragging on the map), update an existing navmesh (replace tiles or
   fill in missing), and a collapsed Advanced section (sea level, door policy, links, island
   filter, capture file, record geometry, checkpoint), with a plain-English guide to each setting
   in a side panel. Unless pinned, a server run uses the tide read when connecting, or the sea
   level the base navmesh was built with, so resumes keep the same coastline. Before connecting, the map area shows the
   exact DayZDiag command to start a data server, with a copy button. Start/Stop, or Resume and
   Start over after a stopped run (replacing the last run's file asks first). Live stats (time
   left, elapsed, built, failed; expert: speed, waiting, empty, stray part ids), log console.
2. **Inspect.** Open a `.nm`; WebGL 2D map with layer toggles (areas, jump links, doors,
   obstacles, isolated, tile grid, polygon edges); pan/zoom; click a tile for its details (header,
   counts, link table, door parts, obstacles); go to tile or world coordinate.
3. **Tools** (expert mode). Fix and update: merge a regenerated range, re-run the island filter.
   Developer checks: verify against a reference, diff summary. Each is a small form with a result
   panel.
4. **Settings** (the shared dialog). General: appearance (theme and interface size) and expert mode. Generation:
   builder threads. About: the app, its updates (version, status, check now, automatic checks, channel) and the
   engine.

## Behaviour and states

- Long runs: OS notification on completion or failure; closing the window during a run asks
  for confirmation (stop or keep running in the tray).
- Connection lost: show retrying, then a **Resume** action that restarts with `--base` and
  `--skip-existing`.
- Every async action shows progress within 100 ms; buttons disable while busy.
- Errors appear next to their cause (field, tile, job), in plain language, with the fix.

## Accessibility

- Text contrast ≥ 4.5:1, focus rings always visible, all actions keyboard reachable.
- Tile grid: arrow keys move the focused tile, Enter opens its details, and a live region
  announces the focused tile's coordinates and state. Hover and focus show values.
- Icons: Lucide (shadcn default), never emoji; icon-only buttons have labels and tooltips.
- Hit targets ≥ 32 px in dense toolbars (desktop pointer), 44 px where touch is likely.

## Engine integration

The app runs `dznav.exe` as a Tauri sidecar with `--json`: one JSON event per line on stdout
(contract in [cli-json.md](cli-json.md)); Stop writes `stop` to its stdin so the
engine saves before exiting. Navmeshes are 400 MB to 1.3 GB, so the inspector never loads a
whole file into the webview. Instead it shows a per-tile metric map (`info --tiles`), and for a
picked tile its details (`tile`) and an engine-rendered PNG (`render`, loaded through Tauri's
asset protocol). Each query currently re-reads the file (about 1.6 s for a large map); a long-lived
engine `serve` mode that keeps it loaded is the planned next step. The CLI keeps working on its own.
