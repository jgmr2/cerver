# dbfiller-web (backend)

Backend HTTP de la interfaz web de `dbfiller` — sirve la API JSON bajo
`/api/*` y el build de Svelte de `../frontend/` como estático en `/`.
Construido con el mismo motor de `cerver` (io_uring, thread-per-core),
bootstrapeado una vez con `dbfiller --scaffold` y después recortado a lo
que esta herramienta realmente necesita.

## Estructura

```
src/
  main.c              punto de entrada (arranca un hilo worker por nucleo)
  core/               motor HTTP (io_uring, aceptar/leer/escribir)
  config/db.c         pool de conexiones del framework -- INACTIVO aca a
                       proposito (ver el comentario grande en init_db()):
                       dbfiller-web se conecta a bases arbitrarias en
                       runtime via libpq directo (controllers/dbfiller_state.c),
                       no via el pool de negocio del framework
  routes/index.h       tabla de rutas + mount estatico de ../public
  utils/               motor HTTP/JSON/auth compartido (vendorizado, ver
                       Créditos abajo) -- utils/auth/jwt.* se mantiene
                       porque utils/http/router.h depende de el aunque
                       ninguna ruta de dbfiller-web use *_auth()
  controllers/
    dbfiller_state.*    estado global (conexion Postgres activa, raiz del
                       repo destino, mutex -- el motor es thread-per-core,
                       este estado es intencionalmente compartido)
    dbfiller_jobs.*     comandos largos en background + polling (docker
                       build/up, `docker run ... psql`, `ab`) -- cerver no
                       soporta streaming, ver el comentario grande ahi
    dbfiller_connection.*  Postgres de prueba desechable, conectar, listar bases
    dbfiller_schema.*      esquemas .sql en <repo_root>/db/init/
    dbfiller_project.*     raiz del repo, scaffolding de proyecto nuevo, .env
    dbfiller_tables.*      (en progreso) listar tablas + generar CRUD
    dbfiller_json.h, dbfiller_shell.h   helpers compartidos (escape JSON,
                       quoting de shell)
    home.h              healthz + 404 generico
public/                 build de Svelte (frontend/), gitignored -- se
                       genera con `npm run build` en ../frontend
```

Docker/Makefile/README quedan fuera de `src/` a propósito: no son
código fuente del backend, son herramientas de build/despliegue.

## Por qué corre en contenedor con el socket de Docker montado

dbfiller-web necesita invocar `docker`/`docker compose`/`ab` contra el
HOST (para levantar el Postgres de prueba, compilar/subir el proyecto
destino, correr pruebas de carga) — ver `docker-compose.yml`:
Docker-outside-of-Docker, con el socket del host montado y
`network_mode: host`. Da acceso root sobre el host a quien pueda hablarle
a ese socket; aceptable en una herramienta de desarrollo local de un solo
usuario, no en un servicio expuesto a otros.

## Puesta en marcha

```bash
docker compose up -d --build
curl http://localhost:8090/healthz
```

Sin `.env` ni variables obligatorias — `PORT` tiene default (`8090`, ver
`docker-compose.yml`). A diferencia de un proyecto cerver generado,
`DATABASE_URL`/`JWT_SECRET` no aplican aca (ver `config/db.c`, `init_db()`).

## Créditos de terceros

- [picohttpparser](https://github.com/h2o/picohttpparser) — Kazuho Oku, Tokuhiro Matsuno, Daisuke Murase, Shigeo Mitsunari (MIT).
- [jsmn](https://github.com/zserge/jsmn) — Serge Zaitsev (MIT), variante minificada en `src/utils/json.h`.
