# cerver

Boilerplate de servidor HTTP en C puro sobre `io_uring`, pensado para levantar APIs pequeñas y rápidas (JSON sobre Postgres) sin depender de un framework. La idea es que el motor (`core/`, `utils/`) se replique tal cual entre proyectos, y que cada API nueva solo agregue archivos en `models/`, `controllers/` y `routes/`.

## Qué es y qué no es

Es:
- Un servidor HTTP/1.1 mono-binario, estático, que atiende cientos de miles de requests por segundo por núcleo sin bloquear nunca un hilo en una syscall.
- Una capa de acceso a Postgres 100% asíncrona (sin threads dedicados a la DB, sin bloqueo) con un pool de conexiones y un registro genérico de prepared statements.
- Un patrón para exponer datos de Postgres como JSON sin ORM: las consultas arman el JSON directamente en SQL (`row_to_json`/`json_agg`), y el C solo reenvía el texto.
- Un servidor de archivos estáticos con fallback SPA (para servir un frontend tipo Svelte/React ya buildeado) y protección real contra path traversal.

No es:
- Un framework con routing por parámetros de path (`/users/:id`), middlewares, ni sesiones — el router actual matchea método+path exacto (ver [Limitaciones](#decisiones-de-diseño-y-limitaciones-conocidas)).
- Un ORM: no hay mapeo de structs, migraciones ni query builder. El SQL vive a mano en cada modelo.

## Arquitectura en una frase

Un proceso, un hilo por núcleo de CPU (`SO_REUSEPORT`), cada hilo con su propio anillo `io_uring`, su propio pool de conexiones Postgres y su propia tabla de rutas — sin locks ni memoria compartida entre hilos.

## Estructura del proyecto

```
main.c                  Arranca un worker_loop() por nucleo
core/
  server.c/.h           Bucle de eventos io_uring: accept, read inicial, dispatch
config/
  db.h/.c               Pool de conexiones Postgres + registro generico de prepared statements
models/
  registry.h            Agrega register_models(); un modelo nuevo = una linea aca
  sakila.h/.c            Ejemplo de modelo: SQL propio + registro de sus prepared statements
  homeModel.h            SQL del endpoint /api de ejemplo
  users.h/.c              Lookup/creacion de usuarios (queries parametrizadas), ver Autenticacion
controllers/
  sakila.h/.c            Handlers HTTP que llaman al modelo y reenvian su JSON
  home.h                 Handler de ejemplo (health-check) y error404
  auth.h/.c               register/login/me, ver Autenticacion
routes/
  index.h                Tabla de rutas: init_routes(), refresh_caches()
utils/
  events.h               Tipos de evento del anillo io_uring, g_shutdown
  json.h                 Parser JSON minimo (variante de jsmn) para bodies de request
  auth/
    password.h/.c         Hashing de contrasenas (PBKDF2-HMAC-SHA256)
    jwt.h/.c               Creacion/verificacion de JWT (HS256)
  http/
    http.h/.c            Respuestas HTTP, servido de archivos async, keep-alive, JSON body
    router.h/.c            add_route()/dispatch(), macros get()/post()/..._auth, parametros de path
    static.h               Mount de directorios estaticos + fallback SPA
    mime.h                  Content-Type por extension
    picohttpparser.c/.h    Parser HTTP/1.x de terceros (MIT, no modificado)
db/
  init/                  Scripts que carga Postgres al primer arranque (auth + schema Sakila + indices)
  sakila/                Schema y datos de Sakila (dataset de ejemplo)
Dockerfile               Build multi-stage: libpq estatico -> binario estatico -> imagen scratch
docker-compose.yml        db (Postgres) + backend, para desarrollo local
Makefile                  Build nativo (fuera de Docker)
```

## Requisitos

- Linux con kernel >= 5.15 (io_uring razonablemente completo) y `liburing` instalada.
- Docker + Docker Compose (forma recomendada de correrlo, incluye Postgres con Sakila precargado).
- Para compilar fuera de Docker: `gcc`, `liburing-dev`, `postgresql-dev`/`libpq-dev`.

## Puesta en marcha rápida

1. Crear `.env` en la raíz (no se trackea en git):
   ```
   POSTGRES_USER=sakila
   POSTGRES_PASSWORD=<elegi una contraseña>
   POSTGRES_DB=sakila
   DATABASE_URL=postgresql://sakila:<la misma contraseña>@/sakila?host=/var/run/postgresql
   ```
   `DATABASE_URL` usa un Unix Domain Socket (`host=/var/run/postgresql`) compartido entre los contenedores `db` y `backend` vía el volumen `pg_socket` — no hay `db:5432` porque no hace falta exponer TCP entre ellos.

2. Levantar todo:
   ```bash
   docker compose up -d
   ```
   La primera vez, Postgres carga el schema y los datos de Sakila (`db/init/`, `db/sakila/`) automáticamente. Si necesitás recargarlos desde cero:
   ```bash
   docker compose down -v
   docker compose up -d
   ```

3. Probar:
   ```bash
   curl http://localhost:8080/api
   curl http://localhost:8080/api/sakila/films/top
   curl http://localhost:8080/api/sakila/actors/top
   ```

## Variables de entorno

| Variable | Usada por | Descripción |
|---|---|---|
| `POSTGRES_USER` / `POSTGRES_PASSWORD` / `POSTGRES_DB` | contenedor `db` | Credenciales con las que Postgres inicializa la base. |
| `DATABASE_URL` | `config/db.c:init_db()` | Cadena de conexión libpq completa. Si falta, el proceso hace `exit(1)` al arrancar. |
| `JWT_SECRET` | `utils/auth/jwt.c`, `main.c` | Secreto HS256 para firmar/verificar JWT (ver [Autenticación](#autenticación)). Si falta, el proceso hace `exit(1)` al arrancar. Generar uno por entorno con `openssl rand -hex 32`, nunca reusar el de `.env` (es solo para desarrollo local). |
| `PORT` | `main.c`, `core/server.c` | Puerto HTTP de escucha. Default `8080` si falta o no es un puerto válido (1-65535). |

## Compilar fuera de Docker

```bash
make          # build de release (-O3), binario ./app
make debug    # con -g, sin -O3, para gdb
make clean
```

El `Makefile` descubre todos los `.c` del árbol con `find` y agrega automáticamente cada carpeta con headers a `-I`, así que un modelo/controlador nuevo no requiere tocar el `Makefile`.

## Manual técnico

> Este manual explica **cómo funciona** el motor. Para las reglas de
> concurrencia que hay que respetar si vas a **modificarlo** (ownership
> de structs, por qué `__thread` a veces necesita `extern`, el patrón de
> timeout con `io_uring_prep_link_timeout`, etc.), ver [CONCURRENCY.md](CONCURRENCY.md).

### Ciclo de vida de un request

1. `main()` llama una vez a `register_models()` (puebla el registro global de prepared statements) y lanza un `worker_loop()` por núcleo.
2. Cada `worker_loop()` (`core/server.c`) abre su propio socket con `SO_REUSEPORT`, su anillo `io_uring`, llama a `init_db()` (conecta su pool y prepara los statements registrados) y a `init_routes()` (llena su tabla de rutas), y entra en un loop `io_uring_submit_and_wait`.
3. Una conexión aceptada arma un `read` con un `link_timeout` de 6s (`_rearm_read`, `utils/http/http.h`) — un cliente que no manda nada en ese plazo se descarta (mitiga slowloris).
4. El request se parsea con `picohttpparser`. Si trae `Content-Length` **y** `Transfer-Encoding` a la vez, se rechaza con 400 (mitiga HTTP request smuggling). Se determina keep-alive según versión HTTP y header `Connection`.
5. `dispatch()` (`utils/http/router.h`) busca método+path exacto en la tabla de rutas; si no matchea y el path es `/api/...`, 404 directo; si no, intenta servir un archivo estático con fallback SPA; si nada aplica, 404.
6. El handler dispara una consulta async (`db_query_*_async`) y retorna sin bloquear. Cuando Postgres responde, el CQE de poll llega a `handle_db_cqe` (`config/db.c`), que invoca el callback del handler con el `PGresult`.
7. El callback arma la respuesta y la manda con `send_res`/`send_json` (`utils/http/http.h`), que arma la escritura como `io_uring` writev y sigue en la misma conexión (keep-alive) o la cierra.

### Capa de datos: sin ORM, con JSON armado en SQL

`config/db.h`/`.c` son **genéricos**: solo pool de conexiones (`S=16` por hilo), cola de espera cuando el pool está lleno, y un registro de prepared statements (`db_register_prepared`) que no sabe nada de ninguna tabla.

Cada modelo (ver `models/sakila.c`) es dueño de:
- El SQL, ya envuelto para que Postgres devuelva el JSON final:
  ```sql
  SELECT COALESCE(json_agg(row_to_json(t)), '[]'::json) FROM (
      SELECT title, length, release_year, rating FROM film ...
  ) t;
  ```
- Su registro (`sakila_register()`, llamado una sola vez desde `main.c` vía `models/registry.h`, **antes** de crear los hilos — el registro es un arreglo global, no `__thread`).

El controlador (`controllers/sakila.c`) no decodifica columnas ni escapa texto: el `PGresult` trae una sola fila con una sola columna de texto (el JSON ya armado), y el controlador solo la envuelve en `{"data": ...}` y la reenvía. Se evaluó adoptar un ORM de C existente y se descartó: las opciones disponibles (p.ej. `c-orm`) usan llamadas bloqueantes de libpq, incompatibles con el modelo sin-bloqueo de este servidor.

### Servidor de estáticos

`utils/http/static.h` registra directorios servibles (`mount_static("/", "./public", 1)` en `routes/index.h`) contra un file descriptor `O_PATH` fijado una sola vez al arrancar el hilo. Cada archivo se abre con `openat2` + `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS`, que hace que el kernel rechace de forma atómica cualquier resolución que intente escapar del directorio raíz (sin el TOCTOU de resolver el path y abrirlo por separado). Con `spa_fallback=1`, un archivo sin extensión que no existe cae a `index.html` (para que un router client-side resuelva la ruta); un path con extensión que no existe es 404 real.

### Seguridad ya incluida

- Headers en toda respuesta: `X-Content-Type-Options`, `X-Frame-Options`, `Referrer-Policy` (`SECURITY_HEADERS`, `utils/http/http.h`).
- Rechazo de `Content-Length` + `Transfer-Encoding` simultáneos (request smuggling).
- `keep-alive` respetado según lo que pide el cliente (no asumido siempre, ver el comentario en `conn_keep_alive`).
- Timeout de 6s en el read inicial de cada conexión (slowloris).
- `openat2(RESOLVE_BENEATH)` para estáticos, más rechazo explícito de segmentos que empiezan con `.` (dotfiles).
- `/api` reservado: nunca cae en el fallback SPA, así un typo en un endpoint da 404 real y no un `200` con `index.html`.
- Autenticación JWT propia (HS256) para rutas que la necesiten, ver [Autenticación](#autenticación).

## Autenticación

`utils/auth/` implementa JWT (HS256) y hashing de contraseñas (PBKDF2-HMAC-SHA256) sin dependencias nuevas — todo sobre OpenSSL, ya enlazado.

**Flujo:**
```
POST /api/auth/register  {"username": "...", "password": "..."}  -> {"token": "..."}
POST /api/auth/login     {"username": "...", "password": "..."}  -> {"token": "..."}
GET  /api/me              Authorization: Bearer <token>           -> {"sub": "...", "username": "..."}
```

- `username`: 3-32 caracteres, alfanumérico + `_`/`-`. `password`: mínimo 8 caracteres.
- El token dura 24hs (`JWT_EXPIRES_SECONDS`, `controllers/auth.c`) — ajustar según la política real que necesite el proyecto.
- Login devuelve `401` tanto si el usuario no existe como si la contraseña es incorrecta (misma respuesta en ambos casos, para no revelar qué usernames existen).

**Para proteger un endpoint nuevo**, registrarlo con `get_auth()`/`post_auth()`/`put_auth()`/`patch_auth()`/`del_auth()` en vez de `get()`/`post()`/etc. (`routes/index.h`):

```c
get_auth("/api/mis-datos", get_mis_datos);
```

`dispatch()` (`utils/http/router.h`) valida el JWT antes de invocar el handler — si falta o es inválido/venció, responde `401` directo, el handler ni se llama. Dentro del handler, una vez ahí, el token ya es válido:

```c
const char *user_id = jwt_claim("sub");
const char *username = jwt_claim("username");
```

**Decisiones que quedan afuera a propósito** (ver `TODO.md`): no hay roles/permisos (todo lo protegido es "logueado sí o no"), y no hay revocación de tokens (un JWT válido lo es hasta que expira — comportamiento esperable de JWT stateless, pero sin logout real del lado servidor).

## Cómo agregar un endpoint nuevo

Este es el patrón a replicar para cada endpoint nuevo (el propósito de este boilerplate):

**1. Modelo** (`models/<dominio>.c`) — el SQL y su registro:
```c
#include "midominio.h"

#define STMT_MI_QUERY "mi_query"

void midominio_register(void) {
    db_register_prepared(STMT_MI_QUERY,
        "SELECT COALESCE(json_agg(row_to_json(t)), '[]'::json) FROM ("
        "SELECT col1, col2 FROM mi_tabla WHERE ... LIMIT 10"
        ") t;");
}

void MiDominio_get_algo_async(struct io_uring *r, int fd, cb callback) {
    db_query_prepared_async(r, fd, STMT_MI_QUERY, callback);
}
```

**2. Registrar el modelo** en `models/registry.h`:
```c
#include "../models/midominio.h"
static inline void register_models(void) {
    sakila_register();
    midominio_register();   // <- nueva linea
}
```

**3. Controlador** (`controllers/<dominio>.c`) — reenvía el JSON:
```c
static int on_algo_fetched(struct io_uring *r, int fd, PGresult *res) {
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {
        res_json(r, fd, "{\"error\":\"query failed\"}");
        return 0;
    }
    char body[16384];
    snprintf(body, sizeof(body), "{\"data\":%s}", PQgetvalue(res, 0, 0));
    res_json(r, fd, body);
    return 0;
}

void get_algo(struct io_uring *r, int fd, const char *body, const char *buf) {
    (void)body; (void)buf;
    MiDominio_get_algo_async(r, fd, on_algo_fetched);
}
```

**4. Ruta** en `routes/index.h`:
```c
#include "../controllers/midominio.h"
// dentro de init_routes():
get("/api/midominio/algo", get_algo);
```

No hace falta tocar `config/db.*`, `core/server.*` ni el `Makefile`.

## Despliegue

`Dockerfile` es multi-stage: compila `libpq`/`libpgcommon`/`libpgport` estáticos desde el source de Postgres 16.2 en Alpine (etapa `pg-static-deps`), compila el binario del server estático contra esas libs (etapa `build`, corre `make`), y copia solo el binario + `/public` a una imagen `FROM scratch` (etapa `runtime`) — sin shell, sin libc dinámica, imagen final de unos cientos de KB.

`.github/workflows/docker-image.yml` hace build+push a Docker Hub en cada push a `main` (requiere los secrets `DOCKERHUB_USERNAME`/`DOCKERHUB_TOKEN` configurados en el repo de GitHub).

## Decisiones de diseño y limitaciones conocidas

- **Sin routing por parámetros de path.** `dispatch()` matchea método+path exacto (`utils/http/router.h`); no hay `/users/:id`. Para IDs dinámicos hoy la opción es leerlos del body/query string dentro del handler, o extender `dispatch()`.
- **Sin ORM ni query builder a propósito.** El SQL vive a mano en cada modelo, envuelto en `row_to_json`/`json_agg` para que Postgres arme el JSON. Evaluado y descartado adoptar un ORM de C existente por incompatibilidad con el modelo sin-bloqueo (ver arriba).
- **Registro de prepared statements es global, no por-request.** Todo modelo debe registrarse antes de que arranque el primer hilo (`register_models()` en `main.c`), nunca desde dentro de un handler.
- **Puerto 8080 hardcodeado**, no configurable por entorno todavía.
- **`utils/http/picohttpparser.c/.h`** es código de terceros (Kazuho Oku et al., licencia MIT) — no se modifica ni se documenta línea por línea, se usa tal cual.
- **`utils/json.h`** es una variante minificada de `jsmn` (Serge Zaitsev, MIT), usada para parsear el body JSON de un request entrante (no de las respuestas, que ya vienen armadas desde Postgres).

## Créditos de terceros

- [picohttpparser](https://github.com/h2o/picohttpparser) — Kazuho Oku, Tokuhiro Matsuno, Daisuke Murase, Shigeo Mitsunari (MIT).
- [jsmn](https://github.com/zserge/jsmn) — Serge Zaitsev (MIT), variante minificada en `utils/json.h`.
