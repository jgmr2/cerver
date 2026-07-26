<script>
  import { route, navigate } from './lib/router.js'
  import About from './routes/About.svelte'
  import Wiki from './routes/Wiki.svelte'
</script>

<header>
  <h1>cerver</h1>
  <nav>
    <button class:active={$route !== '/wiki'} on:click={() => navigate('/about')}>Acerca de</button>
    <button class:active={$route === '/wiki'} on:click={() => navigate('/wiki')}>Wiki</button>
  </nav>
</header>

<main>
  {#if $route === '/wiki'}
    <Wiki />
  {:else}
    <About />
  {/if}
</main>
