<script>
  import { navigate } from './router.js';
</script>

<h2>404 — Pagina no encontrada</h2>
<p>
  El servidor te sirvio igual el <code>index.html</code> (fallback SPA), y este
  componente es el que decide que la ruta no existe dentro de la app.
</p>
<button onclick={() => navigate('/')}>Volver al inicio</button>
