<script>
  import { currentPath, interceptLinks } from './lib/router.js';
  import Home from './lib/Home.svelte';
  import Films from './lib/Films.svelte';
  import Actors from './lib/Actors.svelte';
  import NotFound from './lib/NotFound.svelte';

  const routes = { '/': Home, '/films': Films, '/actors': Actors };

  let Page = $derived(routes[$currentPath] ?? NotFound);
</script>

<div use:interceptLinks>
  <nav>
    <a href="/">Home</a>
    <a href="/films">Peliculas</a>
    <a href="/actors">Actores</a>
    <a href="/esto-no-existe">Ruta inexistente (prueba 404)</a>
  </nav>

  <main>
    <Page />
  </main>
</div>
