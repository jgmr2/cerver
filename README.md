# cerver

Backend HTTP en C sobre `io_uring`, para exponer una API JSON sobre Postgres con footprint mínimo. Pensado para desplegar una instancia aislada por cliente en infraestructura barata (AWS chico) — no es un framework de propósito general.

## Arquitectura

Tres servicios (`docker-compose.yml`):

- **`nginx`** — única vía de entrada, único puerto expuesto (`80`). Sirve el build estático de `frontend/` (SPA, con fallback a `index.html`) y hace reverse proxy de `/api`, `/healthz` y `/docs` al backend.
- **`backend`** — el servidor C. Thread-per-core (`SO_REUSEPORT`), sin locks entre hilos; todo I/O (red, Postgres) pasa por `io_uring`, sin ninguna syscall bloqueante en el camino caliente. Sin ORM: el SQL vive a mano, envuelto para que Postgres arme el JSON final (`row_to_json`/`json_agg`). Sin puerto propio expuesto al host.
- **`db`** — Postgres.

El código que se replica igual en cada proyecto nuevo vive en `src/` (`core/`, `config/`, `utils/`, `main.c`, `Makefile`). Las tablas de negocio de cada proyecto se agregan en `db/init/*.sql`.

Este repo es la plantilla: cada cliente es su propio fork (ver [`FORKING.md`](FORKING.md) para crear uno nuevo y traer actualizaciones de acá después; [`CHANGELOG.md`](CHANGELOG.md) para qué trae cada checkpoint).

## Generación de código (`tools/dbfiller`)

[`tools/dbfiller/generate.py`](tools/dbfiller/README.md) lee `db/init/*.sql` y genera, por cada tabla:

- CRUD completo (`src/controllers/`, `src/models/`) — list/get/create/update/delete, rutas registradas en `src/routes/index.h`.
- Documentación Swagger (`docs-ui/openapi.yaml`), si ese archivo existe en el repo.

Es un paso manual (no corre en el build de Docker): se corre, se revisa el diff, se commitea. El resultado se puede editar a mano después para agregar lógica de negocio — cada archivo generado guarda un hash de su contenido, así que si lo modificás y volvés a correr `generate.py`, se frena y pide `--force` en vez de pisar la edición en silencio.

## Autenticación (derivada del esquema, no hardcodeada)

`tools/dbfiller` busca en el esquema una tabla con PK de una sola columna, una columna `email`/`username` única y `NOT NULL`, y una columna tipo password — y genera `POST /api/auth/register`, `POST /api/auth/login` y `GET /api/me` a partir de ella (JWT HS256, PBKDF2 con sal, comparación en tiempo constante). No hay ningún nombre de tabla ni de columna hardcodeado en el motor: funciona igual con cualquier esquema que cumpla ese criterio. Detalle completo en [`tools/dbfiller/README.md`](tools/dbfiller/README.md#autenticación).

## Seguridad incluida

- Rate-limit de intentos de login fallidos por IP (`LOGIN_MAX_ATTEMPTS`/`LOGIN_WINDOW_SECONDS`) y límite de conexiones concurrentes, global y por IP (`MAX_CONNECTIONS`/`MAX_CONN_PER_IP`) — ver `src/utils/auth/login_limit.h`, `src/utils/net/conn_limit.h`.
- Guardrail contra IDOR (`route_require_owner`, `src/utils/http/router.h`) — plantilla para cualquier endpoint que devuelva un recurso de un usuario puntual.
- Perfil de seccomp propio (`seccomp-cerver.json`) en vez de `unconfined`.
- `X-Forwarded-For`/`X-Real-IP` confiados (`TRUST_PROXY_HEADERS=1`, default) solo porque nginx es la única vía de entrada al backend — ver comentarios en `docker-compose.yml`.
- Apagado graceful (`SIGTERM`/`SIGINT`), reconexión automática a Postgres si la DB se cae y vuelve, healthcheck separado del chequeo de DB.

Ver [`TODO.md`](TODO.md) para lo que falta antes de exponer esto a tráfico real.

## Puesta en marcha

```bash
# crear .env en la raiz con POSTGRES_USER, POSTGRES_PASSWORD, POSTGRES_DB,
# DATABASE_URL y JWT_SECRET (el proceso no arranca si falta DATABASE_URL o JWT_SECRET)
docker compose up -d
curl http://localhost/healthz
```

Todo lo demás es configurable por variable de entorno, con default razonable si no se define — `PORT`, `SHUTDOWN_GRACE_SECONDS`, `CACHE_REFRESH_SECONDS`, `DB_CONNECT_TIMEOUT_SECONDS`, `DB_STARTUP_RETRY_ATTEMPTS`, `DB_STARTUP_RETRY_DELAY_SECONDS`, `JWT_EXPIRES_SECONDS`, `PBKDF2_ITERATIONS`, `DB_POOL_SIZE`, `DB_PENDING_QUEUE_SIZE`, `MAX_CONNECTIONS`, `MAX_CONN_PER_IP`, `LOGIN_MAX_ATTEMPTS`, `LOGIN_WINDOW_SECONDS`, `TRUST_PROXY_HEADERS`, `BACKEND_TARGET` (ver `docker-compose.yml` y `.env`) — nada de esto requiere recompilar.

`MAX_CONN_PER_IP` se deja cerca de `MAX_CONNECTIONS` a propósito: con nginx como única entrada, todas las conexiones le llegan al backend con la IP de nginx (el límite por conexión actúa al aceptar el TCP, antes de que exista ningún header HTTP), así que no puede discriminar clientes reales — un límite real por cliente se configura en nginx (`limit_conn`/`limit_req`), no acá. `LOGIN_MAX_ATTEMPTS` sí puede ser preciso por cliente real detrás del proxy, vía `TRUST_PROXY_HEADERS=1` (default) leyendo `X-Forwarded-For`.

**SYN flood**: no se mitiga en la app — es un ataque contra el handshake TCP, antes de que un byte llegue al proceso. Se asume mitigado por AWS Shield Standard (automático en toda IP pública de EC2) y `tcp_syncookies=1` del host (verificar en la AMI real antes de desplegar).

## TLS

Por default nginx solo escucha en `:80` (sin TLS) — pensado así porque activar HTTPS es una decisión por instancia (necesita el dominio real del cliente apuntando a esa instancia, para poder emitir el certificado). La capacidad ya está lista, sin activar: ver [`nginx/TLS.md`](nginx/TLS.md) para activarla en una instancia puntual (Let's Encrypt vía `certbot`, con renovación automática) o para probar HTTPS en local con un certificado autofirmado.

## Build de producción (sin Swagger UI)

Por default se construye el target `runtime` (con `/docs`, Swagger UI), pensado para desarrollo. Para el stack sin esos estáticos:

```bash
BACKEND_TARGET=runtime-prod docker compose up -d --build
```

`/docs` sigue registrado en el binario (mismo binario, no hay flag de compilación) pero devuelve 404 porque `docs-ui/` no está copiado en esa imagen.

## Tests

```bash
# unitarios (funciones puras del router/estáticos) — necesita liburing-dev,
# libpq-dev, libssl-dev instalados (o correrlo dentro de un contenedor Alpine)
make -C tests/unit test

# integración, contra el stack real (requiere docker compose up -d primero)
python3 tests/integration/test_integration.py
```

El workflow de CI (`.github/workflows/docker-image.yml`) corre ambos antes de publicar la imagen.

## Créditos de terceros

- [picohttpparser](https://github.com/h2o/picohttpparser) — Kazuho Oku, Tokuhiro Matsuno, Daisuke Murase, Shigeo Mitsunari (MIT).
- [jsmn](https://github.com/zserge/jsmn) — Serge Zaitsev (MIT), variante minificada en `src/utils/json.h`.
- [Swagger UI](https://swagger.io/tools/swagger-ui/) — SmartBear Software (Apache 2.0), vendorizado en `docs-ui/`.
