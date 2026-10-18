// Reading navmeshes for Inspect. The desktop app reads them in its own process (src-tauri/src/inspect.rs), which keeps
// what it has worked out until the file changes; the browser preview asks the simulated engine.
import { invoke } from '@tauri-apps/api/core'

import { EngineError, hasRealEngine, runEngine, type EventOf } from './engine'

async function call<T>(command: string, args: Record<string, unknown>): Promise<T> {
  try {
    return await invoke<T>(command, args)
  } catch (e) {
    throw new EngineError('file', String(e))
  }
}

/** A navmesh's totals and per-tile metrics. Reads every tile the first time, about a second for a large map. */
export function navmeshInfo(path: string): Promise<EventOf<'info'>> {
  if (!hasRealEngine) return runEngine(['info', path, '--tiles'], 'info')
  return call('inspect_info', { path })
}

/** One tile's details. Fails with a message starting "no tile" when the navmesh has no tile there. */
export function tileDetail(path: string, x: number, z: number): Promise<EventOf<'tileDetail'>> {
  if (!hasRealEngine) return runEngine(['tile', path, String(x), String(z)], 'tileDetail')
  return call('inspect_tile', { path, x, z })
}

/**
 * A top-down render of the world-space rectangle from (minX, minZ) to (maxX, maxZ), as a PNG object URL to release
 * with URL.revokeObjectURL. Null in the browser preview, which can't render.
 */
export async function renderArea(
  path: string,
  [minX, minZ]: [number, number],
  [maxX, maxZ]: [number, number],
  pixelsPerMeter: number,
): Promise<string | null> {
  if (!hasRealEngine) return null
  const png = await call<ArrayBuffer>('inspect_render', { path, minX, minZ, maxX, maxZ, pixelsPerMeter })
  return URL.createObjectURL(new Blob([png], { type: 'image/png' }))
}
