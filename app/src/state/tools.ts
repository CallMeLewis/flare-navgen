// Runs of the Tools page's commands. Kept here rather than in the page, so a run carries on, and its result is still
// there, after leaving Tools, and so closing the window can ask first while one is still running.
import { EngineError, runEngine, type EventOf } from '@/lib/engine'

import { Store } from './store'

/** Each tool is named after the engine command it runs, which is also the event it expects back. */
export type ToolName = 'verify' | 'diff' | 'merge' | 'islands'

export interface ToolRun<K extends ToolName = ToolName> {
  busy: boolean
  result: EventOf<K> | null
  error: string | null
}

export const toolTitles: Record<ToolName, string> = {
  verify: "Check against Bohemia's generator",
  diff: 'Compare two navmeshes',
  merge: 'Merge a regenerated area',
  islands: 'Flag unreachable areas again',
}

export const toolRuns = new Store<{ [K in ToolName]?: ToolRun<K> }>({})

/** Runs `dznav <tool> <args>`; the tool's run in `toolRuns` follows it. */
export async function runTool<K extends ToolName>(tool: K, args: string[]) {
  const set = (run: ToolRun<K>) => toolRuns.set((runs) => ({ ...runs, [tool]: run }))
  set({ busy: true, result: null, error: null })
  try {
    set({ busy: false, result: await runEngine([tool, ...args], tool), error: null })
  } catch (e) {
    set({ busy: false, result: null, error: e instanceof EngineError ? e.message : String(e) })
  }
}

/** The tools running now. */
export function runningTools(): ToolName[] {
  return (Object.keys(toolRuns.get()) as ToolName[]).filter((tool) => toolRuns.get()[tool]?.busy)
}

/** Resolves once no tool is running. */
export function whenToolsIdle(): Promise<void> {
  return new Promise((resolve) => {
    if (!runningTools().length) return resolve()
    const unsubscribe = toolRuns.subscribe(() => {
      if (!runningTools().length) {
        unsubscribe()
        resolve()
      }
    })
  })
}
