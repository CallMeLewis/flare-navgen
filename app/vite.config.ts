import { readFileSync } from 'node:fs'
import path from 'node:path'
import tailwindcss from '@tailwindcss/vite'
import react from '@vitejs/plugin-react'
import { defineConfig, searchForWorkspaceRoot } from 'vite'

import pkg from './package.json' with { type: 'json' }
import { notesFor } from '@flare/release/changelog'

const host = process.env.TAURI_DEV_HOST
// The running version's release notes, shown in Settings > About.
const notes = notesFor(readFileSync(path.resolve(import.meta.dirname, '../CHANGELOG.md'), 'utf8'), pkg.version) ?? []

// https://v2.tauri.app/start/frontend/vite/
export default defineConfig({
  plugins: [react(), tailwindcss()],
  define: { __APP_VERSION__: JSON.stringify(pkg.version), __APP_RELEASE_NOTES__: JSON.stringify(notes) },
  resolve: {
    alias: { '@': path.resolve(import.meta.dirname, './src') },
    // The shared interface (@flare/ui) uses this app's copies of these, also when it's linked from a flare-common
    // checkout that has its own.
    dedupe: ['react', 'react-dom', 'radix-ui', 'lucide-react', 'cn', 'class-variance-authority', '@tauri-apps/api'],
  },
  clearScreen: false,
  server: {
    port: 1420,
    strictPort: true,
    host: host || false,
    hmr: host ? { protocol: 'ws', host, port: 1421 } : undefined,
    watch: { ignored: ['**/src-tauri/**'] },
    // A linked flare-common checkout serves the shared interface's fonts from its own node_modules.
    fs: { allow: [searchForWorkspaceRoot(process.cwd()), path.resolve(import.meta.dirname, '../../flare-common')] },
  },
  envPrefix: ['VITE_', 'TAURI_ENV_*'],
  build: {
    target: 'chrome120',
    minify: !process.env.TAURI_ENV_DEBUG ? 'oxc' : false,
    sourcemap: !!process.env.TAURI_ENV_DEBUG,
  },
})
