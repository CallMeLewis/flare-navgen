// Client for the dznav engine. In the desktop app it runs the bundled sidecar through the Rust
// side (src-tauri/src/engine.rs); in a plain browser it falls back to a simulated engine so the UI
// can be previewed. Event shapes follow docs/cli-json.md.
import { Channel, invoke, isTauri } from '@tauri-apps/api/core'

import { startMockEngine } from './mock-engine'

export type Vec3 = [number, number, number]
export type TileRange = [x0: number, z0: number, x1: number, z1: number]

export interface WorldParams {
  worldSizeX: number
  worldSizeZ: number
  tilesX: number
  tilesZ: number
  tileWidth: number
  tileHeight: number
  seaLevel: number
  deepWaterDepth: number
  contourMaxError: number
  obstacles: boolean
  blockSize: number
  filterIsolatedIslands: boolean
  seed: Vec3
  agent: {
    standHeight: number
    crouchHeight: number
    proneHeight: number
    diameter: number
    maxStepHeight: number
    maxSlope: number
  }
  cellClasses: { cells: number; width: number; cellSize: number }[]
  linkClasses: {
    index: number
    typeId: number
    jumpLength: number
    jumpHeight: number
    minCenterHeight: number
    dropdownMin: number
    dropdownMax: number
    agentHeight: number
    maxStepHeight: number
    agentRadius: number
    flags: number
    area: number
  }[]
}

export type TileOutcome = 'built' | 'empty' | 'failed'

export type EngineEvent =
  | {
      event: 'start'
      output: string
      tiles: number
      range: TileRange
      world: { tilesX: number; tilesZ: number; tileWidth: number; tileHeight: number }
      server?: string
      capture?: string
      base?: string
      skipExisting?: boolean
    }
  | { event: 'fetched'; x: number; z: number }
  | {
      event: 'tile'
      x: number
      z: number
      outcome: TileOutcome
      ms: number
      bytes?: number
      polys?: number
      verts?: number
      links?: number
      obstacles?: number
      strayPartIds?: number
      error?: string
    }
  | {
      event: 'progress'
      done: number
      total: number
      fetched?: number
      failed?: number
      elapsed: number
      rate: number
      eta?: number
    }
  | { event: 'saved'; path: string; checkpoint: boolean }
  | {
      event: 'islands'
      seedFound: boolean
      seed?: Vec3
      seedPoint?: Vec3
      reachable?: number
      isolated?: number
      output?: string
    }
  | {
      event: 'done'
      output: string
      saved?: boolean
      cancelled?: boolean
      built?: number
      empty?: number
      failed?: number
      seconds?: number
      strayTiles?: number
      resumable?: boolean
      errorCount?: number
      errors?: string[]
      tiles?: number
    }
  | { event: 'error'; kind: 'usage' | 'file' | 'network' | 'protocol'; message: string }
  | { event: 'world'; host: string; params: WorldParams }
  | {
      event: 'info'
      path: string
      tiles: number
      tilesX: number
      tilesZ: number
      tileWidth: number
      tileHeight: number
      origin: Vec3
      maxTiles: number
      maxPolys: number
      polys: number
      verts: number
      links: number
      doorParts: number
      doorPolys: number
      obstacles: number
      agent?: { height: number; radius: number; climb: number }
      cellSizes: [cellSize: number, tiles: number][]
      areas: Record<string, number>
      flags: Record<string, number>
      /** [x, z, bytes, polys, links, obstacles, doorParts] */
      tileList?: [number, number, number, number, number, number, number][]
    }
  | {
      event: 'tileDetail'
      x: number
      z: number
      ref: string
      bmin: Vec3
      bmax: Vec3
      cellSize: number
      bytes: number
      polys: number
      verts: number
      areas: Record<string, number>
      flags: Record<string, number>
      links: {
        class: number
        bidirectional: boolean
        from: Vec3
        to: Vec3
        centerHeight: number
        radius: number
        area: number
        flags: number
      }[]
      doors: { objectId: string; part: number; state: number; polys: number }[]
      obstacles: [number, number, number, number][]
    }
  | { event: 'render'; output: string; width: number; height: number }
  | {
      event: 'verify'
      compared: number
      identical: number
      ignoredIsolated: boolean
      ignoredOrder?: boolean
      different: [number, number][]
    }
  | {
      event: 'diff'
      compared: number
      onlyInA: number
      onlyInB: number
      sameCellSize: number
      a: { polys: number; links: number; obstacles: number; doorParts: number }
      b: { polys: number; links: number; obstacles: number; doorParts: number }
      areaM2: Record<string, [number, number]>
      mismatchPercent: number
      worst: {
        x: number
        z: number
        areaDelta: number
        polys: [number, number]
        links: [number, number]
        cellSize: [number, number]
      }[]
    }
  | { event: 'merge'; replaced: number; output: string }

export type EventOf<K extends EngineEvent['event']> = Extract<EngineEvent, { event: K }>

export interface EngineHandlers {
  onEvent: (event: EngineEvent) => void
  /** Non-JSON output (engine diagnostics). */
  onLog?: (line: string) => void
}

export interface EngineJob {
  /** Asks a generation to finish the tiles in progress, save and exit. */
  stop(): Promise<void>
  /** Ends the process at once without saving. */
  kill(): Promise<void>
  /** Resolves with the exit code: null if there was none, but a killed engine has one (1) on Windows. */
  exited: Promise<number | null>
}

type EngineOutput =
  | { kind: 'stdout'; line: string }
  | { kind: 'stderr'; line: string }
  | { kind: 'exit'; code: number | null }

/** True when running inside the desktop app (otherwise the engine is simulated). */
export const hasRealEngine = isTauri()

/** Runs `dznav --json <args>` and streams its events. */
export async function startEngine(args: string[], handlers: EngineHandlers): Promise<EngineJob> {
  if (!hasRealEngine) return startMockEngine(args, handlers)

  let resolveExit!: (code: number | null) => void
  const exited = new Promise<number | null>((r) => (resolveExit = r))
  const channel = new Channel<EngineOutput>()
  channel.onmessage = (out) => {
    if (out.kind === 'exit') return resolveExit(out.code)
    if (out.kind === 'stdout' && out.line.startsWith('{')) {
      try {
        handlers.onEvent(JSON.parse(out.line) as EngineEvent)
        return
      } catch {
        // fall through: report the raw line
      }
    }
    if (out.line) handlers.onLog?.(out.line)
  }
  const id = await invoke<number>('engine_start', { args, onOutput: channel })
  return {
    stop: () => invoke('engine_stop', { id }),
    kill: () => invoke('engine_kill', { id }),
    exited,
  }
}

export class EngineError extends Error {
  readonly kind: EventOf<'error'>['kind'] | 'exit'
  constructor(kind: EngineError['kind'], message: string) {
    super(message)
    this.kind = kind
  }
}

/** Runs a one-shot command and returns its result event, or throws EngineError. */
export async function runEngine<K extends EngineEvent['event']>(args: string[], expect: K): Promise<EventOf<K>> {
  let result: EventOf<K> | undefined
  let error: EventOf<'error'> | undefined
  const logs: string[] = []
  const job = await startEngine(args, {
    onEvent: (e) => {
      if (e.event === expect) result = e as EventOf<K>
      else if (e.event === 'error') error = e
    },
    onLog: (line) => logs.push(line),
  })
  const code = await job.exited
  if (error) throw new EngineError(error.kind, error.message)
  if (!result) throw new EngineError('exit', logs.at(-1) ?? `The engine exited with code ${code} and no result.`)
  return result
}
