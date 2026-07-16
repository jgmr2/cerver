<script>
  let state = $state({ status: 'loading', data: [], error: null });

  fetch('/api/sakila/films/top')
    .then((r) => r.json())
    .then((json) => (state = { status: 'ok', data: json.data ?? [], error: null }))
    .catch((err) => (state = { status: 'error', data: [], error: String(err) }));
</script>

<h2>Top peliculas</h2>
<p>Consume <code>GET /api/sakila/films/top</code>.</p>

{#if state.status === 'loading'}
  <p>Cargando...</p>
{:else if state.status === 'error'}
  <p class="err">Error: {state.error}</p>
{:else}
  <table>
    <thead>
      <tr><th>Titulo</th><th>Duracion</th><th>Año</th><th>Rating</th></tr>
    </thead>
    <tbody>
      {#each state.data as film}
        <tr>
          <td>{film.title}</td>
          <td>{film.length} min</td>
          <td>{film.release_year}</td>
          <td>{film.rating}</td>
        </tr>
      {/each}
    </tbody>
  </table>
{/if}
