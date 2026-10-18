// Data server connection and the world it serves (from `dznav probe`).
import { EngineError, runEngine, type WorldParams } from '@/lib/engine'

import { persistentStore, Store } from './store'

export interface Settings {
  /** One host, or several separated by commas (each DayZDiag serves one client). */
  server: string
  threads: number | ''
  theme: 'dark' | 'light' | 'system'
  /** The interface size, as a share of the normal size (one of @flare/ui's INTERFACE_SCALES). */
  uiScale: number
  /** Shows server details, extra build statistics and developer tools. */
  expert: boolean
}

export const settings = persistentStore<Settings>('dznav.settings', {
  server: '127.0.0.1',
  threads: '',
  theme: 'dark',
  uiScale: 1,
  expert: false,
})

export type ConnectionState =
  | { status: 'idle' }
  | { status: 'connecting'; host: string }
  | { status: 'connected'; host: string; world: WorldParams; at: Date }
  | { status: 'error'; host: string; kind: string; message: string }

export const connection = new Store<ConnectionState>({ status: 'idle' })

export async function connect(host = settings.get().server) {
  connection.set({ status: 'connecting', host })
  try {
    const result = await runEngine(['probe', host], 'world')
    connection.set({ status: 'connected', host, world: result.params, at: new Date() })
  } catch (e) {
    const err = e instanceof EngineError ? e : new EngineError('exit', String(e))
    connection.set({ status: 'error', host, kind: err.kind, message: err.message })
  }
}
