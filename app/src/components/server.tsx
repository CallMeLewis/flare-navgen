// The data server: connecting to it, help for starting one, and the world it serves (expert mode).
import { Check, CircleAlert, CircleCheck, Copy, LoaderCircle, PlugZap } from 'lucide-react'
import { useId, useState, type ReactNode } from 'react'

import { Facts } from '@/components/common'
import { Button } from '@flare/ui/ui/button'
import { Input } from '@flare/ui/ui/input'
import { Label } from '@flare/ui/ui/label'
import { Sheet, SheetContent, SheetDescription, SheetHeader, SheetTitle } from '@/components/ui/sheet'
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table'
import type { WorldParams } from '@/lib/engine'
import { formatInt, formatNumber } from '@/lib/format'
import { areaInfo, describeFlags } from '@/lib/navmesh'
import { connect, connection, settings } from '@/state/connection'
import { useStore } from '@/state/store'

const serverCommand = 'DayZDiag_x64.exe -server -mod=<your mods> -startNavmeshDataServer -config=serverDZ_navmesh.cfg'

/** Address, Connect and the connection status, for the top of the Generate options. */
export function ServerConnection({ disabled }: { disabled?: boolean }) {
  const conn = useStore(connection)
  const { server, expert } = useStore(settings)
  const [details, setDetails] = useState(false)
  const id = useId()
  const busy = conn.status === 'connecting'

  return (
    <div className="grid gap-1.5">
      <Label htmlFor={id}>Server address</Label>
      <form
        className="flex gap-1.5"
        onSubmit={(e) => {
          e.preventDefault()
          void connect(server.trim())
        }}
      >
        <Input
          id={id}
          value={server}
          onChange={(e) => settings.set((s) => ({ ...s, server: e.target.value }))}
          className="font-mono"
          spellCheck={false}
          disabled={disabled}
          aria-describedby={`${id}-status`}
        />
        <Button type="submit" variant={conn.status === 'connected' ? 'outline' : 'default'} disabled={disabled || busy || !server.trim()}>
          {busy ? <LoaderCircle className="animate-spin" /> : <PlugZap />}
          {busy ? 'Connecting…' : conn.status === 'connected' ? 'Reconnect' : 'Connect'}
        </Button>
      </form>
      <div id={`${id}-status`} role="status" className="text-xs">
        {conn.status === 'connected' ? (
          <p className="flex flex-wrap items-center gap-x-1.5 text-muted-foreground">
            <CircleCheck className="size-3.5 text-primary" aria-hidden />
            Connected · {conn.world.tilesX} × {conn.world.tilesZ} tiles, {formatNumber(conn.world.worldSizeX / 1000, 1)} km
            {expert && (
              <button type="button" className="underline underline-offset-2 hover:text-foreground" onClick={() => setDetails(true)}>
                Server details
              </button>
            )}
          </p>
        ) : conn.status === 'error' ? (
          <div className="grid gap-0.5 text-destructive">
            <p className="flex items-center gap-1.5">
              <CircleAlert className="size-3.5 shrink-0" aria-hidden />
              Can't reach DayZDiag at {conn.host}.
            </p>
            <p className="text-muted-foreground">Check it's running with -startNavmeshDataServer, has finished loading the map, and isn't serving another tool.</p>
            {expert && <p className="font-mono break-all text-muted-foreground selectable">{conn.message}</p>}
          </div>
        ) : (
          <p className="text-muted-foreground">Several addresses, separated by commas, fetch in parallel.</p>
        )}
      </div>
      {conn.status === 'connected' && <ServerDetails world={conn.world} open={details} onOpenChange={setDetails} />}
    </div>
  )
}

/** What to do before connecting: start DayZDiag as a data server. */
export function StartServerHelp() {
  const [copied, setCopied] = useState(false)
  return (
    <div className="grid w-full max-w-xl gap-3 text-sm">
      <h2 className="text-base font-semibold">Start a data server, then connect</h2>
      <ol className="grid list-decimal gap-1 pl-5 text-muted-foreground">
        <li>Create an empty mission for your terrain and a server config that points at it, as described on the BI wiki page "DayZ: Generating navigation mesh".</li>
        <li>Start DayZDiag from the DayZ folder with your terrain's mods:</li>
      </ol>
      <div className="flex items-start gap-2 rounded-md border bg-muted/50 p-2">
        <code className="flex-1 font-mono text-xs break-all selectable">{serverCommand}</code>
        <Button
          variant="ghost"
          size="icon-sm"
          aria-label="Copy command"
          onClick={async () => {
            await navigator.clipboard.writeText(serverCommand)
            setCopied(true)
            setTimeout(() => setCopied(false), 1500)
          }}
        >
          {copied ? <Check /> : <Copy />}
        </Button>
      </div>
      <p className="text-muted-foreground">Wait for the map to finish loading, then press Connect.</p>
    </div>
  )
}

/** Everything the server sent about its world, in a side panel. */
function ServerDetails({ world: w, open, onOpenChange }: { world: WorldParams; open: boolean; onOpenChange: (open: boolean) => void }) {
  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent
        className="w-full overflow-y-auto outline-none data-[side=right]:sm:max-w-2xl"
        // Focus the panel, not its first info button (which would pop a tooltip).
        onOpenAutoFocus={(e) => {
          e.preventDefault()
          ;(e.currentTarget as HTMLElement).focus()
        }}
      >
        <SheetHeader>
          <SheetTitle>Server details</SheetTitle>
          <SheetDescription>The world settings DayZDiag sent when you connected.</SheetDescription>
        </SheetHeader>
        <div className="grid gap-6 px-4 pb-6">
          <div className="grid gap-x-8 gap-y-6 sm:grid-cols-2">
            <Group title="World">
              <Facts
                rows={[
                  ['Size', `${formatInt(w.worldSizeX)} × ${formatInt(w.worldSizeZ)} m`],
                  ['Tiles', `${w.tilesX} × ${w.tilesZ}`],
                  ['Tile size', `${w.tileWidth} × ${w.tileHeight} m`],
                  ['Total', formatInt(w.tilesX * w.tilesZ)],
                ]}
              />
            </Group>
            <Group title="Water">
              <Facts
                rows={[
                  ['Sea level now', `${formatNumber(w.seaLevel)} m`, "Follows the server's tide. Pin it under Advanced on Generate to get the same result on every run."],
                  ['Deep water from', `${formatNumber(w.deepWaterDepth)} m`],
                ]}
              />
            </Group>
            <Group title="Generation">
              <Facts
                rows={[
                  ['Contour error', `${formatNumber(w.contourMaxError, 3)} m`, 'How far polygon edges may stray from the outline of the walkable surface.'],
                  ['Tree obstacles', w.obstacles ? 'On' : 'Off', 'Trees become round obstacles that AI steers around.'],
                  ['Block size', `${w.blockSize} tiles`, 'The server hands out tiles in blocks of this many.'],
                  ['Island filter', w.filterIsolatedIslands ? 'On' : 'Off', "Flags polygons that can't be reached from the seed point, so AI won't path there."],
                  ['Seed point', w.seed.map((v) => v.toFixed(0)).join(', '), "Where the island filter starts (x, y, z in metres). Polygons it can't reach from here are flagged unreachable."],
                ]}
              />
            </Group>
            <Group title="Agent">
              <Facts
                rows={[
                  ['Standing height', `${formatNumber(w.agent.standHeight)} m`],
                  ['Crouching height', `${formatNumber(w.agent.crouchHeight)} m`],
                  ['Prone height', `${formatNumber(w.agent.proneHeight)} m`],
                  ['Diameter', `${formatNumber(w.agent.diameter)} m`],
                  ['Max step', `${formatNumber(w.agent.maxStepHeight)} m`],
                  ['Max slope', `${formatNumber(w.agent.maxSlope, 0)}°`],
                ]}
              />
            </Group>
          </div>

          <Group title="Cell classes" description="The server gives each tile one of these. Smaller cells follow objects and buildings more closely.">
            <Table>
              <TableHeader>
                <TableRow>
                  <TableHead>#</TableHead>
                  <TableHead className="text-right">Cells per tile</TableHead>
                  <TableHead className="text-right">Cell size</TableHead>
                </TableRow>
              </TableHeader>
              <TableBody className="font-mono">
                {w.cellClasses.map((c, i) => (
                  <TableRow key={i}>
                    <TableCell>{i}</TableCell>
                    <TableCell className="text-right">{c.cells}</TableCell>
                    <TableCell className="text-right">{formatNumber(c.cellSize, 3)} m</TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </Group>

          <Group title="Link classes" description="Jump and climb links between walkable surfaces.">
            <Table>
              <TableHeader>
                <TableRow>
                  <TableHead>#</TableHead>
                  <TableHead>Kind</TableHead>
                  <TableHead className="text-right">Length</TableHead>
                  <TableHead className="text-right">Height</TableHead>
                  <TableHead className="text-right">Drop</TableHead>
                  <TableHead className="text-right">Agent radius</TableHead>
                </TableRow>
              </TableHeader>
              <TableBody className="font-mono">
                {w.linkClasses.map((l) => (
                  <TableRow key={l.index}>
                    <TableCell>{l.index}</TableCell>
                    <TableCell className="font-sans" title={areaInfo(l.area).name}>
                      {describeFlags(l.flags)}
                    </TableCell>
                    <TableCell className="text-right">{formatNumber(l.jumpLength)} m</TableCell>
                    <TableCell className="text-right">{formatNumber(l.jumpHeight)} m</TableCell>
                    <TableCell className="text-right">
                      {formatNumber(l.dropdownMin)} – {formatNumber(l.dropdownMax)} m
                    </TableCell>
                    <TableCell className="text-right">{formatNumber(l.agentRadius)} m</TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </Group>
        </div>
      </SheetContent>
    </Sheet>
  )
}

function Group({ title, description, children }: { title: string; description?: string; children: ReactNode }) {
  return (
    <section className="grid content-start gap-2">
      <div>
        <h3 className="text-sm font-semibold">{title}</h3>
        {description && <p className="text-xs text-muted-foreground">{description}</p>}
      </div>
      {children}
    </section>
  )
}
