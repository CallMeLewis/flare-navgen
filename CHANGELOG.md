# Changelog

What changed in each version of Flare Navgen, newest first. See [docs/release-notes.md](docs/release-notes.md) for how to
write them.

## 0.3.0-beta.1 - 2026-10-18

- This beta has a rewritten engine for building navmeshes; it builds the same navmeshes as 0.2.0, so please report anything that behaves differently.
- Building a navmesh is about one and a half times as fast.
- Inspect shows a tile's details and preview as soon as you click it, instead of after a few seconds.

## 0.2.0 - 2026-10-18

- New Interface size in Settings > General, from 90% to 150%, to make everything in Flare Navgen larger or smaller.
- New Update channel in Settings > About: choose Beta to get new versions before everyone else.
- The sidebar now folds to icons by itself when the window is narrow, instead of when you click its edge or press Ctrl+B.
- Each Settings section's title now stays at the top while you scroll.
- Dropdowns now open below the box instead of on top of it.
- Only text worth copying, such as the log, command lines and error messages, can now be selected, so dragging across the window no longer highlights it.
- Right-clicking no longer opens a browser menu with Back, Reload and Inspect; text fields still have Cut, Copy and Paste.
- Fixed the What's new card on the update button staying light in dark mode.

## 0.1.1 - 2026-10-17

- Resume now shows the tiles your navmesh already has as Already built on the map, instead of leaving them as pending.
- Start now asks before replacing a navmesh that's already saved where you're saving, including one from an earlier session.
- Tools keep running and keep their results when you switch to another page, and Flare Navgen asks before closing or installing an update while one is still running.
- Inspect keeps the navmesh you opened when you switch pages, instead of reading it again.
- Fixed End without saving reporting the run as failed: it now shows as stopped.
- Fixed Stop, save and quit getting stuck when the run had already finished, and Stop not working while a run is starting.
- Fixed an emptied tile range showing "null", and an emptied pinned sea level turning itself off, after restarting Flare Navgen.
- Fixed the status bar offering to resume a run that stopped without saving.
- Fixed the map on the Inspect page keeping its old colours after changing the theme.
- Fixed the map coordinates shown when hovering over a tile during a run from a capture file.

## 0.1.0 - 2026-10-17

- First release: connect to DayZDiag, choose where to save, and press Start to build a navmesh for your map.
- See every tile's progress on a live map of your world, with the time left, and stop whenever you like: the tiles built so far are saved, and Resume builds the rest.
- Rebuild part of a map by dragging a range on the map and turning on Update an existing navmesh, so only those tiles are replaced.
- Open a navmesh in Inspect to see its tiles, areas, jump links and doors.
- Change the sea level, doors, jump links and auto-saving under Advanced on the Generate page, with a guide that explains each setting.
- Turn on Expert mode in Settings for server details, extra build statistics, and tools for merging and comparing navmeshes.
- Flare Navgen keeps itself up to date: the button at the bottom of the sidebar checks for new versions, downloads them and restarts to install, and you can turn automatic checks off in Settings.
- The installer and the app are signed, so Windows names the publisher instead of warning about an unknown one.
