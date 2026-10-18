import { FolderOpen, LoaderCircle, X } from 'lucide-react'
import { useCallback, useEffect, useMemo, useState } from 'react'

import { Facts, PageHeader, Stat, Swatch } from '@/components/common'
import { TileGrid } from '@/components/tile-grid'
import { Alert, AlertDescription, AlertTitle } from '@/components/ui/alert'
import { Button } from '@flare/ui/ui/button'
import { Input } from '@flare/ui/ui/input'
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table'
import { ToggleGroup, ToggleGroupItem } from '@flare/ui/ui/toggle-group'
import { cssVarRgb, viridisAt, viridisGradient } from '@/lib/colors'
import { EngineError, hasRealEngine, type EventOf } from '@/lib/engine'
import { renderArea, tileDetail } from '@/lib/inspector'
import { fileName, formatBytes, formatInt, formatNumber, formatPercent } from '@/lib/format'
import { areaInfo, navmeshFilters } from '@/lib/navmesh'
import { inspect, openNavmesh } from '@/state/inspect'
import { useStore } from '@/state/store'
import { useResolvedTheme } from '@/state/theme'

type Info = EventOf<'info'>
type Detail = EventOf<'tileDetail'>

// Columns of info.tileList
const metrics = [
  { key: 'bytes', label: 'Size', column: 2, format: formatBytes },
  { key: 'polys', label: 'Polygons', column: 3, format: formatInt },
  { key: 'links', label: 'Links', column: 4, format: formatInt },
  { key: 'obstacles', label: 'Obstacles', column: 5, format: formatInt },
  { key: 'doors', label: 'Doors', column: 6, format: formatInt },
] as const
type MetricKey = (typeof metrics)[number]['key']

export function InspectPage() {
  const { path, info, loading, error } = useStore(inspect)
  const [draft, setDraft] = useState(path)
  const [metricKey, setMetricKey] = useState<MetricKey>('polys')
  // The tile picked in the navmesh it was picked in, so opening another one closes its details.
  const [pick, setPick] = useState<{ info: Info; tile: [number, number] } | null>(null)
  const picked = info && pick?.info === info ? pick.tile : null
  const setPicked = (tile: [number, number] | null) => setPick(tile && info ? { info, tile } : null)

  const open = (p: string) => {
    setDraft(p)
    void openNavmesh(p)
  }

  const browse = async () => {
    const dialog = await import('@tauri-apps/plugin-dialog')
    const p = await dialog.open({ multiple: false, filters: navmeshFilters })
    if (typeof p === 'string') open(p)
  }

  const metric = metrics.find((m) => m.key === metricKey)!
  const grid = useMemo(() => {
    if (!info?.tileList) return null
    const values = new Float32Array(info.tilesX * info.tilesZ).fill(NaN)
    let max = 0
    for (const t of info.tileList) {
      const v = t[metric.column]
      values[t[1] * info.tilesX + t[0]] = v
      max = Math.max(max, v)
    }
    return { values, max }
  }, [info, metric.column])

  // Tiles the navmesh doesn't have take the background colour, so the map repaints when the theme changes.
  const theme = useResolvedTheme()
  const none = useMemo(() => backgroundRgb(theme), [theme])
  const paint = useCallback(
    (rgba: Uint8ClampedArray, w: number, h: number) => {
      if (!grid) return
      const logMax = Math.log1p(grid.max)
      for (let z = 0; z < h; ++z)
        for (let x = 0; x < w; ++x) {
          const v = grid.values[z * w + x]
          const c = Number.isNaN(v) ? none : viridisAt(logMax > 0 ? Math.log1p(v) / logMax : 0)
          const p = ((h - 1 - z) * w + x) * 4
          rgba[p] = c[0]
          rgba[p + 1] = c[1]
          rgba[p + 2] = c[2]
          rgba[p + 3] = 255
        }
    },
    [grid, none],
  )

  const describe = useCallback(
    (x: number, z: number) => {
      const v = grid?.values[z * (info?.tilesX ?? 0) + x]
      return v === undefined || Number.isNaN(v) ? 'no tile' : `${metric.label.toLowerCase()}: ${metric.format(v)}`
    },
    [grid, info, metric],
  )

  return (
    <>
      <PageHeader
        title="Inspect"
        description={info ? info.path : 'Open a .nm file to see what is inside.'}
        actions={
          <form
            className="flex items-center gap-1.5"
            onSubmit={(e) => {
              e.preventDefault()
              open(draft)
            }}
          >
            <Input
              aria-label="Navmesh file"
              value={draft}
              onChange={(e) => setDraft(e.target.value)}
              placeholder="C:\…\world.nm"
              className="w-96 font-mono"
              spellCheck={false}
            />
            {hasRealEngine && (
              <Button type="button" variant="outline" onClick={browse}>
                <FolderOpen />
                Browse
              </Button>
            )}
            <Button type="submit" disabled={loading || !draft.trim()}>
              {loading && <LoaderCircle className="animate-spin" />}
              {loading ? 'Reading…' : 'Open'}
            </Button>
          </form>
        }
      />

      {error && (
        <Alert variant="destructive" className="m-4 w-auto">
          <AlertTitle>Couldn't read the navmesh</AlertTitle>
          <AlertDescription>
            <p className="font-mono selectable">{error}</p>
            <p>Check the path, and that the file is a DayZ .nm navmesh.</p>
          </AlertDescription>
        </Alert>
      )}

      {!info && !error && (
        <div className="flex flex-1 items-center justify-center p-8 text-center text-sm text-muted-foreground">
          {loading ? (
            <span className="flex items-center gap-2">
              <LoaderCircle className="size-4 animate-spin" /> Reading {fileName(path)}… large maps take a few seconds.
            </span>
          ) : (
            'Open a navmesh to see its tiles, areas, links and doors.'
          )}
        </div>
      )}

      {info && grid && (
        <div className="flex min-h-0 flex-1">
          <Summary info={info} />
          <section aria-label="Tile map" className="flex min-w-0 flex-1 flex-col overflow-hidden">
            <div className="flex min-h-10 shrink-0 flex-wrap items-center gap-x-3 gap-y-1.5 border-b px-3 py-1.5">
              <ToggleGroup type="single" value={metricKey} onValueChange={(v) => v && setMetricKey(v as MetricKey)} aria-label="Colour tiles by">
                {metrics.map((m) => (
                  <ToggleGroupItem key={m.key} value={m.key}>
                    {m.label}
                  </ToggleGroupItem>
                ))}
              </ToggleGroup>
              <span className="flex items-center gap-2 text-xs text-muted-foreground">
                <span className="font-mono">0</span>
                <span aria-hidden className="h-2.5 w-32 rounded-sm" style={{ backgroundImage: viridisGradient }} />
                <span className="font-mono">{metric.format(grid.max)}</span>
                <span>(log scale)</span>
              </span>
            </div>
            <TileGrid
              className="flex-1"
              label={`Tile map coloured by ${metric.label.toLowerCase()}`}
              tilesX={info.tilesX}
              tilesZ={info.tilesZ}
              version={0}
              paint={paint}
              describe={describe}
              picked={picked}
              onPick={(x, z) => setPicked([x, z])}
            />
          </section>
          {picked && <TileDetail key={picked.join()} path={info.path} x={picked[0]} z={picked[1]} onClose={() => setPicked(null)} />}
        </div>
      )}
    </>
  )
}

function backgroundRgb(_theme: string) {
  return cssVarRgb('--card')
}

function Summary({ info }: { info: Info }) {
  const areas = Object.entries(info.areas)
    .map(([id, n]) => ({ id: Number(id), n }))
    .sort((a, b) => b.n - a.n)
  const totalPolys = areas.reduce((s, a) => s + a.n, 0)
  const isolated = Object.entries(info.flags).reduce((s, [f, n]) => (Number(f) & 0x8000 ? s + n : s), 0)
  return (
    <aside aria-label="Summary" className="relative w-80 shrink-0 overflow-y-auto border-r p-4">
      <div className="grid grid-cols-2 gap-x-4 gap-y-3">
        <Stat label="Tiles" value={formatInt(info.tiles)} sub={`of ${info.tilesX} × ${info.tilesZ}`} />
        <Stat label="Polygons" value={formatInt(info.polys)} />
        <Stat label="Jump links" value={formatInt(info.links)} />
        <Stat label="Door parts" value={formatInt(info.doorParts)} />
        <Stat label="Tree obstacles" value={formatInt(info.obstacles)} />
        <Stat label="Unreachable" value={formatInt(isolated)} sub={isolated ? formatPercent(isolated, totalPolys) + ' of polygons' : 'none flagged'} />
      </div>
      <h2 className="mt-5 mb-2 text-xs font-medium text-muted-foreground">Areas</h2>
      <ul className="grid gap-1.5 text-sm">
        {areas.map(({ id, n }) => {
          const a = areaInfo(id)
          return (
            <li key={id} className="grid grid-cols-[auto_1fr_auto] items-center gap-2">
              <Swatch color={a.color} />
              <span className="truncate">{a.name}</span>
              <span className="font-mono text-xs text-muted-foreground">{formatPercent(n, totalPolys)}</span>
            </li>
          )
        })}
      </ul>
      <h2 className="mt-5 mb-2 text-xs font-medium text-muted-foreground">File</h2>
      <Facts
        rows={[
          ['Tile size', `${info.tileWidth} × ${info.tileHeight} m`],
          ...info.cellSizes.map(([cs, n]) => [`${formatNumber(cs, 2)} m cells`, `${formatInt(n)} tiles`] as [string, string]),
          ...(info.agent
            ? ([
                ['Agent height', `${formatNumber(info.agent.height)} m`],
                ['Agent radius', `${formatNumber(info.agent.radius)} m`],
              ] as [string, string][])
            : []),
        ]}
      />
    </aside>
  )
}

function TileDetail({ path, x, z, onClose }: { path: string; x: number; z: number; onClose: () => void }) {
  const [detail, setDetail] = useState<Detail | null>(null)
  const [image, setImage] = useState<string | null>(null)
  const [state, setState] = useState<'loading' | 'rendering' | 'ready' | 'missing' | 'error'>('loading')
  const [error, setError] = useState('')

  // Remounted per tile (keyed), so state starts fresh.
  useEffect(() => {
    let cancelled = false
    let url: string | null = null
    void (async () => {
      try {
        const d = await tileDetail(path, x, z)
        if (cancelled) return
        setDetail(d)
        setState('rendering')
        const rendered = await renderArea(path, [d.bmin[0], d.bmin[2]], [d.bmax[0], d.bmax[2]], 8)
        if (cancelled) {
          if (rendered) URL.revokeObjectURL(rendered)
          return
        }
        url = rendered
        setImage(url)
        setState('ready')
      } catch (e) {
        if (cancelled) return
        const message = e instanceof EngineError ? e.message : String(e)
        setState(message.startsWith('no tile') ? 'missing' : 'error')
        setError(message)
      }
    })()
    return () => {
      cancelled = true
      if (url) URL.revokeObjectURL(url)
    }
  }, [path, x, z])

  return (
    <aside aria-label={`Tile ${x}, ${z}`} className="relative flex w-96 shrink-0 flex-col overflow-y-auto border-l">
      <div className="flex h-10 shrink-0 items-center justify-between border-b px-3">
        <h2 className="text-sm font-medium">
          Tile <span className="font-mono">{x}, {z}</span>
        </h2>
        <Button variant="ghost" size="icon-sm" onClick={onClose} aria-label="Close tile details">
          <X />
        </Button>
      </div>
      <div className="grid gap-4 p-3">
        <div className="relative aspect-square w-full overflow-hidden rounded-sm border bg-card">
          {image ? (
            <img src={image} alt={`Top-down render of tile ${x}, ${z}, coloured by area`} className="size-full [image-rendering:pixelated]" />
          ) : (
            <div className="flex size-full items-center justify-center p-4 text-center text-xs text-muted-foreground">
              {state === 'loading' || state === 'rendering' ? (
                <span className="flex items-center gap-2">
                  <LoaderCircle className="size-4 animate-spin" />
                  {state === 'loading' ? 'Reading tile…' : 'Rendering…'}
                </span>
              ) : state === 'missing' ? (
                'This tile is not in the navmesh (no walkable surface, or not generated).'
              ) : state === 'error' ? (
                <span className="text-destructive">{error}</span>
              ) : (
                'Previews need the desktop app.'
              )}
            </div>
          )}
        </div>

        {detail && (
          <>
            <Facts
              rows={[
                ['World', `${formatInt(detail.bmin[0])}–${formatInt(detail.bmax[0])} m E, ${formatInt(detail.bmin[2])}–${formatInt(detail.bmax[2])} m N`],
                ['Height', `${formatNumber(detail.bmin[1], 1)} to ${formatNumber(detail.bmax[1], 1)} m`],
                ['Cell size', `${formatNumber(detail.cellSize, 2)} m`],
                ['Size', formatBytes(detail.bytes)],
                ['Polygons', formatInt(detail.polys)],
                ['Door parts', formatInt(detail.doors.length)],
                ['Tree obstacles', formatInt(detail.obstacles.length)],
              ]}
            />
            <ul className="grid gap-1 text-sm">
              {Object.entries(detail.areas).map(([id, n]) => {
                const a = areaInfo(Number(id))
                return (
                  <li key={id} className="grid grid-cols-[auto_1fr_auto] items-center gap-2">
                    <Swatch color={a.color} />
                    <span>{a.name}</span>
                    <span className="font-mono text-xs text-muted-foreground">{formatInt(n)}</span>
                  </li>
                )
              })}
            </ul>
            <div>
              <h3 className="mb-1 text-xs font-medium text-muted-foreground">Jump links ({detail.links.length})</h3>
              {detail.links.length > 0 ? (
                <Table>
                  <TableHeader>
                    <TableRow>
                      <TableHead>Class</TableHead>
                      <TableHead className="text-right">Length</TableHead>
                      <TableHead className="text-right">Rise</TableHead>
                      <TableHead>Direction</TableHead>
                    </TableRow>
                  </TableHeader>
                  <TableBody className="font-mono text-xs">
                    {detail.links.slice(0, 200).map((l, i) => (
                      <TableRow key={i}>
                        <TableCell>{l.class}</TableCell>
                        <TableCell className="text-right">{formatNumber(Math.hypot(l.to[0] - l.from[0], l.to[2] - l.from[2]), 2)} m</TableCell>
                        <TableCell className="text-right">{formatNumber(l.to[1] - l.from[1], 2)} m</TableCell>
                        <TableCell className="font-sans">{l.bidirectional ? 'Both ways' : 'One way'}</TableCell>
                      </TableRow>
                    ))}
                  </TableBody>
                </Table>
              ) : (
                <p className="text-xs text-muted-foreground">None in this tile.</p>
              )}
            </div>
          </>
        )}
      </div>
    </aside>
  )
}
