import { AboutSection, AppearanceSettings, InfoRow, SettingRow, SettingsDialog as FlareSettingsDialog, SettingsGroup } from '@flare/ui'
import { Cpu, Info, SlidersHorizontal } from 'lucide-react'

import { Input } from '@flare/ui/ui/input'
import { Switch } from '@flare/ui/ui/switch'
import { hasRealEngine } from '@/lib/engine'
import { settings, type Settings } from '@/state/connection'
import { useStore } from '@/state/store'
import appIcon from '../../src-tauri/icons/128x128@2x.png'

const PROJECT_PAGE = 'https://github.com/CallMeLewis/flare-navgen'

/** Settings, opened from the gear at the bottom of the sidebar, in the dialog every Flare app shares. */
export function SettingsDialog({ open, onOpenChange }: { open: boolean; onOpenChange: (open: boolean) => void }) {
  const s = useStore(settings)
  const set = (patch: Partial<Settings>) => settings.set((prev) => ({ ...prev, ...patch }))

  return (
    <FlareSettingsDialog
      open={open}
      onOpenChange={onOpenChange}
      sections={[
        {
          id: 'general',
          label: 'General',
          icon: SlidersHorizontal,
          description: 'How Flare Navgen looks and how much it shows.',
          content: (
            <>
              <AppearanceSettings theme={s.theme} onThemeChange={(theme) => set({ theme })} scale={s.uiScale} onScaleChange={(uiScale) => set({ uiScale })} />
              <SettingsGroup title="Expert mode">
                <SettingRow
                  id="expert"
                  label="Show expert details"
                  description="Adds the Tools page (merging, the island filter and comparing with Bohemia's generator), server details, extra build statistics and the build-time map."
                >
                  <Switch id="expert" aria-describedby="expert-description" checked={s.expert} onCheckedChange={(expert) => set({ expert })} />
                </SettingRow>
              </SettingsGroup>
            </>
          ),
        },
        {
          id: 'generation',
          label: 'Generation',
          icon: Cpu,
          description: 'How navmeshes are built on this computer.',
          content: (
            <SettingsGroup title="Performance">
              <SettingRow id="threads" label="Builder threads" description="How many tiles build at once. Leave it empty to use every core but one.">
                <Input
                  id="threads"
                  aria-describedby="threads-description"
                  inputMode="numeric"
                  autoComplete="off"
                  spellCheck={false}
                  placeholder="cores − 1"
                  value={String(s.threads)}
                  onChange={(e) => {
                    const v = e.target.value
                    set({ threads: v === '' ? '' : Math.max(1, Math.floor(Number(v)) || 1) })
                  }}
                  className="w-32 shrink-0 font-mono"
                />
              </SettingRow>
            </SettingsGroup>
          ),
        },
        {
          id: 'about',
          label: 'About',
          icon: Info,
          description: 'Version, updates, release notes and the engine.',
          pinned: true,
          content: (
            <AboutSection
              icon={appIcon}
              tagline="Generate and inspect DayZ navmeshes."
              onOpenProject={openProjectPage}
              credits={
                // Recast & Detour's licence (zlib) asks for credit in the product's documentation.
                <>
                  <p>Navmeshes are built with Recast & Detour, under the zlib licence.</p>
                  <p>Flare Navgen is an independent project, not made or endorsed by Bohemia Interactive. DayZ is a trademark of Bohemia Interactive a.s.</p>
                </>
              }
            >
              <SettingsGroup title="Engine">
                <InfoRow
                  label="Engine"
                  description="Builds the navmeshes. It comes with Flare Navgen and updates with it."
                  value={hasRealEngine ? `dznav ${__APP_VERSION__}` : 'Simulated (browser preview)'}
                />
                <InfoRow label="Data server port" description="Where DayZDiag serves the map data the engine reads." value="36533" />
              </SettingsGroup>
            </AboutSection>
          ),
        },
      ]}
    />
  )
}

function openProjectPage() {
  if (!hasRealEngine) {
    window.open(PROJECT_PAGE, '_blank', 'noopener')
    return
  }
  void import('@tauri-apps/api/core').then(({ invoke }) => invoke('open_project_page'))
}
