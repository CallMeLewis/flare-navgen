import { useSyncExternalStore } from 'react'

/** Minimal observable value for state shared across pages. */
export class Store<T> {
  private value: T
  private listeners = new Set<() => void>()

  constructor(initial: T) {
    this.value = initial
  }

  get = () => this.value

  set(next: T | ((prev: T) => T)) {
    this.value = typeof next === 'function' ? (next as (prev: T) => T)(this.value) : next
    for (const l of this.listeners) l()
  }

  subscribe = (listener: () => void) => {
    this.listeners.add(listener)
    return () => {
      this.listeners.delete(listener)
    }
  }
}

export function useStore<T>(store: Store<T>): T {
  return useSyncExternalStore(store.subscribe, store.get)
}

/** A Store persisted to localStorage. */
export function persistentStore<T extends object>(key: string, defaults: T): Store<T> {
  let initial = defaults
  try {
    const saved = localStorage.getItem(key)
    if (saved) initial = { ...defaults, ...JSON.parse(saved, revive) }
  } catch {
    // ignore unreadable settings
  }
  const store = new Store<T>(initial)
  store.subscribe(() => localStorage.setItem(key, JSON.stringify(store.get(), replace)))
  return store
}

// JSON writes NaN, which number fields hold while they're empty, as null, which reads back as something else (a sea
// level that is no longer pinned, a range showing "null"). So it's written as this instead.
const NAN = { $number: 'NaN' }

function replace(_key: string, value: unknown) {
  return typeof value === 'number' && Number.isNaN(value) ? NAN : value
}

function revive(_key: string, value: unknown) {
  return typeof value === 'object' && value !== null && (value as typeof NAN).$number === 'NaN' ? NaN : value
}
