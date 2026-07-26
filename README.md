# cerver

Servidor HTTP en C puro sobre `io_uring`, sin framework, para exponer APIs JSON sobre Postgres.

Nace para un caso de uso concreto: backends a medida para pequeñas empresas, corriendo en instancias de AWS baratas. Ahí el costo por MB de RAM y por vCPU importa más que exprimir el último 10% de throughput — un binario estático de unos cientos de KB permite meter muchos más clientes por instancia que un runtime de JVM o Node.

## Objetivos

- **Footprint mínimo**: binario estático, sin runtime pesado, pensado para correr varias instancias del backend (una por cliente) en la misma máquina.
- **Sin bloqueo, nunca**: todo I/O (red, disco, Postgres) pasa por `io_uring`; ningún hilo worker espera una syscall bloqueante.
- **Sin framework, sin ORM**: menos capas que auditar. El SQL vive a mano, envuelto para que Postgres arme el JSON final (`row_to_json`/`json_agg`).
- **El motor se replica, el negocio no**: todo el código vive en `src/` (`src/core/`, `src/config/`, `src/utils/` son genéricos y se copian igual entre proyectos); cada API nueva solo agrega archivos en `src/models/`, `src/controllers/` y `src/routes/` — normalmente generados por [`tools/dbfiller`](tools/dbfiller/README.md) a partir de `db/init/*.sql`, no escritos a mano.

## Qué incluye hoy

- Servidor HTTP/1.1 thread-per-core (`SO_REUSEPORT`), sin locks entre hilos.
- Pool de conexiones a Postgres 100% asíncrono, con reconexión automática si la DB se cae y vuelve.
- Autenticación JWT (HS256) propia — registro, login, rutas protegidas, derivada del esquema (ver `tools/dbfiller/README.md`, sección "Autenticación").
- Servidor de estáticos con fallback SPA, protegido contra path traversal (lo usa `nginx/`, no el backend — ver más abajo).
- nginx delante (`nginx/`) — única vía de entrada: sirve el build de `frontend/` (placeholder Vite+Svelte) y reverse-proxea `/api`, `/healthz`, `/docs` al backend.
- Documentación interactiva de la API en `/docs` (Swagger UI) — opcional en producción, ver "Build de producción" abajo.
- Apagado graceful, healthcheck separado del chequeo de DB, headers de seguridad, mitigación de slowloris y de HTTP request smuggling.
- Perfil de seccomp propio (`seccomp-cerver.json`, ver `TODO.md`) — default de Docker más `io_uring_*`, no `unconfined`.

## Puesta en marcha

```bash
# crear .env en la raiz con POSTGRES_USER, POSTGRES_PASSWORD, POSTGRES_DB,
# DATABASE_URL y JWT_SECRET (el proceso no arranca si falta DATABASE_URL o JWT_SECRET)
docker compose up -d
curl http://localhost/healthz
```

`docker-compose.yml` levanta tres servicios: `db` (Postgres), `backend`
(sin puerto propio expuesto al host) y `nginx` (único servicio con
puerto público, `80:80`) — ver "nginx delante" abajo.

Todo lo demás es configurable por variable de entorno con un default razonable si no se define — `PORT`, `SHUTDOWN_GRACE_SECONDS`, `CACHE_REFRESH_SECONDS`, `DB_CONNECT_TIMEOUT_SECONDS`, `DB_STARTUP_RETRY_ATTEMPTS`, `DB_STARTUP_RETRY_DELAY_SECONDS`, `JWT_EXPIRES_SECONDS`, `PBKDF2_ITERATIONS`, `DB_POOL_SIZE`, `DB_PENDING_QUEUE_SIZE`, `MAX_CONNECTIONS`, `MAX_CONN_PER_IP`, `TRUST_PROXY_HEADERS` (ver `docker-compose.yml` y `.env`) — nada de esto requiere recompilar para ajustarlo por deployment.

`MAX_CONNECTIONS` (default 4096) y `MAX_CONN_PER_IP` (default 4096 en `docker-compose.yml`, pensando en nginx delante) limitan conexiones TCP concurrentes, global y por IP de origen, para mitigar agotamiento de conexiones (CWE-770) — ver `src/utils/net/conn_limit.h`. Como todas las conexiones le llegan al backend con la IP de nginx (`conn_limit` actúa al aceptar la conexión TCP, antes de que existan headers HTTP), `MAX_CONN_PER_IP` no puede discriminar por cliente real detrás de un proxy — se deja cerca de `MAX_CONNECTIONS` a propósito; un límite real por cliente se configura en nginx (`limit_conn`/`limit_req`), no acá.

`LOGIN_MAX_ATTEMPTS` (default 10) y `LOGIN_WINDOW_SECONDS` (default 60) limitan intentos fallidos de login por IP en `POST /api/auth/login`, para mitigar fuerza bruta/credential stuffing — ver `src/utils/auth/login_limit.h`. A diferencia de `MAX_CONN_PER_IP`, esto sí puede ser preciso detrás de un proxy: con `TRUST_PROXY_HEADERS=1` (default en `docker-compose.yml`), `login_user` lee la IP real del cliente de `X-Forwarded-For`/`X-Real-IP` (ver `extract_forwarded_ip()`, `src/utils/http/router.h`) en vez de la IP del socket (que sería siempre la de nginx). Solo es seguro activarlo porque el backend no tiene otra vía de entrada — si se expone su puerto en paralelo, hay que volver `TRUST_PROXY_HEADERS` a `0` (cualquiera podría falsear el header y saltarse el límite).

## nginx delante

`nginx/Dockerfile` (contexto: la raíz del repo) compila `frontend/` (placeholder Vite+Svelte, ver su propio README) y sirve el resultado como SPA, además de reverse-proxear `/api`, `/healthz` y `/docs` al backend (ver `nginx/nginx.conf`). Es la única vía de entrada — el backend no tiene puerto propio mapeado al host. Reemplazar `frontend/` por la app real no requiere tocar nada de `nginx/` ni de `src/`.

## Build de producción (sin Swagger UI)

Por default (sin nada especial) se construye el target `runtime`, con `/docs` (Swagger UI) — pensado para desarrollo. Para el stack completo sin esos estáticos, vía `docker-compose.yml` (variable `BACKEND_TARGET`, ver su servicio `backend`):

```bash
BACKEND_TARGET=runtime-prod docker compose up -d --build
```

O agregando `BACKEND_TARGET=runtime-prod` al `.env` para no repetirlo. Con `docker build` suelto, sin compose: `docker build --target runtime-prod -t cerver:prod .`.

`/docs` sigue registrado en el binario (mismo binario, no hay flag de compilación) pero devuelve 404 porque `docs-ui/` no está copiado en esa imagen — ver comentarios en el `Dockerfile`.

**SYN flood**: no se mitiga en este repo porque no se puede — es un ataque contra el handshake TCP, antes de que un solo byte llegue a `cerver`. En AWS, EC2 ya tiene protección automática y gratuita contra esto (AWS Shield Standard, activo por default en toda IP pública/Elastic IP, sin configurar nada). A nivel de kernel, `tcp_syncookies` viene en `1` por default en las distros Linux habituales (Ubuntu, Debian, Amazon Linux) — verificarlo en la AMI real (`cat /proc/sys/net/ipv4/tcp_syncookies`) antes de desplegar, no asumirlo. Deliberadamente NO se fuerza vía `sysctls:` en `docker-compose.yml`: con el `userland-proxy` de Docker activo (el default — ver el hallazgo de `MAX_CONN_PER_IP` más arriba), el socket que realmente recibe el handshake TCP público es el del **host**, no el del namespace del contenedor — un sysctl seteado ahí sería puro placebo. Si algún día se desactiva `userland-proxy` (hairpin NAT por iptables), ahí sí pasa a importar el sysctl del contenedor, y hay que revisitar esto.

## Tests

```bash
# unitarios (funciones puras del router/estáticos) — necesita liburing-dev,
# libpq-dev, libssl-dev instalados (o correrlo dentro de un contenedor
# Alpine con esas libs, ver el job "test" de docker-image.yml)
make -C tests/unit test

# integración, contra el stack real (requiere docker compose up -d primero)
python3 tests/integration/test_integration.py
```

El workflow de CI (`.github/workflows/docker-image.yml`) corre ambos antes de publicar la imagen — si fallan, no se hace `build-and-push`.

Ver `/docs` (Swagger UI) para el detalle de cada endpoint, y [TODO.md](TODO.md) para qué está resuelto y qué falta antes de usar esto en producción real.

## Créditos de terceros

- [picohttpparser](https://github.com/h2o/picohttpparser) — Kazuho Oku, Tokuhiro Matsuno, Daisuke Murase, Shigeo Mitsunari (MIT).
- [jsmn](https://github.com/zserge/jsmn) — Serge Zaitsev (MIT), variante minificada en `src/utils/json.h`.
- [Swagger UI](https://swagger.io/tools/swagger-ui/) — SmartBear Software (Apache 2.0), vendorizado en `docs-ui/`.
