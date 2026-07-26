import { defineConfig } from 'vite'
import { svelte } from '@sveltejs/vite-plugin-svelte'

// En produccion, nginx sirve el build de este proyecto (dist/) y
// proxyea /api, /healthz y /docs al backend (ver nginx/nginx.conf) --
// este proxy de abajo es solo para "npm run dev" local, sin nginx en el
// medio, apuntando directo al backend en docker-compose.
export default defineConfig({
  plugins: [svelte()],
  server: {
    proxy: {
      '/api': 'http://localhost:8080',
      '/healthz': 'http://localhost:8080',
      '/docs': 'http://localhost:8080',
    },
  },
})
