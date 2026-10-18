import { blockWebviewMenu, FlareProvider } from '@flare/ui'
import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'

import { TooltipProvider } from '@flare/ui/ui/tooltip'
import { useInstallNotice } from '@/lib/install-notice'
import { initTheme } from '@/state/theme'

import App from './App.tsx'
import './index.css'

initTheme()
blockWebviewMenu()

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <FlareProvider
      app={{ name: 'Flare Navgen', version: __APP_VERSION__, releaseNotes: __APP_RELEASE_NOTES__ }}
      useInstallNotice={useInstallNotice}
    >
      <TooltipProvider delayDuration={400}>
        <App />
      </TooltipProvider>
    </FlareProvider>
  </StrictMode>,
)
