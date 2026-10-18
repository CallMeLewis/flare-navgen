// The navmesh open in Inspect. Kept here rather than in the page, so leaving Inspect and coming back doesn't read the
// file again, which takes seconds for a large map.
import { EngineError, runEngine, type EventOf } from '@/lib/engine'

import { Store } from './store'

export interface InspectState {
  path: string
  info: EventOf<'info'> | null
  loading: boolean
  error: string | null
}

export const inspect = new Store<InspectState>({ path: '', info: null, loading: false, error: null })

let latest = 0

/** Reads a navmesh into Inspect (Generate's "Inspect the navmesh" reads it again after a run). Only the latest one shows. */
export async function openNavmesh(path: string) {
  if (!path.trim()) return
  const request = ++latest
  inspect.set((s) => ({ ...s, path, loading: true, error: null }))
  try {
    const info = await runEngine(['info', path, '--tiles'], 'info')
    if (request === latest) inspect.set({ path, info, loading: false, error: null })
  } catch (e) {
    if (request === latest) inspect.set({ path, info: null, loading: false, error: e instanceof EngineError ? e.message : String(e) })
  }
}
