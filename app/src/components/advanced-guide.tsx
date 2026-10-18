// Plain-English explanations of the Advanced settings on Generate, shown in a side panel.
import type { ReactNode } from 'react'

import { Sheet, SheetContent, SheetDescription, SheetHeader, SheetTitle } from '@/components/ui/sheet'

export type GuideTopic = 'sea-level' | 'doors' | 'links' | 'islands' | 'capture' | 'record' | 'autosave' | 'threads'

interface Entry {
  id: GuideTopic
  title: string
  what: ReactNode
  default: ReactNode
  when: ReactNode
}

const entries: Entry[] = [
  {
    id: 'sea-level',
    title: 'Pin sea level',
    what: (
      <>
        Everything below the sea level is marked as sea, so AI won't walk there. In practice, it decides where the shoreline sits on beaches and gentle slopes. Tiles away
        from the coast aren't affected.
      </>
    ),
    default: (
      <>
        Off. The app uses the tide the server reported when you connected. When you resume a run or update a navmesh this app made, it reuses the sea level from the
        original run, so the coastline matches across the whole map. When you build from a capture file, the sea level saved in that file is used.
      </>
    ),
    when: (
      <>
        Turn it on if you need a particular height, for example to match another navmesh exactly. The value is in metres. The right value is different for every map, and
        the tide changes over time, so don't copy a number from another map.
      </>
    ),
  },
  {
    id: 'doors',
    title: 'Door state in the navmesh',
    what: (
      <>
        Doors can be open or closed. This setting decides which state the navmesh starts with: closed, open, or both, where AI can use the doorway either way.
      </>
    ),
    default: <>Closed.</>,
    when: (
      <>
        The game updates doors itself as they open and close, so this setting makes very little difference in practice. Leave it on Closed unless you have a reason to
        change it.
      </>
    ),
  },
  {
    id: 'links',
    title: 'Jump and climb links',
    what: (
      <>
        Links let AI move between surfaces that aren't directly connected: jumping over fences and low walls, climbing onto ledges, and dropping down from them. How far
        and how high AI can jump comes from your map's server settings.
      </>
    ),
    default: <>On.</>,
    when: (
      <>
        Leave it on. Without links, AI has to walk around every fence and wall, and some places can't be reached at all. Turning it off is only useful for testing.
      </>
    ),
  },
  {
    id: 'islands',
    title: 'Flag unreachable areas',
    what: (
      <>
        After building, the island filter starts at your map's seed point and follows every route AI could walk. Anywhere it can't reach, such as rooftops or closed-off
        rooms, is marked as unreachable so AI doesn't use it.
      </>
    ),
    default: <>On, unless your map's server settings turn the island filter off.</>,
    when: (
      <>
        Leave it on. If you stop a run, the filter is skipped, and it runs over the whole map when you resume. Turning it off is only useful for testing.
      </>
    ),
  },
  {
    id: 'capture',
    title: 'Build from a capture file',
    what: (
      <>
        Builds from geometry saved earlier in a capture file (.dzgeo) instead of fetching it from DayZDiag, so DayZDiag doesn't need to be running. The map's settings
        come from the capture file too.
      </>
    ),
    default: <>Off. Geometry comes from the data server.</>,
    when: (
      <>
        Use it to rebuild a navmesh with different settings without starting DayZDiag again. A capture file only holds the tiles that were fetched when it was saved, so
        it can only rebuild those tiles.
      </>
    ),
  },
  {
    id: 'record',
    title: 'Also save geometry to a capture file',
    what: <>Saves the geometry fetched during this run to a capture file, as well as building the navmesh.</>,
    default: <>Off.</>,
    when: <>Turn it on if you might want to rebuild later, for example to try different settings, without starting DayZDiag.</>,
  },
  {
    id: 'autosave',
    title: 'Auto-save',
    what: (
      <>
        Saves the navmesh every time this many tiles have been built. If the app or your PC crashes, you can resume from the last save instead of starting again.
      </>
    ),
    default: <>Off. The navmesh is saved when the run finishes, or when you press Stop and save.</>,
    when: (
      <>
        Turn it on for long runs. Each save writes the whole file, which can be over a gigabyte for a large map, so a very small number slows the run down. As a rough
        guide, 10,000 tiles saves every few minutes on a typical map.
      </>
    ),
  },
  {
    id: 'threads',
    title: 'Builder threads',
    what: <>How many tiles are built at the same time. More threads finish sooner but use more of your processor.</>,
    default: <>Every processor core but one, which leaves some room for the rest of your PC.</>,
    when: (
      <>
        Lower it if your PC struggles while generating, for example if DayZDiag is running on the same PC and slows down. You can change it in Settings.
      </>
    ),
  },
]

/** The guide, opened at a setting's entry (or at the top). */
export function AdvancedGuide({ open, topic, onOpenChange }: { open: boolean; topic: GuideTopic | null; onOpenChange: (open: boolean) => void }) {
  return (
    <Sheet open={open} onOpenChange={onOpenChange}>
      <SheetContent
        className="w-full overflow-y-auto outline-none data-[side=right]:sm:max-w-lg"
        onOpenAutoFocus={(e) => {
          e.preventDefault()
          const target = topic && document.getElementById(`guide-${topic}`)
          ;(target || (e.currentTarget as HTMLElement)).focus()
        }}
      >
        <SheetHeader>
          <SheetTitle>Advanced settings explained</SheetTitle>
          <SheetDescription>
            A navmesh is the map AI uses to work out where it can walk. These settings change how it's built. The defaults suit almost every map, so you only need them in
            special cases.
          </SheetDescription>
        </SheetHeader>
        <div className="grid gap-6 px-4 pb-6 text-sm">
          {entries.map((e) => (
            <section key={e.id} aria-labelledby={`guide-${e.id}`} className="grid gap-2">
              <h3 id={`guide-${e.id}`} tabIndex={-1} className="scroll-mt-4 rounded-sm text-base font-semibold outline-none focus-visible:ring-2 focus-visible:ring-ring">
                {e.title}
              </h3>
              <p>{e.what}</p>
              <dl className="grid gap-2">
                <div>
                  <dt className="text-xs font-medium text-muted-foreground">Default</dt>
                  <dd>{e.default}</dd>
                </div>
                <div>
                  <dt className="text-xs font-medium text-muted-foreground">When to change it</dt>
                  <dd>{e.when}</dd>
                </div>
              </dl>
            </section>
          ))}
        </div>
      </SheetContent>
    </Sheet>
  )
}
