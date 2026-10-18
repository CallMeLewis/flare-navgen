// Files on this computer, through the desktop app (src-tauri/src/lib.rs). The browser preview has none.
import { invoke } from '@tauri-apps/api/core'

import { hasRealEngine } from './engine'

/** Whether a file is there. Always false in the browser preview. */
export async function fileExists(path: string): Promise<boolean> {
  return hasRealEngine && invoke<boolean>('file_exists', { path })
}
