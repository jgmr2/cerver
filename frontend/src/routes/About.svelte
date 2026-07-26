<section class="prose">
  <h2>Acerca de este proyecto</h2>

  <p>
    Este backend corre sobre <strong>cerver</strong>, un servidor HTTP en C
    sobre <code>io_uring</code> pensado para exponer una API JSON sobre
    Postgres con el mínimo footprint posible — no es un framework de
    propósito general, es la base para desplegar una instancia aislada
    por cliente en infraestructura chica (AWS barata).
  </p>

  <p>
    Este proyecto puntual es un sistema de <strong>gestión de
    inventario</strong>: ítems, categorías, ubicaciones, reportes de
    incidencias y solicitudes, con roles y departamentos. Su
    autenticación no está hardcodeada — se generó a partir de la tabla
    <code>"user"</code> del esquema, detectada automáticamente porque
    cumple el criterio de tabla de autenticación (email único +
    contraseña + rol).
  </p>

  <h3>Stack</h3>
  <ul>
    <li><strong>nginx</strong> — única vía de entrada, sirve este frontend y hace de reverse proxy hacia la API.</li>
    <li><strong>backend</strong> — el servidor C, sin ORM ni framework, thread-per-core.</li>
    <li><strong>Postgres</strong> — la base de datos.</li>
    <li><strong>frontend</strong> — esto que estás viendo, Svelte + Vite, sin frameworks de UI.</li>
  </ul>

  <p>
    Para el detalle técnico completo de cómo funciona cada pieza, ver la
    <a href="#/wiki">Wiki</a>.
  </p>
</section>
