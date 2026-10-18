// Builds the dznav engine (engine/, a Cargo workspace) and copies it into src-tauri/binaries/ under the name Tauri
// expects for a sidecar (dznav-<target triple>.exe). With DZNAV_ENGINE_DIR set, copies the engine already built there
// instead (CI builds it in its own step).
import { execFileSync } from 'node:child_process'
import { copyFileSync, existsSync, mkdirSync, utimesSync } from 'node:fs'
import path from 'node:path'

const root = path.resolve(import.meta.dirname, '../..')
const ext = process.platform === 'win32' ? '.exe' : ''
let dir = process.env.DZNAV_ENGINE_DIR
if (!dir) {
  const engine = path.join(root, 'engine')
  execFileSync('cargo', ['build', '--release', '-p', 'dznav-cli'], { cwd: engine, stdio: 'inherit' })
  dir = path.join(engine, 'target/release')
}
const source = path.join(dir, `dznav${ext}`)
if (!existsSync(source)) {
  console.error(`Engine not found at ${source}.`)
  process.exit(1)
}

const triple = /host: (\S+)/.exec(execFileSync('rustc', ['-vV'], { encoding: 'utf8' }))?.[1]
if (!triple) {
  console.error('Could not read the Rust target triple from `rustc -vV`.')
  process.exit(1)
}

const binaries = path.resolve(import.meta.dirname, '../src-tauri/binaries')
mkdirSync(binaries, { recursive: true })
const target = path.join(binaries, `dznav-${triple}${ext}`)
copyFileSync(source, target)
// The copy keeps the engine's build time on Windows. Tauri's build script copies the sidecar
// next to the app, and Cargo only reruns it for files modified since its last run, so an engine
// built before the app's last build would never reach target/. Stamp the copy with the time now.
const now = new Date()
utimesSync(target, now, now)
console.log(`engine: ${path.relative(root, source)} -> ${path.relative(root, target)}`)
