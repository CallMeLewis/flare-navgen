// Applies the theme setting (dark / light / match system) and the interface size to the window.
import { setInterfaceScale } from '@flare/ui'
import { useSyncExternalStore } from 'react'

import { settings } from './connection'

const media = window.matchMedia('(prefers-color-scheme: dark)')

export function resolvedTheme(): 'dark' | 'light' {
  const theme = settings.get().theme
  return theme === 'system' ? (media.matches ? 'dark' : 'light') : theme
}

let scale = 1

function apply() {
  const theme = resolvedTheme()
  document.documentElement.classList.toggle('dark', theme === 'dark')
  document.documentElement.style.colorScheme = theme
  const { uiScale } = settings.get()
  if (uiScale !== scale) {
    scale = uiScale
    void setInterfaceScale(uiScale).catch(() => {})
  }
}

/** Call once at startup, before rendering. */
export function initTheme() {
  apply()
  settings.subscribe(apply)
  media.addEventListener('change', apply)
}

function subscribe(onChange: () => void) {
  const unsubscribe = settings.subscribe(onChange)
  media.addEventListener('change', onChange)
  return () => {
    unsubscribe()
    media.removeEventListener('change', onChange)
  }
}

export function useResolvedTheme() {
  return useSyncExternalStore(subscribe, resolvedTheme)
}
