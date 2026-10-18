export type Rgb = [number, number, number]

const cache = new Map<string, Rgb>()
let probe: CanvasRenderingContext2D | null = null

/** Converts any CSS colour (including oklch()) to RGB bytes. */
export function toRgb(color: string): Rgb {
  const hit = cache.get(color)
  if (hit) return hit
  probe ??= document.createElement('canvas').getContext('2d', { willReadFrequently: true })
  if (!probe) return [0, 0, 0]
  probe.clearRect(0, 0, 1, 1)
  probe.fillStyle = '#000'
  probe.fillStyle = color
  probe.fillRect(0, 0, 1, 1)
  const [r, g, b] = probe.getImageData(0, 0, 1, 1).data
  const rgb: Rgb = [r, g, b]
  cache.set(color, rgb)
  return rgb
}

/** The current value of a CSS custom property (e.g. "--tile-built") as RGB. */
export function cssVarRgb(name: string, element: Element = document.documentElement): Rgb {
  return toRgb(getComputedStyle(element).getPropertyValue(name).trim() || '#000')
}

// Viridis (perceptually uniform, colour-blind safe), 9 stops.
const viridis: Rgb[] = [
  [68, 1, 84],
  [71, 44, 122],
  [59, 81, 139],
  [44, 113, 142],
  [33, 144, 141],
  [39, 173, 129],
  [92, 200, 99],
  [170, 220, 50],
  [253, 231, 37],
]

/** t in [0, 1] -> viridis colour. */
export function viridisAt(t: number): Rgb {
  const x = Math.min(1, Math.max(0, t)) * (viridis.length - 1)
  const i = Math.min(viridis.length - 2, Math.floor(x))
  const f = x - i
  const a = viridis[i]
  const b = viridis[i + 1]
  return [a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f]
}

export const viridisGradient = `linear-gradient(to right, ${viridis.map((c) => `rgb(${c.join(' ')})`).join(', ')})`
