import { ChevronRight, CircleAlert, Info, LoaderCircle, Play, RotateCcw, Square, TriangleAlert, X } from 'lucide-react'
import { Children, useCallback, useEffect, useMemo, useRef, useState, type ReactNode } from 'react'

import type { Page } from '@/components/app-sidebar'
import { AdvancedGuide, type GuideTopic } from '@/components/advanced-guide'
import { Field, FileField, PageHeader, Stat, Swatch, SwitchField, TextField } from '@/components/common'
import { ServerConnection, StartServerHelp } from '@/components/server'
import { TileGrid } from '@/components/tile-grid'
import { Alert, AlertDescription, AlertTitle } from '@/components/ui/alert'
import { Button } from '@flare/ui/ui/button'
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '@flare/ui/ui/dialog'
import { Input } from '@flare/ui/ui/input'
import { Progress } from '@flare/ui/ui/progress'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@flare/ui/ui/select'
import { Separator } from '@/components/ui/separator'
import { ToggleGroup, ToggleGroupItem } from '@flare/ui/ui/toggle-group'
import { cssVarRgb, viridisAt, viridisGradient, type Rgb } from '@/lib/colors'
import type { TileRange } from '@/lib/engine'
import { fileExists } from '@/lib/files'
import { fileName, formatDuration, formatInt, formatNumber, formatPercent } from '@/lib/format'
import { captureFilters, navmeshFilters } from '@/lib/navmesh'
import { cn } from '@/lib/utils'
import { connection, settings } from '@/state/connection'
import { generation, generateArgs, TileState, type GenerateOptions, type LogEntry } from '@/state/generation'
import { openNavmesh } from '@/state/inspect'
import { persistentStore, useStore } from '@/state/store'
import { useResolvedTheme } from '@/state/theme'

const options = persistentStore<Omit<GenerateOptions, 'server' | 'threads'>>('dznav.generate', {
  output: '',
  source: 'server',
  capture: '',
  range: null,
  base: '',
  skipExisting: false,
  seaLevel: null,
  doors: 'closed',
  links: true,
  islandFilter: true,
  checkpoint: '',
  record: '',
})

// The sea level each navmesh was built with (by path), so resuming or updating it keeps its coastline.
const runSeaLevels = persistentStore<Record<string, number>>('dznav.seaLevels', {})

// The output last picked in the save dialog, which has already asked about replacing it. For this session only: after
// a restart, a remembered path that holds a navmesh is asked about again.
let confirmedOutput = ''

type Errors = Partial<Record<'output' | 'capture' | 'range' | 'base' | 'seaLevel', string>>

export function GeneratePage({ onNavigate, onOpenSettings }: { onNavigate: (page: Page) => void; onOpenSettings: () => void }) {
  const opts = useStore(options)
  const { server, threads, expert } = useStore(settings)
  const conn = useStore(connection)
  const job = useStore(generation.summary)
  const [errors, setErrors] = useState<Errors>({})
  const [viewChoice, setView] = useState<'status' | 'time'>('status')
  // Build-time colouring is an expert view.
  const view = expert ? viewChoice : 'status'
  const [picked, setPicked] = useState<[number, number] | null>(null)
  const [updating, setUpdating] = useState(opts.base !== '')
  // `lastRun`: the file is the one the last run saved, which a resume could build on instead.
  const [confirmReplace, setConfirmReplace] = useState<{ options: GenerateOptions; lastRun: boolean } | null>(null)
  const [guide, setGuide] = useState<{ open: boolean; topic: GuideTopic | null }>({ open: false, topic: null })
  const help = (topic: GuideTopic) => () => setGuide({ open: true, topic })

  const world = conn.status === 'connected' ? conn.world : null
  const tilesX = generation.tilesX || world?.tilesX || 0
  const tilesZ = generation.tilesZ || world?.tilesZ || 0
  useEffect(() => {
    if (world) generation.setWorld(world.tilesX, world.tilesZ)
  }, [world])

  const running = job.status === 'starting' || job.status === 'running' || job.status === 'stopping'
  const set = (patch: Partial<typeof opts>) => {
    options.set((o) => ({ ...o, ...patch }))
    setErrors({})
  }
  const full: GenerateOptions = { ...opts, server: server.trim(), threads }

  const validate = (o: GenerateOptions, requireBase: boolean): Errors => {
    const e: Errors = {}
    if (!o.output.trim()) e.output = 'Choose where to save the navmesh.'
    else if (!o.output.trim().toLowerCase().endsWith('.nm')) e.output = 'Use a .nm file name.'
    if (o.source === 'capture' && !o.capture.trim()) e.capture = 'Choose a .dzgeo capture, or use the data server.'
    if ((requireBase || o.skipExisting) && !o.base.trim()) e.base = 'Choose the navmesh to update.'
    if (o.range) {
      const [x0, z0, x1, z1] = o.range
      if ([x0, z0, x1, z1].some((v) => !Number.isInteger(v) || v < 0)) e.range = 'Tile numbers are whole numbers from 0.'
      else if (x0 > x1 || z0 > z1) e.range = 'The first corner must be the lower one (x0 ≤ x1, z0 ≤ z1).'
      else if (tilesX && (x1 >= tilesX || z1 >= tilesZ)) e.range = `This world has tiles 0–${tilesX - 1} by 0–${tilesZ - 1}.`
    }
    if (o.seaLevel !== null && !Number.isFinite(o.seaLevel)) e.seaLevel = 'Enter the sea level in metres.'
    return e
  }

  // Unless pinned, a server run keeps the sea level of the navmesh it builds on, or else uses the
  // tide read when connecting, so stopping and resuming in a later session can't shift the coast.
  // (A capture file holds its own sea level.)
  const autoSeaLevel = (o: GenerateOptions): { value: number; from: 'base' | 'tide' } | null => {
    if (o.source !== 'server') return null
    const fromBase = o.base ? runSeaLevels.get()[pathKey(o.base)] : undefined
    if (fromBase !== undefined) return { value: fromBase, from: 'base' }
    return world ? { value: world.seaLevel, from: 'tide' } : null
  }
  const auto = autoSeaLevel(updating ? full : { ...full, base: '' })

  // The last run saved tiles that a new run would build on (Resume) or replace (Start over).
  const canResume = !running && !!job.done?.saved && (job.status === 'stopped' || (job.status === 'failed' && !!job.done.resumable))

  const start = async (o: GenerateOptions = full, { confirmed = false, requireBase = updating } = {}) => {
    const e = validate(o, requireBase)
    setErrors(e)
    if (Object.keys(e).length) return
    // Writing over a navmesh without building on it throws its tiles away: the last run's, or one from before.
    if (!confirmed && !samePath(o.base, o.output)) {
      const lastRun = !!job.done?.saved && samePath(o.output, job.output)
      const saved = lastRun || (!samePath(o.output, confirmedOutput) && (await fileExists(o.output.trim()).catch(() => false)))
      if (saved) {
        setConfirmReplace({ options: o, lastRun })
        return
      }
    }
    // Starting fresh (Start over) turns off the update options a resume left behind.
    if (!o.base && opts.base) {
      options.set((prev) => ({ ...prev, base: '', skipExisting: false }))
      setUpdating(false)
    }
    setConfirmReplace(null)
    setPicked(null)
    const sea = o.seaLevel === null ? autoSeaLevel(o) : null
    const seaLevel = sea ? sea.value : o.seaLevel
    if (seaLevel !== null) runSeaLevels.set((m) => ({ ...m, [pathKey(o.output)]: seaLevel }))
    const notes = sea
      ? [
          sea.from === 'base'
            ? `Sea level ${formatNumber(sea.value)} m, the same as the run that built ${fileName(o.base)}.`
            : `Sea level ${formatNumber(sea.value)} m, the server's tide when you connected.`,
        ]
      : []
    void generation.start({ ...o, seaLevel }, notes)
  }

  const startOver = () => void start({ ...full, base: '', skipExisting: false }, { requireBase: false })

  const resume = () => {
    const o = { ...full, base: job.output, skipExisting: true }
    options.set((prev) => ({ ...prev, base: job.output, skipExisting: true }))
    setUpdating(true)
    setConfirmReplace(null)
    void start(o)
  }

  // Colours for the map, resolved from the theme.
  const theme = useResolvedTheme()
  const palette = useMemo(() => paletteFromTheme(theme), [theme])
  const range = job.start?.range ?? opts.range
  const paint = useCallback(
    (rgba: Uint8ClampedArray, w: number, h: number) => {
      const states = generation.states
      const ms = generation.buildMs
      const hasRun = states.length === w * h && states.some((s) => s !== TileState.Outside)
      let maxMs = 1
      if (view === 'time') for (let i = 0; i < ms.length; ++i) maxMs = Math.max(maxMs, ms[i])
      const logMax = Math.log1p(maxMs)
      for (let z = 0; z < h; ++z)
        for (let x = 0; x < w; ++x) {
          const i = z * w + x
          let state: number = hasRun ? states[i] : TileState.Pending
          if (!hasRun && range && (x < range[0] || x > range[2] || z < range[1] || z > range[3])) state = TileState.Outside
          let c = palette[state]
          if (view === 'time' && (state === TileState.Built || state === TileState.Empty) && ms[i] > 0) c = viridisAt(Math.log1p(ms[i]) / logMax)
          const p = ((h - 1 - z) * w + x) * 4
          rgba[p] = c[0]
          rgba[p + 1] = c[1]
          rgba[p + 2] = c[2]
          rgba[p + 3] = 255
        }
    },
    [palette, view, range],
  )

  // The run's world when there is one: a capture run may have no server connected, or another world on it.
  const tileWidth = job.start?.world.tileWidth ?? world?.tileWidth ?? 50
  const tileHeight = job.start?.world.tileHeight ?? world?.tileHeight ?? 50
  const describe = useCallback(
    (x: number, z: number) => {
      const i = z * generation.tilesX + x
      const s = generation.states[i]
      const ms = generation.buildMs[i]
      const name = stateNames[s as TileState] ?? 'pending'
      const tile = `· ${formatInt(x * tileWidth)}–${formatInt((x + 1) * tileWidth)} m E, ${formatInt(z * tileHeight)}–${formatInt((z + 1) * tileHeight)} m N`
      return `${name}${ms ? ` in ${formatNumber(ms / 1000, 1)} s` : ''} ${tile}`
    },
    [tileWidth, tileHeight],
  )

  // Per-state tile counts and the slowest tile, for the legends.
  const { counts, maxMs } = useMemo(() => tileStats(job.tileVersion), [job.tileVersion])

  const p = job.progress
  const hasMap = tilesX > 0 && tilesZ > 0

  return (
    <>
      <PageHeader
        title="Generate"
        description={
          job.start ? `${running ? 'Building' : 'Last run:'} ${job.start.output}` : 'Connect to DayZDiag, choose where to save, then press Start.'
        }
        actions={
          running ? (
            <>
              {job.status === 'stopping' && (
                <Button variant="ghost" onClick={() => void generation.kill()}>
                  <X />
                  End without saving
                </Button>
              )}
              <Button variant="destructive" size="lg" onClick={() => void generation.stop()} disabled={job.status === 'stopping'}>
                {job.status === 'stopping' ? <LoaderCircle className="animate-spin" /> : <Square />}
                {job.status === 'stopping' ? 'Stopping…' : 'Stop and save'}
              </Button>
            </>
          ) : canResume ? (
            <>
              <Button variant="outline" size="lg" onClick={startOver}>
                <Play />
                Start over
              </Button>
              <Button size="lg" onClick={resume}>
                <RotateCcw />
                Resume
              </Button>
            </>
          ) : (
            <Button size="lg" onClick={() => void start()}>
              <Play />
              Start
            </Button>
          )
        }
      />
      <Dialog open={!!confirmReplace} onOpenChange={(open) => !open && setConfirmReplace(null)}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>Replace {fileName(confirmReplace?.options.output ?? '')}?</DialogTitle>
            <DialogDescription>
              {!confirmReplace?.lastRun
                ? 'A navmesh is already saved there. Starting replaces it. To build into it instead, turn on Update an existing navmesh.'
                : canResume
                  ? `It holds the ${formatInt(job.done?.built ?? 0)} tiles your last run built. Starting over replaces them. Resume to keep them and build the rest.`
                  : 'It holds the navmesh your last run built. Starting over replaces it.'}
            </DialogDescription>
          </DialogHeader>
          <DialogFooter>
            <Button variant="ghost" onClick={() => setConfirmReplace(null)}>
              Cancel
            </Button>
            {confirmReplace?.lastRun && canResume && (
              <Button variant="outline" onClick={resume}>
                Resume instead
              </Button>
            )}
            <Button
              variant="destructive"
              onClick={() => confirmReplace && void start(confirmReplace.options, { confirmed: true, requireBase: !!confirmReplace.options.base })}
            >
              Replace and start
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
      <AdvancedGuide open={guide.open} topic={guide.topic} onOpenChange={(open) => setGuide((g) => ({ ...g, open }))} />
      <div className="flex min-h-0 flex-1">
        {/* Options */}
        <aside aria-label="Generation options" className="relative w-88 shrink-0 overflow-y-auto border-r p-4">
          {/* Outside the fieldset so Server details stays usable during a run. */}
          {opts.source === 'server' && (
            <>
              <Section title="Data server">
                <ServerConnection disabled={running} />
              </Section>
              <Separator className="my-5" />
            </>
          )}
          <fieldset disabled={running} className="grid gap-5 disabled:opacity-60">
            {opts.source === 'capture' && (
              <Section title="Capture file">
                <FileField
                  label="Read geometry from"
                  mode="open"
                  filters={captureFilters}
                  value={opts.capture}
                  onChange={(capture) => set({ capture })}
                  hint="Geometry saved by an earlier run, so DayZDiag doesn't need to be running. Switch back to the data server under Advanced."
                  error={errors.capture}
                />
              </Section>
            )}
            {opts.source === 'capture' && <Separator />}

            <Section title="Output">
              <FileField
                label="Save navmesh as"
                mode="save"
                filters={navmeshFilters}
                value={opts.output}
                onChange={(output, chosen) => {
                  if (chosen) confirmedOutput = output
                  set({ output })
                }}
                placeholder="C:\…\world.nm"
                error={errors.output}
              />
            </Section>

            <Separator />

            <Section title="Tiles">
              <Field label="Build" hint={opts.range ? 'Or drag on the map to choose a range.' : 'Drag on the map to choose a range instead.'} error={errors.range}>
                <ToggleGroup
                  type="single"
                  value={opts.range ? 'range' : 'all'}
                  onValueChange={(v) => v && set({ range: v === 'all' ? null : (opts.range ?? [0, 0, Math.max(0, tilesX - 1), Math.max(0, tilesZ - 1)]) })}
                  className="w-full"
                >
                  <ToggleGroupItem value="all" className="h-8 flex-1 text-[13px]">
                    Whole world
                  </ToggleGroupItem>
                  <ToggleGroupItem value="range" className="h-8 flex-1 text-[13px]">
                    Range
                  </ToggleGroupItem>
                </ToggleGroup>
                {opts.range && <RangeInputs range={opts.range} onChange={(range) => set({ range })} invalid={!!errors.range} />}
              </Field>
              <SwitchField
                label="Update an existing navmesh"
                hint="Build into a navmesh you already have instead of starting empty."
                checked={updating}
                onCheckedChange={(on) => {
                  setUpdating(on)
                  if (!on) set({ base: '', skipExisting: false })
                }}
              />
              {updating && (
                <>
                  <FileField label="Navmesh to update" mode="open" filters={navmeshFilters} value={opts.base} onChange={(base) => set({ base })} error={errors.base} />
                  <Field
                    label="Which tiles to build"
                    hint={
                      opts.skipExisting
                        ? 'Skips tiles the navmesh already has. Use this to finish a stopped run.'
                        : 'Rebuilds the chosen tiles and replaces them; every other tile is kept.'
                    }
                  >
                    <ToggleGroup
                      type="single"
                      value={opts.skipExisting ? 'missing' : 'replace'}
                      onValueChange={(v) => v && set({ skipExisting: v === 'missing' })}
                      className="w-full"
                    >
                      <ToggleGroupItem value="replace" className="h-8 flex-1 text-[13px]">
                        Replace tiles
                      </ToggleGroupItem>
                      <ToggleGroupItem value="missing" className="h-8 flex-1 text-[13px]">
                        Fill in missing
                      </ToggleGroupItem>
                    </ToggleGroup>
                  </Field>
                </>
              )}
            </Section>

            <Separator />

            <details className="group">
              <summary className="-mx-2 flex h-9 cursor-pointer list-none items-center gap-1 rounded-md px-2 text-sm font-semibold transition-colors duration-150 outline-none hover:bg-accent/50 focus-visible:ring-2 focus-visible:ring-ring/50 [&::-webkit-details-marker]:hidden">
                <ChevronRight className="size-4 text-muted-foreground transition-transform group-open:rotate-90" aria-hidden />
                Advanced
              </summary>
              <p className="mt-0.5 text-xs text-muted-foreground">
                The defaults suit most maps.{' '}
                <button type="button" className="underline underline-offset-2 hover:text-foreground" onClick={() => setGuide({ open: true, topic: null })}>
                  What do these do?
                </button>
              </p>
              <div className="mt-5 grid gap-7">
                <AdvancedGroup title="Navmesh">
                  <div className="grid gap-3">
                    <SwitchField
                      label="Pin sea level"
                      hint={
                        opts.seaLevel !== null
                          ? 'Everything below this height is marked as sea.'
                          : opts.source === 'capture'
                            ? 'When off, the sea level saved in the capture file is used.'
                            : auto?.from === 'base'
                              ? `When off, the original run's sea level (${formatNumber(auto.value)} m) is used, so the coastline matches.`
                              : auto
                                ? `When off, the server's tide when you connected (${formatNumber(auto.value)} m) is used.`
                                : "When off, the server's tide at the start of the run is used."
                      }
                      checked={opts.seaLevel !== null}
                      onCheckedChange={(on) => set({ seaLevel: on ? (auto?.value ?? NaN) : null })}
                      onHelp={help('sea-level')}
                    />
                    {opts.seaLevel !== null && (
                      <TextField
                        label="Sea level (m)"
                        value={String(opts.seaLevel)}
                        onChange={(v) => set({ seaLevel: v.trim() === '' ? NaN : Number(v) })}
                        error={errors.seaLevel}
                        mono
                        inputMode="decimal"
                      />
                    )}
                  </div>
                  <Field label="Door state in the navmesh" hint="Which state doors start in. The game updates them as they open and close." onHelp={help('doors')}>
                    <Select value={opts.doors} onValueChange={(doors) => set({ doors: doors as GenerateOptions['doors'] })}>
                      <SelectTrigger className="w-full">
                        <SelectValue />
                      </SelectTrigger>
                      <SelectContent>
                        <SelectItem value="closed">Closed</SelectItem>
                        <SelectItem value="open">Open</SelectItem>
                        <SelectItem value="enabled">Open or closed (both walkable)</SelectItem>
                      </SelectContent>
                    </Select>
                  </Field>
                  <SwitchField
                    label="Jump and climb links"
                    hint="Lets AI jump over, climb onto and drop down from obstacles."
                    checked={opts.links}
                    onCheckedChange={(links) => set({ links })}
                    onHelp={help('links')}
                  />
                  <SwitchField
                    label="Flag unreachable areas"
                    hint="Marks places AI can never reach, such as rooftops, so AI doesn't use them."
                    checked={opts.islandFilter}
                    onCheckedChange={(islandFilter) => set({ islandFilter })}
                    onHelp={help('islands')}
                  />
                </AdvancedGroup>

                <AdvancedGroup title="Geometry and saving">
                  <SwitchField
                    label="Build from a capture file"
                    hint="Use geometry saved by an earlier run instead of the data server."
                    checked={opts.source === 'capture'}
                    onCheckedChange={(on) => set({ source: on ? 'capture' : 'server' })}
                    onHelp={help('capture')}
                  />
                  <FileField
                    label="Also save geometry to a capture file"
                    mode="save"
                    filters={captureFilters}
                    value={opts.record}
                    onChange={(record) => set({ record })}
                    hint="Lets you regenerate later without DayZDiag running."
                    onHelp={help('record')}
                  />
                  <TextField
                    label="Auto-save every (tiles)"
                    value={String(opts.checkpoint)}
                    onChange={(v) => set({ checkpoint: v === '' ? '' : Math.max(1, Math.floor(Number(v)) || 1) })}
                    placeholder="off"
                    hint="Leave empty to save only at the end. A crash loses at most this many tiles."
                    mono
                    inputMode="numeric"
                    onHelp={help('autosave')}
                  />
                </AdvancedGroup>
                <p className="text-xs text-muted-foreground">
                  Builder threads: <span className="font-mono text-foreground">{threads || 'cores − 1'}</span>.{' '}
                  <button type="button" className="underline underline-offset-2 hover:text-foreground" onClick={onOpenSettings}>
                    Change in Settings
                  </button>
                </p>
              </div>
            </details>
          </fieldset>

          {expert && (
            <details className="mt-4 text-xs">
              <summary className="cursor-pointer text-muted-foreground">Command line</summary>
              <code className="mt-1 block rounded-sm bg-muted p-2 font-mono break-all selectable">dznav {generateArgs(full).join(' ')}</code>
            </details>
          )}
        </aside>

        {/* Map */}
        <section aria-label="Tile map" className="flex min-w-0 flex-1 flex-col overflow-hidden">
          {hasMap ? (
            <>
              <div className="flex min-h-10 shrink-0 items-center gap-3 border-b px-3 py-1.5">
                {expert && (
                  <ToggleGroup type="single" value={view} onValueChange={(v) => v && setView(v as typeof view)} aria-label="Colour tiles by">
                    <ToggleGroupItem value="status">Status</ToggleGroupItem>
                    <ToggleGroupItem value="time">Build time</ToggleGroupItem>
                  </ToggleGroup>
                )}
                <div className="flex min-w-0 flex-1 flex-wrap items-center gap-x-3 gap-y-1 text-xs">
                  {view === 'status' ? (
                    <>
                      <LegendItem color={palette[TileState.Pending]} label="Pending" count={counts[TileState.Pending]} />
                      <LegendItem color={palette[TileState.Queued]} label="Building" count={counts[TileState.Queued]} />
                      <LegendItem color={palette[TileState.Built]} label="Built" count={counts[TileState.Built]} />
                      <LegendItem color={palette[TileState.Empty]} label="Empty" count={counts[TileState.Empty]} />
                      <LegendItem color={palette[TileState.Failed]} label="Failed" count={counts[TileState.Failed]} />
                      {counts[TileState.Existing] > 0 && <LegendItem color={palette[TileState.Existing]} label="Already built" count={counts[TileState.Existing]} />}
                    </>
                  ) : (
                    <span className="flex items-center gap-2 text-muted-foreground">
                      <span className="font-mono">0 s</span>
                      <span aria-hidden className="h-2.5 w-32 rounded-sm" style={{ backgroundImage: viridisGradient }} />
                      <span className="font-mono">{formatNumber(maxMs / 1000, 1)} s</span>
                      <span>(log scale)</span>
                    </span>
                  )}
                </div>
              </div>
              <TileGrid
                className="flex-1"
                label="Generation tile map"
                tilesX={tilesX}
                tilesZ={tilesZ}
                version={job.tileVersion}
                paint={paint}
                describe={describe}
                selection={running ? job.start?.range : opts.range}
                onSelectionChange={running ? undefined : (r) => set({ range: r })}
                picked={picked}
                onPick={(x, z) => setPicked([x, z])}
              />
            </>
          ) : opts.source === 'server' ? (
            <div className="flex flex-1 items-center justify-center overflow-y-auto p-8">
              <StartServerHelp />
            </div>
          ) : (
            <div className="flex flex-1 items-center justify-center p-8 text-center">
              <p className="max-w-sm text-sm text-muted-foreground">The map appears when the run starts and the capture file's world is known.</p>
            </div>
          )}
        </section>

        {/* Progress and log */}
        <aside aria-label="Progress" className="flex w-80 shrink-0 flex-col border-l">
          <div className="grid gap-3 border-b p-4">
            <div className="flex items-baseline justify-between">
              <span className="font-mono text-3xl">{p ? formatPercent(p.done, p.total) : '–'}</span>
              <span className="font-mono text-xs text-muted-foreground">
                {p ? `${formatInt(p.done)} / ${formatInt(p.total)}` : job.start ? `0 / ${formatInt(job.start.tiles)}` : 'not started'}
              </span>
            </div>
            <Progress value={p && p.total ? (100 * p.done) / p.total : 0} aria-label="Generation progress" />
            <div className="grid grid-cols-2 gap-x-4 gap-y-2">
              <Stat label="Time left" value={running && p ? formatDuration(p.eta) : '–'} />
              <Stat label="Elapsed" value={formatDuration(job.done?.seconds ?? p?.elapsed)} />
              <Stat label="Built" value={formatInt(job.built)} />
              <Stat label="Failed" value={<span className={cn(job.failed > 0 && 'text-destructive')}>{formatInt(job.failed)}</span>} />
              {expert && (
                <>
                  <Stat label="Speed" value={running && p ? `${p.rate.toFixed(1)} tiles/s` : '–'} />
                  <Stat label="Waiting to build" value={formatInt(job.queued)} />
                  <Stat label="Empty" value={formatInt(job.empty)} sub={job.empty ? 'nothing walkable' : undefined} />
                  <Stat label="Bad door part IDs" value={formatInt(job.strayTiles)} sub={job.strayTiles ? 'tiles, fixed automatically' : undefined} />
                </>
              )}
            </div>
          </div>

          <RunOutcome onResume={resume} onInspect={() => {
            void openNavmesh(job.output)
            onNavigate('inspect')
          }} />

          <Log entries={job.log} />
        </aside>
      </div>
    </>
  )
}

const stateNames: Record<TileState, string> = {
  [TileState.Outside]: 'not in this run',
  [TileState.Pending]: 'pending',
  [TileState.Queued]: 'fetched, building',
  [TileState.Built]: 'built',
  [TileState.Empty]: 'empty (no walkable surface)',
  [TileState.Failed]: 'failed',
  [TileState.Existing]: 'already in the navmesh, kept',
}

/** The tile arrays change in place; `version` tracks them. */
function tileStats(_version: number) {
  const counts = [0, 0, 0, 0, 0, 0, 0]
  for (const s of generation.states) counts[s]++
  let maxMs = 0
  for (const v of generation.buildMs) maxMs = Math.max(maxMs, v)
  return { counts, maxMs }
}

/** Map colours from the current theme's CSS variables. */
function paletteFromTheme(_theme: string | undefined): Record<number, Rgb> {
  return {
    [TileState.Outside]: cssVarRgb('--card'),
    [TileState.Pending]: cssVarRgb('--tile-pending'),
    [TileState.Queued]: cssVarRgb('--tile-queued'),
    [TileState.Built]: cssVarRgb('--tile-built'),
    [TileState.Empty]: cssVarRgb('--tile-empty'),
    [TileState.Failed]: cssVarRgb('--tile-failed'),
    [TileState.Existing]: cssVarRgb('--tile-existing'),
  }
}

function RunOutcome({ onResume, onInspect }: { onResume: () => void; onInspect: () => void }) {
  const job = useStore(generation.summary)
  const d = job.done
  if (job.status === 'finished' && d)
    return (
      <Alert className="m-3 w-auto">
        {d.failed ? <TriangleAlert /> : <Info />}
        <AlertTitle>{d.failed ? `Finished, ${formatInt(d.failed)} tiles failed` : 'Finished'}</AlertTitle>
        <AlertDescription>
          <p>
            {formatInt(d.built)} tiles built in {formatDuration(d.seconds)}.
            {job.islands?.seedFound && ` ${formatInt(job.islands.isolated)} polygons flagged unreachable.`}
            {d.failed ? ' Failed tiles have no navmesh; they are listed in the log. Regenerating just those tiles usually fixes them.' : ''}
          </p>
          <Button size="sm" variant="outline" className="mt-1" onClick={onInspect}>
            Inspect the navmesh
          </Button>
        </AlertDescription>
      </Alert>
    )
  if (job.status === 'stopped' || (job.status === 'failed' && d?.resumable))
    return (
      <Alert className="m-3 w-auto">
        <TriangleAlert />
        <AlertTitle>{job.status === 'stopped' ? 'Stopped' : 'Stopped early'}</AlertTitle>
        <AlertDescription>
          <p>{d?.saved ? 'The tiles built so far are saved. Resume to build the rest.' : (job.error ?? 'Nothing was saved.')}</p>
          {d?.saved && (
            <Button size="sm" className="mt-1" onClick={onResume}>
              <RotateCcw />
              Resume
            </Button>
          )}
        </AlertDescription>
      </Alert>
    )
  if (job.status === 'failed')
    return (
      <Alert variant="destructive" className="m-3 w-auto">
        <CircleAlert />
        <AlertTitle>Generation failed</AlertTitle>
        <AlertDescription>{job.error ?? 'See the log below.'}</AlertDescription>
      </Alert>
    )
  return null
}

function Log({ entries }: { entries: LogEntry[] }) {
  const ref = useRef<HTMLDivElement>(null)
  const stick = useRef(true)
  useEffect(() => {
    const el = ref.current
    if (el && stick.current) el.scrollTop = el.scrollHeight
  }, [entries])
  return (
    <div className="flex min-h-0 flex-1 flex-col">
      <h2 className="px-4 pt-3 pb-1 text-xs font-medium text-muted-foreground">Log</h2>
      <div
        ref={ref}
        className="min-h-0 flex-1 overflow-y-auto px-4 pb-3 font-mono text-xs leading-snug selectable"
        role="log"
        aria-label="Generation log"
        onScroll={(e) => {
          const el = e.currentTarget
          stick.current = el.scrollHeight - el.scrollTop - el.clientHeight < 24
        }}
      >
        {entries.length === 0 && <p className="font-sans text-muted-foreground">Messages from the engine appear here.</p>}
        {entries.map((e, i) => (
          <p key={i} className={cn('py-0.5 break-words', e.level === 'error' && 'text-destructive', e.level === 'warn' && 'text-tile-queued')}>
            <span className="text-muted-foreground">{e.time.toLocaleTimeString('en-GB')}</span> {e.level !== 'info' && <span className="sr-only">{e.level}: </span>}
            {e.message}
          </p>
        ))}
      </div>
    </div>
  )
}

function LegendItem({ color, label, count }: { color: Rgb; label: string; count: number }) {
  return (
    <span className="flex items-center gap-1.5">
      <Swatch color={`rgb(${color.join(' ')})`} />
      <span>{label}</span>
      <span className="font-mono text-muted-foreground">{formatInt(count)}</span>
    </span>
  )
}

/** A titled card of settings inside Advanced, one setting per row, like the cards in Settings. */
function AdvancedGroup({ title, children }: { title: string; children: ReactNode }) {
  return (
    <section className="grid gap-2" aria-label={title}>
      <h3 className="text-xs font-semibold tracking-wider text-muted-foreground uppercase">{title}</h3>
      <div className="divide-y rounded-lg border bg-card">
        {Children.toArray(children).map((child, i) => (
          <div key={i} className="p-3">
            {child}
          </div>
        ))}
      </div>
    </section>
  )
}

function Section({ title, children }: { title: string; children: ReactNode }) {
  return (
    <section className="grid gap-4" aria-label={title}>
      <h2 className="text-sm font-semibold">{title}</h2>
      {children}
    </section>
  )
}

/** Windows paths compare case-insensitively, with either slash. */
function pathKey(p: string) {
  return p.trim().replace(/\//g, '\\').toLowerCase()
}

function samePath(a: string, b: string) {
  return !!a.trim() && pathKey(a) === pathKey(b)
}

function RangeInputs({ range, onChange, invalid }: { range: TileRange; onChange: (r: TileRange) => void; invalid: boolean }) {
  const labels = ['x from', 'z from', 'x to', 'z to']
  return (
    <div className="grid grid-cols-4 gap-1.5">
      {range.map((v, i) => (
        <label key={i} className="grid gap-1 text-xs text-muted-foreground">
          {labels[i]}
          <Input
            value={Number.isFinite(v) ? String(v) : ''}
            inputMode="numeric"
            className="px-1.5 font-mono"
            aria-invalid={invalid}
            onChange={(e) => {
              const next = [...range] as TileRange
              next[i] = e.target.value === '' ? NaN : Number(e.target.value)
              onChange(next)
            }}
          />
        </label>
      ))}
    </div>
  )
}
