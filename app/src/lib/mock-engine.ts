// Simulated engine for previewing the UI in a browser (outside the desktop app). It produces
// plausible events with the same shapes as the real engine; nothing here touches files.
import type { EngineEvent, EngineHandlers, EngineJob, TileRange, WorldParams } from './engine'

const TILES = 327
const TILE = 50

export const mockWorld: WorldParams = {
  worldSizeX: TILES * TILE,
  worldSizeZ: TILES * TILE,
  tilesX: TILES,
  tilesZ: TILES,
  tileWidth: TILE,
  tileHeight: TILE,
  seaLevel: 0.75,
  deepWaterDepth: 1.5,
  contourMaxError: 0.05,
  obstacles: true,
  blockSize: 16,
  filterIsolatedIslands: true,
  seed: [8192, 0, 8192],
  agent: { standHeight: 1.5, crouchHeight: 1.1, proneHeight: 0.6, diameter: 0.6, maxStepHeight: 0.45, maxSlope: 60 },
  cellClasses: [
    { cells: 500, width: 50, cellSize: 0.1 },
    { cells: 200, width: 50, cellSize: 0.25 },
  ],
  linkClasses: [
    { index: 0, typeId: 1, jumpLength: 2.5, jumpHeight: 1.1, minCenterHeight: 0.4, dropdownMin: -0.5, dropdownMax: 0.8, agentHeight: 1.5, maxStepHeight: 0.45, agentRadius: 0.3, flags: 128, area: 48 },
    { index: 1, typeId: 2, jumpLength: 1.5, jumpHeight: 2.2, minCenterHeight: 1.2, dropdownMin: 1.1, dropdownMax: 2.2, agentHeight: 1.5, maxStepHeight: 0.45, agentRadius: 0.3, flags: 512, area: 50 },
    { index: 2, typeId: 3, jumpLength: 2.0, jumpHeight: 3.5, minCenterHeight: 0, dropdownMin: 2.2, dropdownMax: 3.5, agentHeight: 1.5, maxStepHeight: 0.45, agentRadius: 0.3, flags: 256, area: 51 },
    { index: 3, typeId: 4, jumpLength: 2.0, jumpHeight: 5.0, minCenterHeight: 0, dropdownMin: 3.5, dropdownMax: 5.0, agentHeight: 1.5, maxStepHeight: 0.45, agentRadius: 0.3, flags: 256, area: 52 },
  ],
}

// Deterministic pseudo-random value in [0, 1) for a tile.
function hash(x: number, z: number, salt = 0) {
  let h = (x * 374761393 + z * 668265263 + salt * 2147483647) | 0
  h = Math.imul(h ^ (h >>> 13), 1274126177)
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296
}

/** 0 = sea, otherwise a land "density" in (0, 1]. */
export function mockLand(x: number, z: number) {
  const cx = TILES / 2
  const dx = (x - cx) / cx
  const dz = (z - cx) / cx
  const wobble = 0.12 * Math.sin(x * 0.07) + 0.1 * Math.cos(z * 0.05) + 0.06 * Math.sin((x + z) * 0.13)
  const d = Math.hypot(dx * 1.1, dz) + wobble
  if (d > 0.82) return 0
  return Math.min(1, (0.82 - d) * 2.5 + 0.2 * hash(x, z))
}

function delay(ms: number) {
  return new Promise((r) => setTimeout(r, ms))
}

function option(args: string[], name: string, count = 1) {
  const i = args.indexOf(name)
  return i < 0 ? undefined : args.slice(i + 1, i + 1 + count)
}

export async function startMockEngine(args: string[], { onEvent }: EngineHandlers): Promise<EngineJob> {
  let stopRequested = false
  let killed = false
  let resolveExit!: (code: number | null) => void
  const exited = new Promise<number | null>((r) => (resolveExit = r))
  const emit = (e: EngineEvent) => {
    if (!killed) onEvent(e)
  }

  const run = async (): Promise<number> => {
    const [cmd] = args
    await delay(300)
    switch (cmd) {
      case 'probe': {
        if (args[1] && !/^(127\.0\.0\.1|localhost)$/.test(args[1].split(',')[0])) {
          emit({ event: 'error', kind: 'network', message: `cannot connect to ${args[1].split(',')[0]}:36533` })
          return 1
        }
        emit({ event: 'world', host: args[1] ?? '127.0.0.1', params: mockWorld })
        return 0
      }
      case 'generate':
        return mockGenerate(args, emit, () => stopRequested || killed)
      case 'info': {
        const tileList: [number, number, number, number, number, number, number][] = []
        for (let z = 0; z < TILES; ++z)
          for (let x = 0; x < TILES; ++x) {
            const land = mockLand(x, z)
            const polys = land ? Math.round(40 + land * 900 * (0.5 + hash(x, z, 1))) : 2
            tileList.push([x, z, polys * 170, polys, Math.round(land * 30 * hash(x, z, 2)), Math.round(land * 40 * hash(x, z, 3)), Math.round(land * 20 * hash(x, z, 4))])
          }
        const sum = (i: number) => tileList.reduce((s, t) => s + t[i], 0)
        emit({
          event: 'info',
          path: args[1],
          tiles: tileList.length,
          tilesX: TILES,
          tilesZ: TILES,
          tileWidth: TILE,
          tileHeight: TILE,
          origin: [0, 0, 0],
          maxTiles: TILES * TILES,
          maxPolys: 1048576,
          polys: sum(3),
          verts: Math.round(sum(3) * 2.6),
          links: sum(4),
          doorParts: sum(6),
          doorPolys: Math.round(sum(6) * 0.15),
          obstacles: sum(5),
          agent: { height: 1.5, radius: 0.3, climb: 0.45 },
          cellSizes: [[0.1, 61000], [0.25, 45929]],
          areas: { 1: 4100000, 2: 21000, 3: 90000, 5: 2400000, 8: 610000, 12: 31000, 13: 52000, 48: 70000, 50: 21000, 51: 6000, 52: 9000 },
          flags: { 1: 4100000, 5: 52000, 7: 31000, 9: 610000, 32: 2400000, 128: 70000, 32769: 1200000 },
          tileList: args.includes('--tiles') ? tileList : undefined,
        })
        return 0
      }
      case 'tile': {
        const x = Number(args[2])
        const z = Number(args[3])
        const land = mockLand(x, z)
        const n = Math.round(land * 12)
        emit({
          event: 'tileDetail',
          x,
          z,
          ref: '0x0001000000100000',
          bmin: [x * TILE, -40, z * TILE],
          bmax: [(x + 1) * TILE, 60, (z + 1) * TILE],
          cellSize: land > 0.5 ? 0.1 : 0.25,
          bytes: Math.round(land * 120000) + 1200,
          polys: Math.round(land * 700) + 2,
          verts: Math.round(land * 1900) + 4,
          areas: land ? { 1: Math.round(land * 420), 8: Math.round(land * 80), 13: n } : { 5: 2 },
          flags: land ? { 1: Math.round(land * 420), 9: Math.round(land * 80), 5: n } : { 32: 2 },
          links: Array.from({ length: n }, (_, i) => ({
            class: i % 3,
            bidirectional: i % 2 === 0,
            from: [x * TILE + 5 + i * 3, 30, z * TILE + 10] as [number, number, number],
            to: [x * TILE + 6.5 + i * 3, 30.7, z * TILE + 10.2] as [number, number, number],
            centerHeight: 0.45,
            radius: 0.8,
            area: 48 + (i % 3),
            flags: 128,
          })),
          doors: Array.from({ length: Math.round(n / 2) }, (_, i) => ({ objectId: `0x00000000000${(4096 + i).toString(16)}`, part: i, state: i % 2, polys: 2 })),
          obstacles: Array.from({ length: n }, (_, i) => [x * TILE + 3 * i, 31, z * TILE + 40, 0.4] as [number, number, number, number]),
        })
        return 0
      }
      case 'render':
        emit({ event: 'render', output: args[2], width: 400, height: 400 })
        return 0
      case 'verify': {
        await delay(900)
        emit({ event: 'verify', compared: 26160, identical: 25961, ignoredIsolated: true, different: [[111, 14], [316, 19], [10, 32], [5, 34], [61, 26]] })
        return 1
      }
      case 'diff':
        await delay(900)
        emit({
          event: 'diff',
          compared: 13080,
          onlyInA: 13080,
          onlyInB: 0,
          sameCellSize: 13080,
          a: { polys: 783973, links: 113559, obstacles: 100326, doorParts: 65302 },
          b: { polys: 783973, links: 113487, obstacles: 100326, doorParts: 65302 },
          areaM2: { 1: [6.1e6, 6.1e6], 5: [2.2e7, 2.2e7], 8: [3.1e5, 3.1e5] },
          mismatchPercent: 0,
          worst: [{ x: 14, z: 40, areaDelta: 0, polys: [2, 2], links: [0, 0], cellSize: [0.25, 0.25] }],
        })
        return 0
      case 'merge':
        await delay(900)
        emit({ event: 'merge', replaced: 1240, output: args[3] })
        return 0
      case 'islands':
        await delay(900)
        emit({ event: 'islands', seedFound: true, seedPoint: [8209.4, 40.3, 8187.5], reachable: 4461219, isolated: 3119929, output: args[2] })
        return 0
      default:
        emit({ event: 'error', kind: 'usage', message: `--json is not supported by '${cmd}'` })
        return 1
    }
  }

  void run().then((code) => resolveExit(killed ? null : code))
  return {
    stop: async () => {
      stopRequested = true
    },
    kill: async () => {
      killed = true
    },
    exited,
  }
}

async function mockGenerate(args: string[], emit: (e: EngineEvent) => void, stopped: () => boolean) {
  const r = option(args, '--range', 4)?.map(Number)
  const range: TileRange = r ? [r[0], r[1], Math.min(r[2], TILES - 1), Math.min(r[3], TILES - 1)] : [0, 0, TILES - 1, TILES - 1]
  const order: [number, number][] = []
  for (let bz = range[1]; bz <= range[3]; bz += 16)
    for (let bx = range[0]; bx <= range[2]; bx += 16)
      for (let z = bz; z <= Math.min(range[3], bz + 15); ++z)
        for (let x = bx; x <= Math.min(range[2], bx + 15); ++x) order.push([x, z])
  emit({
    event: 'start',
    output: args[1],
    tiles: order.length,
    range,
    world: { tilesX: TILES, tilesZ: TILES, tileWidth: TILE, tileHeight: TILE },
    server: option(args, '--server')?.[0] ?? '127.0.0.1',
  })

  const t0 = performance.now()
  const rate = 450 // tiles/s, faster than real so a preview run is watchable
  let fetched = 0
  let done = 0
  let built = 0
  let empty = 0
  let failed = 0
  const errors: string[] = []
  const queue: [number, number][] = []
  let lastProgress = 0
  while (done < order.length && !stopped()) {
    await delay(50)
    const target = Math.min(order.length, Math.floor(((performance.now() - t0) / 1000) * rate))
    while (fetched < Math.min(order.length, target + 60)) {
      const [x, z] = order[fetched++]
      queue.push([x, z])
      emit({ event: 'fetched', x, z })
    }
    while (done < target && queue.length) {
      const [x, z] = queue.shift()!
      ++done
      const land = mockLand(x, z)
      if (land > 0 && hash(x, z, 9) < 0.0004) {
        ++failed
        errors.push(`tile (${x},${z}): Could not build Detour navmesh.`)
        emit({ event: 'tile', x, z, outcome: 'failed', ms: 800, error: 'Could not build Detour navmesh.' })
      } else if (land === 0 && hash(x, z, 8) < 0.002) {
        ++empty
        emit({ event: 'tile', x, z, outcome: 'empty', ms: 40 })
      } else {
        ++built
        const polys = land ? Math.round(40 + land * 900) : 2
        emit({ event: 'tile', x, z, outcome: 'built', ms: 200 + land * 2400, bytes: polys * 170, polys, verts: polys * 3, links: Math.round(land * 20), obstacles: Math.round(land * 30) })
      }
    }
    const now = performance.now()
    if (now - lastProgress > 250) {
      lastProgress = now
      const elapsed = (now - t0) / 1000
      emit({ event: 'progress', done, total: order.length, fetched, failed, elapsed, rate: done / elapsed, eta: (order.length - done) / Math.max(1, done / elapsed) })
    }
  }
  const cancelled = done < order.length
  const elapsed = (performance.now() - t0) / 1000
  emit({ event: 'progress', done, total: order.length, fetched, failed, elapsed, rate: done / elapsed, eta: 0 })
  if (!cancelled)
    emit({ event: 'islands', seedFound: true, seed: [8192, 0, 8192], seedPoint: [8209.4, 40.3, 8187.5], reachable: 4461219, isolated: 3119929 })
  emit({
    event: 'done',
    output: args[1],
    saved: built + empty > 0,
    cancelled,
    built,
    empty,
    failed,
    seconds: elapsed,
    strayTiles: 0,
    resumable: cancelled,
    errorCount: errors.length,
    errors,
  })
  return cancelled ? 130 : failed ? 1 : 0
}
