// DayZ navmesh vocabulary: area ids and polygon flags (docs/format/nm-format.md). Colours match
// the engine's `render` output (src/dznav/Preview.cpp).

export interface AreaInfo {
  name: string
  color: string
}

const areas: Record<number, AreaInfo> = {
  1: { name: 'Terrain', color: '#609C48' },
  2: { name: 'Water', color: '#78B4E6' },
  3: { name: 'Deep water', color: '#326EC8' },
  4: { name: 'Sea', color: '#5AC8D2' },
  5: { name: 'Deep sea', color: '#193778' },
  8: { name: 'Building', color: '#D68C3C' },
  12: { name: 'Door (open)', color: '#F0DC46' },
  13: { name: 'Door (closed)', color: '#DC3C32' },
  15: { name: 'Ladder', color: '#DC50DC' },
  16: { name: 'Crawl', color: '#965AC8' },
  17: { name: 'Crouch', color: '#E68CBE' },
}

export function areaInfo(id: number): AreaInfo {
  if (areas[id]) return areas[id]
  // 48 is the ordinary jump/climb link; 49+ are link types the world defines for particular objects.
  if (id === 48) return { name: 'Jump and climb links', color: '#FFFFFF' }
  if (id > 48) return { name: `Special links (area ${id})`, color: '#FFFFFF' }
  return { name: `Area ${id}`, color: '#A0A0A0' }
}

export const polyFlags: [bit: number, name: string][] = [
  [0x1, 'walk'],
  [0x2, 'disabled'],
  [0x4, 'door'],
  [0x8, 'inside'],
  [0x10, 'swim'],
  [0x20, 'swim (sea)'],
  [0x40, 'ladder'],
  [0x80, 'jump over'],
  [0x100, 'jump down'],
  [0x200, 'climb'],
  [0x400, 'crawl'],
  [0x800, 'crouch'],
  [0x8000, 'isolated'],
]

export function describeFlags(flags: number) {
  const names = polyFlags.filter(([bit]) => flags & bit).map(([, name]) => name)
  return names.length ? names.join(', ') : 'none'
}

/** File dialog filters. */
export const navmeshFilters = [{ name: 'DayZ navmesh', extensions: ['nm'] }]
export const captureFilters = [{ name: 'dznav geometry capture', extensions: ['dzgeo'] }]
