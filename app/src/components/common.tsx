import { FolderOpen, Info } from 'lucide-react'
import type { ReactNode } from 'react'
import { useId } from 'react'

import { Button } from '@flare/ui/ui/button'
import { Input } from '@flare/ui/ui/input'
import { Label } from '@flare/ui/ui/label'
import { Switch } from '@flare/ui/ui/switch'
import { Tooltip, TooltipContent, TooltipTrigger } from '@flare/ui/ui/tooltip'
import { hasRealEngine } from '@/lib/engine'
import { cn } from '@/lib/utils'

export function PageHeader({ title, description, actions }: { title: string; description?: ReactNode; actions?: ReactNode }) {
  return (
    <header className="flex min-h-14 shrink-0 items-center gap-4 border-b px-4 py-2">
      <div className="min-w-0 flex-1">
        <h1 className="text-xl font-semibold">{title}</h1>
        {description && <p className="truncate text-sm text-muted-foreground">{description}</p>}
      </div>
      {actions && <div className="flex shrink-0 items-center gap-2">{actions}</div>}
    </header>
  )
}

export function Stat({ label, value, sub, className }: { label: string; value: ReactNode; sub?: ReactNode; className?: string }) {
  return (
    <div className={cn('min-w-0', className)}>
      <div className="text-xs text-muted-foreground">{label}</div>
      <div className="truncate font-mono text-base text-foreground">{value}</div>
      {sub && <div className="truncate font-mono text-xs text-muted-foreground">{sub}</div>}
    </div>
  )
}

/** Label/value rows for parameter lists, with an optional explanation per row. */
export function Facts({ rows }: { rows: [label: string, value: ReactNode, hint?: string][] }) {
  return (
    <dl className="grid grid-cols-[auto_1fr] gap-x-4 gap-y-1.5 text-sm">
      {rows.map(([label, value, hint]) => (
        <div key={label} className="contents">
          <dt className="flex items-center gap-1 text-muted-foreground">
            {label}
            {hint && (
              <Tooltip>
                <TooltipTrigger asChild>
                  <button type="button" className="rounded-sm text-muted-foreground/70 hover:text-foreground focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none" aria-label={`About ${label.toLowerCase()}`}>
                    <Info className="size-3.5" />
                  </button>
                </TooltipTrigger>
                <TooltipContent className="max-w-64">{hint}</TooltipContent>
              </Tooltip>
            )}
          </dt>
          <dd className="text-right font-mono">{value}</dd>
        </div>
      ))}
    </dl>
  )
}

export interface FileFieldProps {
  label: string
  value: string
  /** `chosen` is true for a path picked in the file dialog, which for `save` has already asked about replacing it. */
  onChange: (path: string, chosen?: boolean) => void
  mode: 'open' | 'save'
  filters?: { name: string; extensions: string[] }[]
  placeholder?: string
  hint?: ReactNode
  error?: string
  disabled?: boolean
  /** Opens a fuller explanation of the field. */
  onHelp?: () => void
}

/** A path input with a Browse button (native dialog in the desktop app). */
export function FileField({ label, value, onChange, mode, filters, placeholder, hint, error, disabled, onHelp }: FileFieldProps) {
  const id = useId()
  const browse = async () => {
    const dialog = await import('@tauri-apps/plugin-dialog')
    const path = mode === 'open' ? await dialog.open({ multiple: false, directory: false, filters }) : await dialog.save({ filters, defaultPath: value || undefined })
    if (typeof path === 'string') onChange(path, true)
  }
  return (
    <div className="grid gap-1.5">
      <LabelRow htmlFor={id} label={label} onHelp={onHelp} />
      <div className="flex gap-1.5">
        <Input
          id={id}
          value={value}
          onChange={(e) => onChange(e.target.value)}
          placeholder={placeholder}
          disabled={disabled}
          aria-invalid={!!error}
          aria-describedby={hint || error ? `${id}-hint` : undefined}
          className="font-mono"
          spellCheck={false}
        />
        {hasRealEngine && (
          <Tooltip>
            <TooltipTrigger asChild>
              <Button variant="outline" size="icon" onClick={browse} disabled={disabled} aria-label={`Browse for ${label.toLowerCase()}`}>
                <FolderOpen />
              </Button>
            </TooltipTrigger>
            <TooltipContent>Browse</TooltipContent>
          </Tooltip>
        )}
      </div>
      {(error || hint) && (
        <p id={`${id}-hint`} className={cn('text-xs', error ? 'text-destructive' : 'text-muted-foreground')}>
          {error ?? hint}
        </p>
      )}
    </div>
  )
}

export function Swatch({ color, className, pattern }: { color: string; className?: string; pattern?: 'dots' | 'cross' }) {
  return (
    <span
      aria-hidden
      className={cn('inline-block size-3 shrink-0 rounded-[2px] border border-black/20', className)}
      style={{
        backgroundColor: color,
        backgroundImage:
          pattern === 'cross'
            ? 'linear-gradient(45deg, transparent 42%, rgb(0 0 0 / .6) 42% 58%, transparent 58%), linear-gradient(-45deg, transparent 42%, rgb(0 0 0 / .6) 42% 58%, transparent 58%)'
            : pattern === 'dots'
              ? 'radial-gradient(rgb(0 0 0 / .45) 30%, transparent 32%)'
              : undefined,
        backgroundSize: pattern === 'dots' ? '4px 4px' : undefined,
      }}
    />
  )
}

/** A labelled group of controls (toggle groups, selects) with an optional hint or error. */
export function Field({ label, hint, error, onHelp, children }: { label: string; hint?: ReactNode; error?: string; onHelp?: () => void; children: ReactNode }) {
  const id = useId()
  return (
    <div className="grid gap-1.5" role="group" aria-labelledby={id} aria-describedby={hint || error ? `${id}-hint` : undefined}>
      <div className="flex items-center gap-1">
        <span id={id} className="text-sm leading-none font-medium">
          {label}
        </span>
        {onHelp && <HelpButton label={label} onClick={onHelp} />}
      </div>
      {children}
      {(error || hint) && (
        <p id={`${id}-hint`} className={cn('text-xs', error ? 'text-destructive' : 'text-muted-foreground')}>
          {error ?? hint}
        </p>
      )}
    </div>
  )
}

export function TextField({
  label,
  value,
  onChange,
  hint,
  error,
  placeholder,
  mono,
  inputMode,
  onHelp,
}: {
  label: string
  value: string
  onChange: (v: string) => void
  hint?: ReactNode
  error?: string
  placeholder?: string
  mono?: boolean
  inputMode?: 'decimal' | 'numeric'
  onHelp?: () => void
}) {
  const id = useId()
  return (
    <div className="grid gap-1.5">
      <LabelRow htmlFor={id} label={label} onHelp={onHelp} />
      <Input
        id={id}
        value={value === 'NaN' ? '' : value}
        onChange={(e) => onChange(e.target.value)}
        placeholder={placeholder}
        className={cn(mono && 'font-mono')}
        inputMode={inputMode}
        spellCheck={false}
        aria-invalid={!!error}
        aria-describedby={hint || error ? `${id}-hint` : undefined}
      />
      {(error || hint) && (
        <p id={`${id}-hint`} className={cn('text-xs', error ? 'text-destructive' : 'text-muted-foreground')}>
          {error ?? hint}
        </p>
      )}
    </div>
  )
}

export function SwitchField({
  label,
  hint,
  checked,
  onCheckedChange,
  disabled,
  onHelp,
}: {
  label: string
  hint?: ReactNode
  checked: boolean
  onCheckedChange: (v: boolean) => void
  disabled?: boolean
  onHelp?: () => void
}) {
  const id = useId()
  return (
    <div className="flex items-start justify-between gap-3">
      <div className="grid gap-0.5">
        <LabelRow htmlFor={id} label={label} onHelp={onHelp} className={cn(disabled && 'opacity-60')} />
        {hint && (
          <p id={`${id}-hint`} className="text-xs text-muted-foreground">
            {hint}
          </p>
        )}
      </div>
      <Switch id={id} checked={checked} onCheckedChange={onCheckedChange} disabled={disabled} aria-describedby={hint ? `${id}-hint` : undefined} />
    </div>
  )
}

/** A field label, with an info button when there's a fuller explanation. */
function LabelRow({ htmlFor, label, onHelp, className }: { htmlFor: string; label: string; onHelp?: () => void; className?: string }) {
  if (!onHelp)
    return (
      <Label htmlFor={htmlFor} className={className}>
        {label}
      </Label>
    )
  return (
    <div className={cn('flex items-center gap-1', className)}>
      <Label htmlFor={htmlFor}>{label}</Label>
      <HelpButton label={label} onClick={onHelp} />
    </div>
  )
}

function HelpButton({ label, onClick }: { label: string; onClick: () => void }) {
  return (
    <button
      type="button"
      onClick={onClick}
      className="rounded-sm text-muted-foreground/70 hover:text-foreground focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none"
      aria-label={`About ${label.toLowerCase()}`}
    >
      <Info className="size-3.5" />
    </button>
  )
}
