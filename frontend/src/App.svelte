<script>
  // Placeholder: solo valida que nginx sirve estos estaticos y proxyea
  // /api al backend desde el mismo origen (ver nginx/nginx.conf). Sin
  // logica de negocio -- reemplazar por la app real.
  let status = 'cargando...'
  let detail = ''

  fetch('/api')
    .then((r) => r.json())
    .then((data) => {
      status = 'backend OK'
      detail = JSON.stringify(data)
    })
    .catch((err) => {
      status = 'no se pudo contactar al backend'
      detail = String(err)
    })
</script>

<main>
  <h1>cerver</h1>
  <p>Placeholder de frontend (Vite + Svelte) servido por nginx.</p>
  <p><strong>{status}</strong></p>
  <p><code>{detail}</code></p>
</main>

<style>
  main {
    font-family: system-ui, sans-serif;
    max-width: 40rem;
    margin: 4rem auto;
    padding: 0 1rem;
  }
  code {
    display: block;
    background: #f2f2f2;
    padding: 0.5rem;
    border-radius: 4px;
    word-break: break-all;
  }
</style>
