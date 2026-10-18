import { AppSidebar as FlareSidebar, type SidebarItem } from '@flare/ui'
import { Map, Play, Wrench } from 'lucide-react'

import { settings } from '@/state/connection'
import { generation } from '@/state/generation'
import { useStore } from '@/state/store'

export type Page = 'generate' | 'inspect' | 'tools'

/** The Flare apps' shared sidebar, with this app's pages. Tools only shows in expert mode. */
export function AppSidebar({ page, onNavigate, onOpenSettings }: { page: Page; onNavigate: (page: Page) => void; onOpenSettings: () => void }) {
  const job = useStore(generation.summary)
  const { expert } = useStore(settings)
  const running = job.status === 'running' || job.status === 'stopping' || job.status === 'starting'
  const percent = job.progress && job.progress.total ? Math.floor((100 * job.progress.done) / job.progress.total) : 0

  const items: SidebarItem<Page>[] = [
    // A running generation's progress, beside the page it's on.
    { id: 'generate', label: 'Generate', icon: Play, ...(running && { badge: `${percent}%`, badgeLabel: `${percent}% done` }) },
    { id: 'inspect', label: 'Inspect', icon: Map },
    ...(expert ? [{ id: 'tools' as const, label: 'Tools', icon: Wrench }] : []),
  ]
  return <FlareSidebar sections={[{ label: 'Pages', items }]} active={page} onSelect={onNavigate} onOpenSettings={onOpenSettings} />
}
