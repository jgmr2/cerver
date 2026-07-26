<script>
  const topics = [
    {
      id: 'arquitectura',
      title: 'Arquitectura',
      html: `
        <p>Tres servicios, cada uno con un trabajo:</p>
        <ul>
          <li><strong>nginx</strong> — única vía de entrada (único puerto expuesto). Sirve el build de este mismo frontend como SPA (con fallback a <code>index.html</code>) y hace reverse proxy de <code>/api</code>, <code>/healthz</code> y <code>/docs</code> al backend.</li>
          <li><strong>backend</strong> — el servidor C. Thread-per-core (<code>SO_REUSEPORT</code>): cada núcleo corre su propio hilo con su propio anillo <code>io_uring</code>, sin locks entre hilos. Todo I/O (red, Postgres) es asíncrono, no bloqueante — ninguna syscall bloqueante en el camino caliente. Sin ORM: el SQL vive a mano, envuelto para que Postgres arme el JSON final (<code>row_to_json</code>/<code>json_agg</code>).</li>
          <li><strong>Postgres</strong> — la base de datos.</li>
        </ul>
        <p>El backend no tiene puerto propio expuesto al host: nginx es la única forma de llegar a él desde afuera.</p>
      `,
    },
    {
      id: 'auth',
      title: 'Autenticación',
      html: `
        <p>No hay ninguna tabla ni columna de autenticación hardcodeada en el motor. Al generar el proyecto, se busca en el esquema SQL una tabla que tenga:</p>
        <ul>
          <li>una clave primaria de una sola columna,</li>
          <li>una columna <code>email</code> o <code>username</code> única y <code>NOT NULL</code>,</li>
          <li>y una columna que parezca una contraseña (<code>password_hash</code> o similar).</li>
        </ul>
        <p>Con eso se generan <code>POST /api/auth/register</code>, <code>POST /api/auth/login</code> y <code>GET /api/me</code>. Adentro: JWT propio (HS256), contraseñas con PBKDF2 y sal, comparación en tiempo constante para no filtrar información por temporización. Además hay rate-limit de intentos de login fallidos por IP, para frenar fuerza bruta.</p>
      `,
    },
    {
      id: 'codegen',
      title: 'Generación de código',
      html: `
        <p>Una herramienta (<code>dbfiller</code>) lee el esquema SQL del proyecto y genera, por cada tabla de negocio, los endpoints CRUD completos (listar, ver, crear, editar, borrar) — sin escribirlos a mano.</p>
        <p>Es un paso manual, no algo que corra solo al compilar: se corre, se revisa el resultado, y se guarda en el proyecto como cualquier otro código. Se puede editar a mano después para agregar lógica de negocio propia — el generador se da cuenta si un archivo ya fue modificado y no lo pisa por las dudas.</p>
      `,
    },
    {
      id: 'seguridad',
      title: 'Seguridad',
      html: `
        <ul>
          <li>Límite de intentos de login fallidos por IP, y límite de conexiones concurrentes (global y por IP).</li>
          <li>Protección contra IDOR: un endpoint que devuelve datos de "tu" usuario compara el dueño real del dato contra tu sesión, nunca confía en un id que venga del cliente.</li>
          <li>Perfil de sandboxing propio del proceso (en vez de correr sin ninguna restricción).</li>
          <li>Headers de origen del cliente confiados únicamente porque nginx es la única puerta de entrada — si eso cambiara, dejarían de confiarse.</li>
        </ul>
      `,
    },
    {
      id: 'despliegue',
      title: 'Despliegue',
      html: `
        <p>El modelo pensado es una instancia chica de nube por cliente, no un servidor compartido multi-tenant — cada cliente tiene su propia copia del proyecto, su propia base, y puede personalizar su propia lógica de negocio sin afectar a los demás.</p>
        <p>HTTPS se activa por instancia cuando el cliente tiene su dominio propio apuntando a ella (certificado real, con renovación automática); antes de eso, la capacidad ya existe pero no hace falta activarla.</p>
      `,
    },
  ]

  let active = topics[0].id
  $: current = topics.find((t) => t.id === active)
</script>

<section class="wiki">
  <nav class="wiki-nav">
    {#each topics as t (t.id)}
      <button class:active={active === t.id} on:click={() => (active = t.id)}>{t.title}</button>
    {/each}
  </nav>
  <article class="prose">
    <h2>{current.title}</h2>
    {@html current.html}
  </article>
</section>
