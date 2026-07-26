# frontend

Placeholder mínimo de frontend (Vite + Svelte, sin SvelteKit — solo se
necesita un build estático de una SPA, sin SSR). `App.svelte` solo hace
un `fetch('/api')` para validar que nginx sirve estos estáticos y
proxyea `/api` al backend desde el mismo origen (ver
[`nginx/nginx.conf`](../nginx/nginx.conf)) — reemplazar por la app real.

## Desarrollo local

```bash
cd frontend
npm install
npm run dev
```

`vite.config.js` ya proxyea `/api`, `/healthz` y `/docs` a
`http://localhost:8080` (el backend corriendo suelto, sin nginx en el
medio) para que `npm run dev` funcione sin tener que levantar todo el
stack de Docker.

## Build de producción

No hace falta correrlo a mano: `nginx/Dockerfile` construye este
proyecto (`npm ci && npm run build`) como parte de `docker compose up
--build`. Si se quiere generar el build suelto para inspeccionarlo:

```bash
cd frontend
npm install
npm run build   # deja el resultado en frontend/dist/
```
