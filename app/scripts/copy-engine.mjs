// Copies the dznav engine into src-tauri/binaries/ under the name Tauri expects for a sidecar
// (dznav-<target triple>.exe). Build the engine first: cmake --build build (see README).
import { execFileSync } from 'node:child_process'
import { copyFileSync, existsSync, mkdirSync, utimesSync } from 'node:fs'
import path from 'node:path'

const root = path.resolve(import.meta.dirname, '../..')
const ext = process.platform === 'win32' ? '.exe' : ''
const source = path.join(process.env.DZNAV_ENGINE_DIR ?? path.join(root, 'build/src/cli'), `dznav${ext}`)
if (!existsSync(source)) {
  console.error(`Engine not found at ${source}. Build it first: cmake --build build`)
  process.exit(1)
}

const triple = /host: (\S+)/.exec(execFileSync('rustc', ['-vV'], { encoding: 'utf8' }))?.[1]
if (!triple) {
  console.error('Could not read the Rust target triple from `rustc -vV`.')
  process.exit(1)
}

const dir = path.resolve(import.meta.dirname, '../src-tauri/binaries')
mkdirSync(dir, { recursive: true })
const target = path.join(dir, `dznav-${triple}${ext}`)
copyFileSync(source, target)
// The copy keeps the engine's build time on Windows. Tauri's build script copies the sidecar
// next to the app, and Cargo only reruns it for files modified since its last run, so an engine
// built before the app's last build would never reach target/. Stamp the copy with the time now.
const now = new Date()
utimesSync(target, now, now)
console.log(`engine: ${path.relative(root, source)} -> ${path.relative(root, target)}`)
