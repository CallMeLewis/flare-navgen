import { AppShell } from '@flare/ui'
import { useEffect, useState } from 'react'

import { AppMark } from '@/components/app-mark'
import { AppSidebar, type Page } from '@/components/app-sidebar'
import { SettingsDialog } from '@/components/settings-dialog'
import { StatusBar } from '@/components/status-bar'
import { Button } from '@flare/ui/ui/button'
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '@flare/ui/ui/dialog'
import { hasRealEngine } from '@/lib/engine'
import { GeneratePage } from '@/pages/generate'
import { InspectPage } from '@/pages/inspect'
import { ToolsPage } from '@/pages/tools'
import { settings } from '@/state/connection'
import { generation } from '@/state/generation'
import { useStore } from '@/state/store'
import { runningTools, toolRuns, toolTitles, whenToolsIdle } from '@/state/tools'

export default function App() {
  const [chosen, setPage] = useState<Page>('generate')
  const { expert } = useStore(settings)
  // Tools is an expert page.
  const page = chosen === 'tools' && !expert ? 'generate' : chosen
  const [confirmClose, setConfirmClose] = useState(false)
  const [quitting, setQuitting] = useState(false)
  const [settingsOpen, setSettingsOpen] = useState(false)
  // What the close prompt is about: a generation, or else the tools still running (merging, say). Once neither is, it
  // has nothing to ask and goes away.
  useStore(generation.summary)
  useStore(toolRuns)
  const generating = generation.running
  const tools = runningTools()
  if (confirmClose && !quitting && !generating && !tools.length) setConfirmClose(false)

  // Closing the window during a run asks first.
  useEffect(() => {
    if (!hasRealEngine) return
    let unlisten: (() => void) | undefined
    void import('@tauri-apps/api/window').then(async ({ getCurrentWindow }) => {
      unlisten = await getCurrentWindow().onCloseRequested((event) => {
        if (generation.running || runningTools().length) {
          event.preventDefault()
          setConfirmClose(true)
        }
      })
    })
    return () => unlisten?.()
  }, [])

  // `wait`: stop the generation, saving it, and let the tools finish. Otherwise everything ends as the app closes.
  const quit = async (mode: 'wait' | 'now') => {
    setQuitting(true)
    try {
      if (mode === 'wait') {
        await generation.stop()
        await Promise.all([generation.whenIdle(), whenToolsIdle()])
      } else {
        await generation.kill()
      }
      const { getCurrentWindow } = await import('@tauri-apps/api/window')
      await getCurrentWindow().destroy()
    } catch {
      setQuitting(false)
    }
  }

  return (
    <AppShell
      // The sky blue of the app's icon.
      mark={<AppMark className="size-3.5" />}
      markClassName="bg-brand/15 text-brand"
      sidebar={<AppSidebar page={page} onNavigate={setPage} onOpenSettings={() => setSettingsOpen(true)} />}
    >
      <main className="flex min-w-0 flex-1 flex-col">
        <div className="flex min-h-0 flex-1 flex-col">
          {page === 'generate' && <GeneratePage onNavigate={setPage} onOpenSettings={() => setSettingsOpen(true)} />}
          {page === 'inspect' && <InspectPage />}
          {page === 'tools' && <ToolsPage />}
        </div>
        <StatusBar />
      </main>

      <SettingsDialog open={settingsOpen} onOpenChange={setSettingsOpen} />

      <Dialog open={confirmClose} onOpenChange={(open) => !quitting && setConfirmClose(open)}>
        <DialogContent showCloseButton={!quitting}>
          {generating ? (
            <DialogHeader>
              <DialogTitle>A navmesh is still generating</DialogTitle>
              <DialogDescription>
                Stopping saves the tiles built so far, so you can resume later. Quitting without saving loses everything since the last checkpoint.
                {tools.length > 0 && ` ${toolTitles[tools[0]]} is running too, and finishes first when you stop.`}
              </DialogDescription>
            </DialogHeader>
          ) : (
            <DialogHeader>
              <DialogTitle>{tools.length > 1 ? 'Tools are still running' : `${tools.length ? toolTitles[tools[0]] : 'A tool'} is still running`}</DialogTitle>
              <DialogDescription>Quitting now ends {tools.length > 1 ? 'them' : 'it'}, and can leave a file being saved incomplete.</DialogDescription>
            </DialogHeader>
          )}
          <DialogFooter>
            <Button variant="ghost" onClick={() => setConfirmClose(false)} disabled={quitting}>
              Keep running
            </Button>
            <Button variant="destructive" onClick={() => quit('now')} disabled={quitting}>
              {generating ? 'Quit without saving' : 'Quit now'}
            </Button>
            <Button onClick={() => quit('wait')} disabled={quitting}>
              {quitting ? (generating ? 'Saving…' : 'Finishing…') : generating ? 'Stop, save and quit' : 'Quit when it finishes'}
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </AppShell>
  )
}
