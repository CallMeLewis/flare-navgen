const integer = new Intl.NumberFormat('en-GB', { maximumFractionDigits: 0 })

export function formatInt(n: number | undefined) {
  return n === undefined ? '–' : integer.format(n)
}

export function formatNumber(n: number | undefined, digits = 2) {
  if (n === undefined || !Number.isFinite(n)) return '–'
  return n.toLocaleString('en-GB', { minimumFractionDigits: digits, maximumFractionDigits: digits })
}

export function formatBytes(n: number | undefined) {
  if (n === undefined) return '–'
  if (n < 1024) return `${n} B`
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`
  if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MB`
  return `${(n / 1024 / 1024 / 1024).toFixed(2)} GB`
}

/** 3725 -> "1:02:05" */
export function formatDuration(seconds: number | undefined) {
  if (seconds === undefined || !Number.isFinite(seconds)) return '–'
  const s = Math.max(0, Math.round(seconds))
  const h = Math.floor(s / 3600)
  const m = Math.floor((s % 3600) / 60)
  const ss = String(s % 60).padStart(2, '0')
  return h ? `${h}:${String(m).padStart(2, '0')}:${ss}` : `${m}:${ss}`
}

export function formatPercent(part: number, total: number, digits = 1) {
  return total > 0 ? `${((100 * part) / total).toFixed(digits)}%` : '–'
}

export function fileName(path: string) {
  return path.split(/[\\/]/).pop() ?? path
}
