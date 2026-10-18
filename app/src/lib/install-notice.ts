import type { InstallNotice } from '@flare/ui'

import { generation } from '@/state/generation'
import { useStore } from '@/state/store'
import { runningTools, toolRuns } from '@/state/tools'

/** What the update's install prompt warns about while a navmesh generates or a tool runs, as installing ends them. */
export function useInstallNotice(): InstallNotice | null {
  const job = useStore(generation.summary)
  const running = job.status === 'starting' || job.status === 'running' || job.status === 'stopping'
  // Subscribed so the warning follows the tools.
  useStore(toolRuns)
  const toolRunning = runningTools().length > 0
  if (!running && !toolRunning) return null
  const notes = [
    running && 'A navmesh is still generating: installing now ends it without saving. To keep the tiles built so far, press Stop and save first.',
    toolRunning && 'A tool is still running: installing now ends it, and can leave a file it is saving incomplete.',
  ].filter(Boolean)
  return {
    note: notes.join(' '),
    confirmLabel: running ? 'Install and end the run' : 'Install and end the tool',
    destructive: true,
  }
}
