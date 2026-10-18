// The navmesh open in Inspect. Kept here rather than in the page, so leaving Inspect and coming back doesn't ask for its
// summary again.
import { EngineError, type EventOf } from '@/lib/engine'
import { navmeshInfo } from '@/lib/inspector'

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
    const info = await navmeshInfo(path)
    if (request === latest) inspect.set({ path, info, loading: false, error: null })
  } catch (e) {
    if (request === latest) inspect.set({ path, info: null, loading: false, error: e instanceof EngineError ? e.message : String(e) })
  }
}
