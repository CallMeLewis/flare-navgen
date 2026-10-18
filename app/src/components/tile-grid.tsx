// A navmesh tile map drawn on a canvas: one pixel per tile, scaled up, north at the top.
// Mouse: wheel zooms, right/middle drag pans, left drag selects a range (or click picks a tile).
// Keyboard: arrows move the focused tile (Shift extends the selection), Enter picks it,
// + / - zoom, 0 fits the map, Escape clears the selection.
import { useCallback, useEffect, useRef, useState } from 'react'

import type { TileRange } from '@/lib/engine'
import { cn } from '@/lib/utils'

export interface TileGridProps {
  tilesX: number
  tilesZ: number
  /** Change to repaint. */
  version: number
  /** Fills RGBA for every tile; pixel (x, z) is at ((tilesZ - 1 - z) * tilesX + x) * 4. */
  paint: (rgba: Uint8ClampedArray, tilesX: number, tilesZ: number) => void
  /** Text for a tile, shown on hover/focus and announced to screen readers. */
  describe: (x: number, z: number) => string
  selection?: TileRange | null
  onSelectionChange?: (range: TileRange | null) => void
  /** The picked tile (outlined). */
  picked?: [number, number] | null
  onPick?: (x: number, z: number) => void
  label: string
  className?: string
}

interface View {
  scale: number // CSS px per tile
  ox: number // CSS px offset of tile column 0
  oy: number // CSS px offset of the top row (z = tilesZ - 1)
}

export function TileGrid({
  tilesX,
  tilesZ,
  version,
  paint,
  describe,
  selection,
  onSelectionChange,
  picked,
  onPick,
  label,
  className,
}: TileGridProps) {
  const wrapRef = useRef<HTMLDivElement>(null)
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const sourceRef = useRef<HTMLCanvasElement | null>(null)
  const viewRef = useRef<View | null>(null)
  // While true the map refits on resize; zooming or panning turns it off, 0 turns it back on.
  const fittedRef = useRef(true)
  const sizeRef = useRef({ w: 0, h: 0 })
  const dragRef = useRef<{ mode: 'pan' | 'select'; x: number; y: number; ox: number; oy: number; anchor: [number, number] } | null>(null)
  const frameRef = useRef(0)
  const [hover, setHover] = useState<[number, number] | null>(null)
  const [focus, setFocus] = useState<[number, number] | null>(null)
  const [draft, setDraft] = useState<TileRange | null>(null)
  const anchorRef = useRef<[number, number] | null>(null)

  const fit = useCallback(() => {
    const { w, h } = sizeRef.current
    if (!w || !h || !tilesX || !tilesZ) return
    const scale = Math.min(w / tilesX, h / tilesZ) * 0.98
    viewRef.current = { scale, ox: (w - tilesX * scale) / 2, oy: (h - tilesZ * scale) / 2 }
    fittedRef.current = true
  }, [tilesX, tilesZ])

  const draw = useCallback(() => {
    frameRef.current = 0
    const canvas = canvasRef.current
    const source = sourceRef.current
    const view = viewRef.current
    if (!canvas || !source || !view) return
    const ctx = canvas.getContext('2d')
    if (!ctx) return
    const dpr = window.devicePixelRatio || 1
    const { w, h } = sizeRef.current
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
    ctx.clearRect(0, 0, w, h)
    ctx.imageSmoothingEnabled = false
    ctx.drawImage(source, view.ox, view.oy, tilesX * view.scale, tilesZ * view.scale)

    const style = getComputedStyle(canvas)
    const rect = (x0: number, z0: number, x1: number, z1: number) => {
      const x = view.ox + Math.min(x0, x1) * view.scale
      const y = view.oy + (tilesZ - 1 - Math.max(z0, z1)) * view.scale
      return [x, y, (Math.abs(x1 - x0) + 1) * view.scale, (Math.abs(z1 - z0) + 1) * view.scale] as const
    }
    const range = draft ?? selection
    if (range) {
      const [x, y, rw, rh] = rect(...range)
      ctx.fillStyle = 'rgb(248 250 252 / 0.12)'
      ctx.fillRect(x, y, rw, rh)
      ctx.strokeStyle = style.getPropertyValue('--foreground')
      ctx.lineWidth = 1.5
      ctx.setLineDash([5, 3])
      ctx.strokeRect(x + 0.5, y + 0.5, rw - 1, rh - 1)
      ctx.setLineDash([])
    }
    const outline = (t: [number, number] | null | undefined, color: string, width: number) => {
      if (!t) return
      const [x, y, rw, rh] = rect(t[0], t[1], t[0], t[1])
      const pad = Math.max(1, width)
      ctx.strokeStyle = color
      ctx.lineWidth = width
      ctx.strokeRect(x - pad / 2, y - pad / 2, rw + pad, rh + pad)
    }
    outline(picked, style.getPropertyValue('--primary'), 2)
    outline(hover, style.getPropertyValue('--foreground'), 1)
    if (document.activeElement === canvas) outline(focus, style.getPropertyValue('--ring'), 2)
  }, [draft, focus, hover, picked, selection, tilesX, tilesZ])

  const drawRef = useRef(draw)
  useEffect(() => {
    drawRef.current = draw
  }, [draw])
  const requestDraw = useCallback(() => {
    if (!frameRef.current) frameRef.current = requestAnimationFrame(() => drawRef.current())
  }, [])

  // Repaint the tile image when the data changes.
  useEffect(() => {
    if (!tilesX || !tilesZ) return
    let source = sourceRef.current
    if (!source || source.width !== tilesX || source.height !== tilesZ) {
      source = document.createElement('canvas')
      source.width = tilesX
      source.height = tilesZ
      sourceRef.current = source
    }
    const ctx = source.getContext('2d')
    if (!ctx) return
    const image = ctx.createImageData(tilesX, tilesZ)
    paint(image.data, tilesX, tilesZ)
    ctx.putImageData(image, 0, 0)
    requestDraw()
  }, [paint, requestDraw, tilesX, tilesZ, version])

  // Size the canvas to its container.
  useEffect(() => {
    const wrap = wrapRef.current
    const canvas = canvasRef.current
    if (!wrap || !canvas) return
    const observer = new ResizeObserver(([entry]) => {
      const { width, height } = entry.contentRect
      const dpr = window.devicePixelRatio || 1
      const first = !sizeRef.current.w
      sizeRef.current = { w: width, h: height }
      canvas.width = Math.round(width * dpr)
      canvas.height = Math.round(height * dpr)
      if (first || !viewRef.current || fittedRef.current) fit()
      drawRef.current()
    })
    observer.observe(wrap)
    return () => observer.disconnect()
  }, [fit])

  useEffect(() => {
    fit()
    requestDraw()
  }, [fit, requestDraw])

  // Overlays (selection, hover, focus) changed.
  useEffect(() => {
    requestDraw()
  }, [draw, requestDraw])

  const tileAt = (clientX: number, clientY: number): [number, number] | null => {
    const view = viewRef.current
    const canvas = canvasRef.current
    if (!view || !canvas) return null
    const r = canvas.getBoundingClientRect()
    const x = Math.floor((clientX - r.left - view.ox) / view.scale)
    const row = Math.floor((clientY - r.top - view.oy) / view.scale)
    const z = tilesZ - 1 - row
    return x >= 0 && x < tilesX && z >= 0 && z < tilesZ ? [x, z] : null
  }

  const clampTile = (x: number, z: number): [number, number] => [
    Math.min(tilesX - 1, Math.max(0, x)),
    Math.min(tilesZ - 1, Math.max(0, z)),
  ]

  const zoom = (factor: number, cx?: number, cy?: number) => {
    const view = viewRef.current
    if (!view) return
    const { w, h } = sizeRef.current
    const px = cx ?? w / 2
    const py = cy ?? h / 2
    const scale = Math.min(64, Math.max(0.5, view.scale * factor))
    const k = scale / view.scale
    fittedRef.current = false
    viewRef.current = { scale, ox: px - (px - view.ox) * k, oy: py - (py - view.oy) * k }
    requestDraw()
  }

  const onWheel = (e: React.WheelEvent) => {
    const r = canvasRef.current!.getBoundingClientRect()
    zoom(e.deltaY < 0 ? 1.2 : 1 / 1.2, e.clientX - r.left, e.clientY - r.top)
  }

  const onPointerDown = (e: React.PointerEvent) => {
    const view = viewRef.current
    if (!view) return
    canvasRef.current!.setPointerCapture(e.pointerId)
    const t = tileAt(e.clientX, e.clientY)
    const pan = e.button !== 0 || e.altKey || !t
    dragRef.current = { mode: pan ? 'pan' : 'select', x: e.clientX, y: e.clientY, ox: view.ox, oy: view.oy, anchor: t ?? [0, 0] }
    if (!pan && t) {
      setFocus(t)
      if (onSelectionChange) setDraft([t[0], t[1], t[0], t[1]])
    }
  }

  const onPointerMove = (e: React.PointerEvent) => {
    const drag = dragRef.current
    const t = tileAt(e.clientX, e.clientY)
    if (t?.[0] !== hover?.[0] || t?.[1] !== hover?.[1]) setHover(t)
    if (!drag) return
    if (drag.mode === 'pan') {
      fittedRef.current = false
      viewRef.current = { ...viewRef.current!, ox: drag.ox + e.clientX - drag.x, oy: drag.oy + e.clientY - drag.y }
      requestDraw()
    } else if (onSelectionChange && t) {
      setDraft(normalise([drag.anchor[0], drag.anchor[1], t[0], t[1]]))
    }
  }

  const onPointerUp = (e: React.PointerEvent) => {
    const drag = dragRef.current
    dragRef.current = null
    if (!drag || drag.mode !== 'select') return
    const moved = Math.hypot(e.clientX - drag.x, e.clientY - drag.y) > 4
    if (onSelectionChange && draft && moved) onSelectionChange(draft)
    else onPick?.(drag.anchor[0], drag.anchor[1])
    setDraft(null)
  }

  const onKeyDown = (e: React.KeyboardEvent) => {
    const current = focus ?? [Math.floor(tilesX / 2), Math.floor(tilesZ / 2)]
    const step = e.ctrlKey ? 10 : 1
    const moves: Record<string, [number, number]> = {
      ArrowLeft: [-step, 0],
      ArrowRight: [step, 0],
      ArrowUp: [0, step],
      ArrowDown: [0, -step],
    }
    if (moves[e.key]) {
      e.preventDefault()
      const next = clampTile(current[0] + moves[e.key][0], current[1] + moves[e.key][1])
      if (e.shiftKey && onSelectionChange) {
        anchorRef.current ??= current
        onSelectionChange(normalise([anchorRef.current[0], anchorRef.current[1], next[0], next[1]]))
      } else {
        anchorRef.current = null
      }
      setFocus(next)
      ensureVisible(next)
    } else if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault()
      onPick?.(current[0], current[1])
    } else if (e.key === '+' || e.key === '=') {
      zoom(1.25)
    } else if (e.key === '-') {
      zoom(1 / 1.25)
    } else if (e.key === '0') {
      fit()
      requestDraw()
    } else if (e.key === 'Escape' && onSelectionChange) {
      anchorRef.current = null
      onSelectionChange(null)
    }
  }

  const ensureVisible = ([x, z]: [number, number]) => {
    const view = viewRef.current
    if (!view) return
    const { w, h } = sizeRef.current
    const px = view.ox + x * view.scale
    const py = view.oy + (tilesZ - 1 - z) * view.scale
    let { ox, oy } = view
    const m = view.scale * 2
    if (px < m) ox += m - px
    if (px > w - m) ox -= px - (w - m)
    if (py < m) oy += m - py
    if (py > h - m) oy -= py - (h - m)
    viewRef.current = { ...view, ox, oy }
  }

  const readout = hover ?? focus
  return (
    <div className={cn('relative flex min-h-0 flex-col', className)}>
      <div ref={wrapRef} className="relative min-h-0 flex-1 overflow-hidden">
        <canvas
          ref={canvasRef}
          role="application"
          aria-label={`${label}. Arrow keys move between tiles${onSelectionChange ? ', Shift and arrows select a range' : ''}, Enter opens a tile, plus and minus zoom, 0 fits the map.`}
          tabIndex={0}
          className="absolute inset-0 size-full cursor-crosshair touch-none rounded-sm outline-none focus-visible:ring-2 focus-visible:ring-ring"
          onWheel={onWheel}
          onPointerDown={onPointerDown}
          onPointerMove={onPointerMove}
          onPointerUp={onPointerUp}
          onPointerLeave={() => setHover(null)}
          onContextMenu={(e) => e.preventDefault()}
          onKeyDown={onKeyDown}
          onFocus={() => {
            if (!focus) setFocus([Math.floor(tilesX / 2), Math.floor(tilesZ / 2)])
            requestDraw()
          }}
          onBlur={requestDraw}
        />
      </div>
      <div className="flex h-7 shrink-0 items-center gap-3 border-t px-2 font-mono text-xs text-muted-foreground" aria-hidden>
        {readout ? (
          <span className="truncate">
            <span className="text-foreground">
              {readout[0]}, {readout[1]}
            </span>{' '}
            {describe(readout[0], readout[1])}
          </span>
        ) : (
          <span>Hover or focus a tile for details · wheel to zoom · right-drag to pan</span>
        )}
      </div>
      <div className="sr-only" aria-live="polite">
        {focus ? `Tile ${focus[0]}, ${focus[1]}: ${describe(focus[0], focus[1])}` : ''}
      </div>
    </div>
  )
}

function normalise([x0, z0, x1, z1]: TileRange): TileRange {
  return [Math.min(x0, x1), Math.min(z0, z1), Math.max(x0, x1), Math.max(z0, z1)]
}
