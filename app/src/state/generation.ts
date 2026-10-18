// The generation job: runs `dznav generate`, and keeps per-tile state for the map.
import { runEngine, startEngine, type EngineEvent, type EngineJob, type EventOf, type TileRange } from '@/lib/engine'
import { fileName } from '@/lib/format'

import { Store } from './store'

export const TileState = {
  Outside: 0,
  Pending: 1,
  Queued: 2,
  Built: 3,
  Empty: 4,
  Failed: 5,
  /** Already in the navmesh being updated, so this run leaves it alone (Fill in missing). */
  Existing: 6,
} as const
export type TileState = (typeof TileState)[keyof typeof TileState]

export interface GenerateOptions {
  output: string
  source: 'server' | 'capture'
  server: string
  capture: string
  range: TileRange | null
  base: string
  skipExisting: boolean
  seaLevel: number | null
  doors: 'closed' | 'open' | 'enabled'
  links: boolean
  islandFilter: boolean
  threads: number | ''
  checkpoint: number | ''
  record: string
}

export function generateArgs(o: GenerateOptions): string[] {
  const args = ['generate', o.output]
  if (o.source === 'capture') args.push('--capture', o.capture)
  else args.push('--server', o.server)
  if (o.range) args.push('--range', ...o.range.map(String))
  if (o.base) {
    args.push('--base', o.base)
    if (o.skipExisting) args.push('--skip-existing')
  }
  if (o.seaLevel !== null) args.push('--sea-level', String(o.seaLevel))
  if (o.doors !== 'closed') args.push('--doors', o.doors)
  if (!o.links) args.push('--no-links')
  if (!o.islandFilter) args.push('--no-island-filter')
  if (o.threads) args.push('--threads', String(o.threads))
  if (o.checkpoint) args.push('--checkpoint', String(o.checkpoint))
  if (o.record) args.push('--record', o.record)
  return args
}

export type JobStatus = 'idle' | 'starting' | 'running' | 'stopping' | 'finished' | 'stopped' | 'failed'

export interface LogEntry {
  time: Date
  level: 'info' | 'warn' | 'error'
  message: string
}

export interface JobSummary {
  status: JobStatus
  args: string[]
  output: string
  start?: EventOf<'start'>
  progress?: EventOf<'progress'>
  done?: EventOf<'done'>
  islands?: EventOf<'islands'>
  error?: string
  built: number
  empty: number
  failed: number
  queued: number
  strayTiles: number
  log: LogEntry[]
  /** Bumped whenever tile states change (the map redraws on change). */
  tileVersion: number
}

const MAX_LOG = 1000

class GenerationJob {
  readonly summary = new Store<JobSummary>(emptySummary())
  /** Per tile (x + z * tilesX). */
  states = new Uint8Array(0)
  /** Build time per tile in ms (for the metric view). */
  buildMs = new Float32Array(0)
  tilesX = 0
  tilesZ = 0
  private job: EngineJob | null = null
  /** Stop or kill asked for before the engine was started, carried out once it is. */
  private requested: 'stop' | 'kill' | null = null
  /** Set by kill(). A killed engine still has an exit code on Windows (1), so it can't tell us itself. */
  private killed = false
  /** Tiles the navmesh being updated already has, which a Fill in missing run skips. */
  private existing: { tilesX: number; tiles: [number, number][] } | null = null
  /** Tiles already reported as failed (their errors repeat in the final summary). */
  private failedTiles = new Set<string>()
  private pending: Partial<JobSummary> = {}
  private flushScheduled = false

  get running() {
    const s = this.summary.get().status
    return s === 'starting' || s === 'running' || s === 'stopping'
  }

  /** Resolves once no run is in progress. */
  whenIdle(): Promise<void> {
    return new Promise((resolve) => {
      if (!this.running) return resolve()
      const unsubscribe = this.summary.subscribe(() => {
        if (!this.running) {
          unsubscribe()
          resolve()
        }
      })
    })
  }

  /** Prepares the map for a world before any run. */
  setWorld(tilesX: number, tilesZ: number) {
    if (this.running || (tilesX === this.tilesX && tilesZ === this.tilesZ)) return
    this.tilesX = tilesX
    this.tilesZ = tilesZ
    this.states = new Uint8Array(tilesX * tilesZ)
    this.buildMs = new Float32Array(tilesX * tilesZ)
    this.summary.set((s) => ({ ...s, tileVersion: s.tileVersion + 1 }))
  }

  /** `notes` are logged after the command, to explain choices the app made for this run. */
  async start(options: GenerateOptions, notes: string[] = []) {
    if (this.running) return
    const args = generateArgs(options)
    this.failedTiles.clear()
    this.requested = null
    this.killed = false
    this.existing = null
    this.summary.set({ ...emptySummary(), status: 'starting', args, output: options.output, tileVersion: this.summary.get().tileVersion + 1 })
    this.log('info', `dznav ${args.map(quote).join(' ')}`)
    for (const note of notes) this.log('info', note)
    // The engine doesn't report the tiles Fill in missing skips, so read them from the navmesh first to show them on
    // the map. Not alongside the run: the engine may save over that file while it's being read.
    if (options.base && options.skipExisting) {
      this.log('info', `Reading ${fileName(options.base)} for the tiles it already has.`)
      try {
        const info = await runEngine(['info', options.base, '--tiles'], 'info')
        this.existing = { tilesX: info.tilesX, tiles: (info.tileList ?? []).map(([x, z]) => [x, z]) }
      } catch {
        // The run reports a navmesh it can't read; the map just won't show the tiles it keeps.
      }
    }
    if (this.requested) {
      this.finish('stopped', 'Stopped before the run started. Nothing was changed.')
      return
    }
    try {
      this.job = await startEngine(args, {
        onEvent: (e) => this.onEvent(e),
        onLog: (line) => this.log('warn', line),
      })
    } catch (e) {
      this.finish('failed', String(e))
      return
    }
    if (this.requested === 'kill') void this.job.kill().catch(ignore)
    else if (this.requested === 'stop') void this.job.stop().catch(ignore)
    const code = await this.job.exited
    this.job = null
    this.flush()
    const s = this.summary.get()
    if (s.status === 'failed') return
    if (this.killed || code === null) this.finish('stopped', 'Stopped without saving.')
    else if (s.done?.cancelled) this.finish('stopped')
    // Saved with every tile attempted; individual failed tiles are reported in the summary.
    else if (s.done?.saved && !s.done.resumable) this.finish('finished')
    else if (s.done?.resumable) this.finish('failed', s.done.errors?.[0] ? `The run ended early: ${s.done.errors[0]}` : 'The run ended early.')
    else this.finish('failed', s.error ?? (s.done?.errors?.[0] ? `${s.done.errorCount} errors, first: ${s.done.errors[0]}` : `The engine exited with code ${code}.`))
  }

  // Both may be asked for before the engine has started (while reading the navmesh to update), and may race the engine
  // ending by itself, which the exit handles; so neither fails.
  async stop() {
    const status = this.summary.get().status
    if (status !== 'starting' && status !== 'running') return
    this.summary.set((s) => ({ ...s, status: 'stopping' }))
    this.log('info', 'Stopping: finishing the tiles in progress, then saving.')
    if (this.job) await this.job.stop().catch(ignore)
    else this.requested ??= 'stop'
  }

  async kill() {
    if (!this.running) return
    this.killed = true
    this.log('warn', 'Ending the engine without saving.')
    if (this.job) await this.job.kill().catch(ignore)
    else this.requested = 'kill'
  }

  private onEvent(e: EngineEvent) {
    switch (e.event) {
      case 'start': {
        const { tilesX, tilesZ } = e.world
        if (tilesX !== this.tilesX || tilesZ !== this.tilesZ) {
          this.tilesX = tilesX
          this.tilesZ = tilesZ
          this.states = new Uint8Array(tilesX * tilesZ)
          this.buildMs = new Float32Array(tilesX * tilesZ)
        } else {
          this.states.fill(TileState.Outside)
          this.buildMs.fill(0)
        }
        const [x0, z0, x1, z1] = e.range
        for (let z = z0; z <= z1; ++z) this.states.fill(TileState.Pending, z * tilesX + x0, z * tilesX + x1 + 1)
        if (e.skipExisting && this.existing?.tilesX === tilesX)
          for (const [x, z] of this.existing.tiles) if (x >= x0 && x <= x1 && z >= z0 && z <= z1) this.states[z * tilesX + x] = TileState.Existing
        // A stop asked for while starting stays in force.
        this.update({ status: this.summary.get().status === 'stopping' ? 'stopping' : 'running', start: e })
        this.log('info', `Generating ${e.tiles.toLocaleString('en-GB')} tiles, x ${x0}–${x1}, z ${z0}–${z1}.`)
        break
      }
      case 'fetched':
        this.states[e.z * this.tilesX + e.x] = TileState.Queued
        this.pending.queued = (this.pending.queued ?? this.summary.get().queued) + 1
        this.schedule()
        break
      case 'tile': {
        const i = e.z * this.tilesX + e.x
        this.states[i] = e.outcome === 'built' ? TileState.Built : e.outcome === 'empty' ? TileState.Empty : TileState.Failed
        this.buildMs[i] = e.ms
        const s = { ...this.summary.get(), ...this.pending }
        this.pending.queued = Math.max(0, s.queued - 1)
        if (e.outcome === 'built') this.pending.built = s.built + 1
        else if (e.outcome === 'empty') this.pending.empty = s.empty + 1
        else {
          this.pending.failed = s.failed + 1
          this.failedTiles.add(`${e.x},${e.z}`)
          this.log('error', `Tile ${e.x}, ${e.z} failed: ${e.error ?? 'unknown error'}`)
        }
        if (e.strayPartIds) this.pending.strayTiles = s.strayTiles + 1
        this.schedule()
        break
      }
      case 'progress':
        this.pending.progress = e
        this.schedule()
        break
      case 'saved':
        this.log('info', `Checkpoint saved to ${e.path}.`)
        break
      case 'islands':
        this.update({ islands: e })
        this.log(
          'info',
          e.seedFound
            ? `Island filter: ${e.reachable?.toLocaleString('en-GB')} polygons reachable, ${e.isolated?.toLocaleString('en-GB')} isolated.`
            : 'Island filter skipped: no polygon near the seed in the generated area.',
        )
        break
      case 'done':
        this.update({ done: e })
        if (e.cancelled) this.log('warn', e.saved ? `Stopped. Saved ${e.output}; resume to build the rest.` : 'Stopped before the first tile; nothing was saved.')
        else this.log(e.failed ? 'warn' : 'info', `Finished in ${Math.round(e.seconds ?? 0)} s: ${e.built} built, ${e.empty} empty, ${e.failed} failed. Saved ${e.output}.`)
        for (const err of e.errors ?? []) {
          const m = /^tile \((\d+),(\d+)\)/.exec(err)
          if (!m || !this.failedTiles.has(`${m[1]},${m[2]}`)) this.log('error', err)
        }
        break
      case 'error':
        this.update({ error: e.message })
        this.log('error', e.message)
        break
    }
  }

  private finish(status: JobStatus, error?: string) {
    // Tiles fetched but never built (the run stopped) are pending again.
    for (let i = 0; i < this.states.length; ++i) if (this.states[i] === TileState.Queued) this.states[i] = TileState.Pending
    this.update({ status, queued: 0, ...(error ? { error } : {}) })
  }

  private log(level: LogEntry['level'], message: string) {
    const log = [...(this.pending.log ?? this.summary.get().log), { time: new Date(), level, message }]
    this.pending.log = log.length > MAX_LOG ? log.slice(-MAX_LOG) : log
    this.schedule()
  }

  private update(patch: Partial<JobSummary>) {
    Object.assign(this.pending, patch)
    this.flush()
  }

  // Tile events arrive ~100 a second; coalesce them into 10 updates a second. (A timer rather than
  // requestAnimationFrame, which stops while the window is minimised.)
  private schedule() {
    if (this.flushScheduled) return
    this.flushScheduled = true
    setTimeout(() => this.flush(), 100)
  }

  private flush() {
    this.flushScheduled = false
    const patch = this.pending
    this.pending = {}
    this.summary.set((s) => ({ ...s, ...patch, tileVersion: s.tileVersion + 1 }))
  }
}

function emptySummary(): JobSummary {
  return { status: 'idle', args: [], output: '', built: 0, empty: 0, failed: 0, queued: 0, strayTiles: 0, log: [], tileVersion: 0 }
}

function ignore() {}

function quote(arg: string) {
  return /[\s"]/.test(arg) ? `"${arg.replace(/"/g, '\\"')}"` : arg
}

export const generation = new GenerationJob()
