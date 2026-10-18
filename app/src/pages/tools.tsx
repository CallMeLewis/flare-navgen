import { LoaderCircle } from 'lucide-react'
import type { ReactNode } from 'react'

import { FileField, PageHeader, Stat, SwitchField } from '@/components/common'
import { Alert, AlertDescription, AlertTitle } from '@/components/ui/alert'
import { Button } from '@flare/ui/ui/button'
import { Card, CardContent, CardDescription, CardFooter, CardHeader, CardTitle } from '@/components/ui/card'
import { Input } from '@flare/ui/ui/input'
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table'
import { formatInt, formatNumber, formatPercent } from '@/lib/format'
import { areaInfo, navmeshFilters } from '@/lib/navmesh'
import { Store, useStore } from '@/state/store'
import { runTool, toolRuns, toolTitles, type ToolName, type ToolRun } from '@/state/tools'

export function ToolsPage() {
  return (
    <>
      <PageHeader title="Tools" description="Fix, check and compare navmesh files." />
      <div className="relative min-h-0 flex-1 overflow-auto p-4">
        <div className="mx-auto grid max-w-6xl gap-6">
          <ToolGroup title="Fix and update">
            <MergeTool />
            <IslandsTool />
          </ToolGroup>
          <ToolGroup title="Developer checks" description="Compare navmeshes in depth, for example against Bohemia's generator.">
            <VerifyTool />
            <DiffTool />
          </ToolGroup>
        </div>
      </div>
    </>
  )
}

function ToolGroup({ title, description, children }: { title: string; description?: string; children: ReactNode }) {
  return (
    <section className="grid gap-3" aria-label={title}>
      <div>
        <h2 className="text-base font-semibold">{title}</h2>
        {description && <p className="text-sm text-muted-foreground">{description}</p>}
      </div>
      <div className="grid gap-4 xl:grid-cols-2">{children}</div>
    </section>
  )
}

/** A tool's run (state/tools.ts), which carries on after leaving the page. */
function useTool<K extends ToolName>(tool: K) {
  const current = useStore(toolRuns)[tool] as ToolRun<K> | undefined
  return {
    busy: current?.busy ?? false,
    result: current?.result ?? null,
    error: current?.error ?? null,
    run: (args: string[]) => void runTool(tool, args),
  }
}

/** A tool's inputs, kept for the session like its run, so they're still there after leaving the page. */
function useForm<T extends object>(store: Store<T>) {
  const form = useStore(store)
  return [form, (patch: Partial<T>) => store.set((f) => ({ ...f, ...patch }))] as const
}

const verifyForm = new Store({ reference: '', ours: '', ignoreOrder: false })
const diffForm = new Store({ a: '', b: '' })
const mergeForm = new Store({ base: '', patch: '', out: '', range: ['', '', '', ''] })
const islandsForm = new Store({ input: '', out: '', seed: ['', '', ''], allHeights: false })

function ToolCard({
  title,
  description,
  children,
  action,
  busyLabel,
  disabled,
  busy,
  onRun,
  error,
  result,
}: {
  title: string
  description: string
  children: ReactNode
  action: string
  busyLabel: string
  disabled: boolean
  busy: boolean
  onRun: () => void
  error: string | null
  result: ReactNode
}) {
  return (
    <Card>
      <CardHeader>
        <CardTitle>{title}</CardTitle>
        <CardDescription>{description}</CardDescription>
      </CardHeader>
      <CardContent className="grid flex-1 content-start gap-3">
        <fieldset disabled={busy} className="grid gap-3">
          {children}
        </fieldset>
        {error && (
          <Alert variant="destructive">
            <AlertTitle>That didn't work</AlertTitle>
            <AlertDescription className="font-mono selectable">{error}</AlertDescription>
          </Alert>
        )}
        {result}
      </CardContent>
      <CardFooter className="justify-end">
        <Button onClick={onRun} disabled={disabled || busy}>
          {busy && <LoaderCircle className="animate-spin" />}
          {busy ? busyLabel : action}
        </Button>
      </CardFooter>
    </Card>
  )
}

function VerifyTool() {
  const [{ reference, ours, ignoreOrder }, set] = useForm(verifyForm)
  const t = useTool('verify')
  const r = t.result
  return (
    <ToolCard
      title={toolTitles.verify}
      description="Byte-compares the tiles both files have, ignoring what legitimately varies between runs (links to neighbours, door states)."
      action="Verify"
      busyLabel="Verifying…"
      disabled={!reference.trim() || !ours.trim()}
      busy={t.busy}
      onRun={() => t.run([reference, ours, ...(ignoreOrder ? ['--ignore-order'] : [])])}
      error={t.error}
      result={
        r && (
          <div className="grid gap-2 rounded-md border p-3">
            <div className="grid grid-cols-3 gap-3">
              <Stat label="Identical" value={formatPercent(r.identical, r.compared, 2)} />
              <Stat label="Tiles compared" value={formatInt(r.compared)} />
              <Stat label="Different" value={formatInt(r.different.length)} />
            </div>
            {r.ignoredIsolated && <p className="text-xs text-muted-foreground">The reference has no unreachable-area flags (a partial save), so those flags were ignored.</p>}
            {r.different.length > 0 && (
              <details className="text-xs">
                <summary className="cursor-pointer text-muted-foreground">Differing tiles (x, z)</summary>
                <p className="mt-1 max-h-32 overflow-y-auto font-mono break-words selectable">
                  {r.different.slice(0, 2000).map(([x, z]) => `${x},${z}`).join('  ')}
                  {r.different.length > 2000 && ` … and ${formatInt(r.different.length - 2000)} more`}
                </p>
              </details>
            )}
          </div>
        )
      }
    >
      <FileField label="Navmesh from Bohemia's generator" mode="open" filters={navmeshFilters} value={reference} onChange={(reference) => set({ reference })} />
      <FileField label="Navmesh to check" mode="open" filters={navmeshFilters} value={ours} onChange={(ours) => set({ ours })} />
      <SwitchField
        label="Ignore object order"
        hint="For files made in different DayZDiag sessions, which can list trees and doors in another order."
        checked={ignoreOrder}
        onCheckedChange={(ignoreOrder) => set({ ignoreOrder })}
      />
    </ToolCard>
  )
}

function DiffTool() {
  const [{ a, b }, set] = useForm(diffForm)
  const t = useTool('diff')
  const r = t.result
  return (
    <ToolCard
      title={toolTitles.diff}
      description="Totals and walkable area per area type, and the tiles that differ most."
      action="Compare"
      busyLabel="Comparing…"
      disabled={!a.trim() || !b.trim()}
      busy={t.busy}
      onRun={() => t.run([a, b, '15'])}
      error={t.error}
      result={
        r && (
          <div className="grid gap-3 rounded-md border p-3">
            <div className="grid grid-cols-3 gap-3">
              <Stat label="Tiles in both" value={formatInt(r.compared)} />
              <Stat label="Only in A / B" value={`${formatInt(r.onlyInA)} / ${formatInt(r.onlyInB)}`} />
              <Stat label="Area mismatch" value={`${formatNumber(r.mismatchPercent, 3)}%`} />
            </div>
            <Table>
              <TableHeader>
                <TableRow>
                  <TableHead />
                  <TableHead className="text-right">A</TableHead>
                  <TableHead className="text-right">B</TableHead>
                </TableRow>
              </TableHeader>
              <TableBody className="font-mono text-xs">
                {(['polys', 'links', 'obstacles', 'doorParts'] as const).map((k) => (
                  <TableRow key={k}>
                    <TableCell className="font-sans">{{ polys: 'Polygons', links: 'Jump links', obstacles: 'Obstacles', doorParts: 'Door parts' }[k]}</TableCell>
                    <TableCell className="text-right">{formatInt(r.a[k])}</TableCell>
                    <TableCell className="text-right">{formatInt(r.b[k])}</TableCell>
                  </TableRow>
                ))}
                {Object.entries(r.areaM2).map(([id, [va, vb]]) => (
                  <TableRow key={id}>
                    <TableCell className="font-sans">{areaInfo(Number(id)).name} (km²)</TableCell>
                    <TableCell className="text-right">{formatNumber(va / 1e6, 4)}</TableCell>
                    <TableCell className="text-right">{formatNumber(vb / 1e6, 4)}</TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </div>
        )
      }
    >
      <FileField label="Navmesh A" mode="open" filters={navmeshFilters} value={a} onChange={(a) => set({ a })} />
      <FileField label="Navmesh B" mode="open" filters={navmeshFilters} value={b} onChange={(b) => set({ b })} />
    </ToolCard>
  )
}

function MergeTool() {
  const [{ base, patch, out, range }, set] = useForm(mergeForm)
  const t = useTool('merge')
  const hasRange = range.every((v) => v.trim() !== '')
  const partialRange = !hasRange && range.some((v) => v.trim() !== '')
  return (
    <ToolCard
      title={toolTitles.merge}
      description="Replaces tiles in a navmesh with the same tiles from another, and reconnects the seams."
      action="Merge"
      busyLabel="Merging…"
      disabled={!base.trim() || !patch.trim() || !out.trim() || partialRange}
      busy={t.busy}
      onRun={() => t.run([base, patch, out, ...(hasRange ? range : [])])}
      error={t.error}
      result={t.result && <p className="text-sm">Replaced {formatInt(t.result.replaced)} tiles and saved {t.result.output}.</p>}
    >
      <FileField label="Navmesh to update" mode="open" filters={navmeshFilters} value={base} onChange={(base) => set({ base })} />
      <FileField label="Take tiles from" mode="open" filters={navmeshFilters} value={patch} onChange={(patch) => set({ patch })} />
      <NumberRow
        label="Only tiles in range (optional)"
        labels={['x from', 'z from', 'x to', 'z to']}
        values={range}
        onChange={(range) => set({ range })}
        error={partialRange ? 'Fill in all four numbers, or none to take every tile.' : undefined}
      />
      <FileField label="Save as" mode="save" filters={navmeshFilters} value={out} onChange={(out) => set({ out })} />
    </ToolCard>
  )
}

function IslandsTool() {
  const [{ input, out, seed, allHeights }, set] = useForm(islandsForm)
  const t = useTool('islands')
  return (
    <ToolCard
      title={toolTitles.islands}
      description="Re-runs the island filter from a seed point: polygons that can't be reached from it are flagged so AI won't path there."
      action="Run filter"
      busyLabel="Running filter…"
      disabled={!input.trim() || !out.trim() || seed.some((v) => v.trim() === '' || Number.isNaN(Number(v)))}
      busy={t.busy}
      onRun={() => t.run([input, out, ...seed, ...(allHeights ? ['--all-heights'] : [])])}
      error={t.error}
      result={
        t.result && (
          <p className="text-sm">
            {formatInt(t.result.reachable)} polygons reachable, {formatInt(t.result.isolated)} flagged unreachable. Saved {t.result.output}.
          </p>
        )
      }
    >
      <FileField label="Navmesh" mode="open" filters={navmeshFilters} value={input} onChange={(input) => set({ input })} />
      <NumberRow label="Seed point (m)" labels={['x (east)', 'y (height)', 'z (north)']} values={seed} onChange={(seed) => set({ seed })} />
      <SwitchField
        label="Seed may be on any floor"
        hint="Searches every height for the seed's polygon. Turn on if the filter can't find a polygon near the seed."
        checked={allHeights}
        onCheckedChange={(allHeights) => set({ allHeights })}
      />
      <FileField label="Save as" mode="save" filters={navmeshFilters} value={out} onChange={(out) => set({ out })} />
    </ToolCard>
  )
}

function NumberRow({
  label,
  labels,
  values,
  onChange,
  error,
}: {
  label: string
  labels: string[]
  values: string[]
  onChange: (v: string[]) => void
  error?: string
}) {
  return (
    <div className="grid gap-1.5" role="group" aria-label={label}>
      <span className="text-sm leading-none font-medium">{label}</span>
      <div className="grid gap-1.5" style={{ gridTemplateColumns: `repeat(${labels.length}, minmax(0, 1fr))` }}>
        {labels.map((l, i) => (
          <label key={l} className="grid gap-1 text-xs text-muted-foreground">
            {l}
            <Input
              value={values[i]}
              inputMode="decimal"
              className="font-mono"
              aria-invalid={!!error}
              onChange={(e) => onChange(values.map((v, j) => (j === i ? e.target.value : v)))}
            />
          </label>
        ))}
      </div>
      {error && <p className="text-xs text-destructive">{error}</p>}
    </div>
  )
}
