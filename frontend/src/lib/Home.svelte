<script>
  let state = $state({ status: 'loading', data: null, error: null });

  fetch('/api')
    .then((r) => r.json())
    .then((data) => (state = { status: 'ok', data, error: null }))
    .catch((err) => (state = { status: 'error', data: null, error: String(err) }));
</script>

<h2>Home</h2>
<p>Consume <code>GET /api</code> (timestamp crudo de Postgres via libpq binario).</p>

{#if state.status === 'loading'}
  <p>Cargando...</p>
{:else if state.status === 'error'}
  <p class="err">Error: {state.error}</p>
{:else}
  <pre>{JSON.stringify(state.data, null, 2)}</pre>
{/if}
