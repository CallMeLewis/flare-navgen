import { CircleAlert, CircleCheck, CircleDashed, LoaderCircle } from 'lucide-react'

import { hasRealEngine } from '@/lib/engine'
import { formatDuration, formatInt, formatPercent } from '@/lib/format'
import { connection } from '@/state/connection'
import { generation } from '@/state/generation'
import { useStore } from '@/state/store'

export function StatusBar() {
  const conn = useStore(connection)
  const job = useStore(generation.summary)
  const p = job.progress

  return (
    <footer className="flex h-7 shrink-0 items-center gap-4 border-t bg-sidebar px-3 text-xs text-muted-foreground">
      <span className="flex items-center gap-1.5" role="status">
        {conn.status === 'connected' ? (
          <>
            <CircleCheck className="size-3.5 text-primary" aria-hidden />
            <span>
              Connected to <span className="font-mono text-foreground">{conn.host}</span> · {conn.world.tilesX} × {conn.world.tilesZ} tiles
            </span>
          </>
        ) : conn.status === 'connecting' ? (
          <>
            <LoaderCircle className="size-3.5 animate-spin" aria-hidden />
            <span>Connecting to {conn.host}…</span>
          </>
        ) : conn.status === 'error' ? (
          <>
            <CircleAlert className="size-3.5 text-destructive" aria-hidden />
            <span>Can't reach the data server at {conn.host}</span>
          </>
        ) : (
          <>
            <CircleDashed className="size-3.5" aria-hidden />
            <span>Not connected</span>
          </>
        )}
      </span>

      <span className="flex min-w-0 flex-1 items-center gap-2" aria-live="off">
        {(job.status === 'running' || job.status === 'stopping') && p ? (
          <>
            <span className="h-1.5 w-32 shrink-0 overflow-hidden rounded-full bg-muted" aria-hidden>
              <span className="block h-full bg-primary" style={{ width: formatPercent(p.done, p.total, 2) }} />
            </span>
            <span className="truncate font-mono">
              {job.status === 'stopping' ? 'Stopping · ' : ''}
              {formatInt(p.done)} / {formatInt(p.total)} tiles ({formatPercent(p.done, p.total)}) · {p.rate.toFixed(1)} tiles/s · {formatDuration(p.eta)} left
            </span>
          </>
        ) : job.status === 'starting' ? (
          <span>Starting generation…</span>
        ) : job.status === 'finished' ? (
          <span>
            Last run finished · {formatInt(job.built)} tiles built{job.failed ? `, ${formatInt(job.failed)} failed` : ''}
          </span>
        ) : job.status === 'stopped' ? (
          <span>{job.done?.saved ? 'Last run stopped · resume from Generate' : 'Last run stopped without saving'}</span>
        ) : job.status === 'failed' ? (
          <span className="text-destructive">Last run failed</span>
        ) : null}
      </span>

      {!hasRealEngine && <span className="rounded-sm bg-muted px-1.5 py-0.5 text-foreground">Preview: simulated engine</span>}
    </footer>
  )
}
