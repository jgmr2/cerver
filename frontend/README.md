# frontend

Landing mínima (Vite + Svelte, sin SvelteKit — solo se necesita un
build estático de una SPA, sin SSR), en el estilo de la página de
bienvenida que traen otros frameworks al levantar el servicio (Rails,
Django, Laravel): una sección **"Acerca de"** y una **Wiki** con varios
temas (arquitectura, autenticación, generación de código, seguridad,
despliegue) explicando cómo está armado el proyecto — sin login ni
lógica de negocio. Reemplazar por la app real del proyecto cuando haga
falta.

- `src/routes/About.svelte` — resumen del proyecto y del stack.
- `src/routes/Wiki.svelte` — los temas de la wiki (array `topics` en el
  propio componente; agregar uno nuevo es agregar un elemento ahí).
- `src/lib/router.js` — router mínimo por `location.hash`, sin
  dependencia externa.

## Desarrollo local

```bash
cd frontend
npm install
npm run dev
```

No depende del backend para nada (contenido estático, sin `fetch`), así
que `npm run dev` alcanza solo, sin levantar el resto del stack.

## Build de producción

No hace falta correrlo a mano: `nginx/Dockerfile` construye este
proyecto (`npm ci && npm run build`) como parte de `docker compose up
--build`. Si se quiere generar el build suelto para inspeccionarlo:

```bash
cd frontend
npm install
npm run build   # deja el resultado en frontend/dist/
```
