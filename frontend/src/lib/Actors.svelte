<script>
  let state = $state({ status: 'loading', data: [], error: null });

  fetch('/api/sakila/actors/top')
    .then((r) => r.json())
    .then((json) => (state = { status: 'ok', data: json.data ?? [], error: null }))
    .catch((err) => (state = { status: 'error', data: [], error: String(err) }));
</script>

<h2>Top actores</h2>
<p>Consume <code>GET /api/sakila/actors/top</code>.</p>

{#if state.status === 'loading'}
  <p>Cargando...</p>
{:else if state.status === 'error'}
  <p class="err">Error: {state.error}</p>
{:else}
  <table>
    <thead>
      <tr><th>Nombre</th><th>Apellido</th><th># Peliculas</th></tr>
    </thead>
    <tbody>
      {#each state.data as actor}
        <tr>
          <td>{actor.first_name}</td>
          <td>{actor.last_name}</td>
          <td>{actor.films}</td>
        </tr>
      {/each}
    </tbody>
  </table>
{/if}
