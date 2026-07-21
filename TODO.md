# TODO — cimientos de cerver

Lista de puntos a revisar antes de apoyar trabajo de negocio real sobre este
boilerplate. Lo resuelto queda tachado y resumido en una línea — el detalle
completo de qué se probó y cómo sigue en el historial de git de este archivo.

## 🔴 Pendientes críticos

- [x] ~~**Rotar credenciales de desarrollo local** — `POSTGRES_PASSWORD`
  (era `12345`) y `JWT_SECRET` regenerados (`openssl rand`), aplicados al
  `.env` y a la Postgres ya corriendo (`ALTER USER`, no solo el archivo).
  Probado end-to-end: `/healthz`, `/api`, registro y login funcionando
  con las credenciales nuevas.~~

- [x] ~~**Testing: de cero a un mínimo viable real.** `tests/unit/` — 26
  tests de las funciones puras (`url_decode_path`, `path_has_hidden_segment`,
  `path_looks_like_asset`, `mime_type_for_path`, `path_is_api`), incluyendo
  las cabeceras reales del motor, no una reimplementación. `tests/integration/`
  — 23 tests contra el stack real (Docker + Postgres): flujo completo de
  auth, guardrail IDOR, rate-limit de login, `413`/`400` de protocolo,
  fallback SPA vs `404` de `/api`. `.github/workflows/docker-image.yml`
  ahora tiene un job `test` que corre ambos y bloquea `build-and-push` si
  falla (`needs: test`).
  Probado que el test de integración detecta una regresión de verdad, no
  solo que pasa cuando todo está bien: deshabilité a propósito el guardrail
  de IDOR, el test lo agarró exacto (`403` esperado, `200` real), y lo
  revertí después.
  **Encontrado en el camino armando la simulación de CI (checkout limpio,
  sin el `.env`/volúmenes ya calentados de esta sesión):**
  - El `Makefile` principal usa `find` recursivo para `.c` — al agregar
    `tests/unit/`, empezó a arrastrar su `main()` y chocaba con el
    `main()` real. Excluido `tests/` del glob, igual que ya excluía `tools/`.
  - `DB_POOL_SIZE` default (4) más `max_connections=20` (pensado para 2
    vCPU) revienta en cualquier runner con más cores — confirmado en este
    mismo host de 8 cores. Fijado `DB_POOL_SIZE=1` explícito en el paso de
    CI para que no dependa de cuántos cores le toquen al runner.
  - **Bug real y preexistente, no de esta sesión:**
    `db/init/02_postgres-sakila-schema.sql` seguía físicamente en el repo
    pese a que este mismo archivo ya decía "resuelto sacando Sakila del
    todo" más abajo — sobre un volumen de Postgres fresco (como arranca
    cualquier CI real) fallaba con `role "postgres" does not exist`
    porque el script asume ese nombre de rol y `POSTGRES_USER` es otro.
    Confirmado sin ninguna referencia en código (`grep`) y eliminado.
    Esto llevaba probablemente toda la sesión fallando en silencio contra
    el volumen de Postgres del entorno de desarrollo, sin notarse porque
    los scripts de init solo corren una vez, contra un volumen vacío.
  Regresión final, simulando un checkout de CI genuinamente limpio (clon
  aparte, sin `.env`, sin volúmenes previos): 23/23 tests de integración
  ok, `RestartCount=0`.~~
- [ ] **`X-Forwarded-For` en `conn_limit`/`login_limit`, para cuando se
      configure nginx.** Decisión de esta sesión: no vale la pena arreglar
      `userland-proxy` de Docker a nivel de host, porque con nginx delante
      cerver va a ver la IP de nginx igual, no la del cliente real — el
      problema se reintroduce un nivel más arriba. El fix real es leer
      `X-Forwarded-For`/`X-Real-IP` en `conn_limit_get_ip()` en vez de la IP
      del socket — **solo si nginx es la única vía de entrada** (si el puerto
      de cerver sigue expuesto en paralelo, cualquiera puede falsear ese
      header y saltarse `MAX_CONN_PER_IP`/`LOGIN_MAX_ATTEMPTS`). No
      implementado todavía porque nginx no existe aún en el repo.
- [ ] **Logging estructurado.** Sigue siendo `printf`/`fprintf(stderr, ...)`
      suelto. Sin esto, si cualquiera de los mecanismos de esta sesión se
      dispara en producción (rate limit de login, rechazo de conexión,
      reinicio por crash), no queda rastro consultable.
- [ ] **Roles/permisos + revocación de JWT.** Hoy todo lo protegido es
      "logueado sí o no". Distinto del guardrail de IDOR
      (`route_require_owner`, ya resuelto): ese resuelve "¿puede ver ESTE
      recurso, que es suyo?"; roles resuelve "¿puede hacer X en general?". JWT
      válido hasta que expira (24h default), sin lista de revocación ni logout
      real del lado servidor.
- [ ] **Circuit breaker cuando el pool de DB se agota** (`DB_PENDING_Q` lleno
      bajo carga sostenida). Hoy devuelve `NULL` al callback sin backoff ni
      jitter — probado en un stress test puntual, no bajo saturación
      sostenida real.

## ✅ Resuelto esta sesión

**Seguridad**
- [x] ~~`.env` fuera del tracking de git, purgado del historial
  (`git filter-repo`), `*.pem` en `.gitignore`.~~
- [x] ~~Inyección de comandos en `.github/workflows/docker-image.yml:26`
  (código muerto con vulnerabilidad viva) — eliminada.~~
- [x] ~~Grep de secrets hardcodeados fuera de `.env`: ninguno encontrado
  (foto de hoy, no hay gate automático que lo garantice a futuro).~~
- [x] ~~**Límite de conexiones concurrentes, global y por IP** (CWE-770) —
  `utils/net/conn_limit.h`/`.c`, `MAX_CONNECTIONS`/`MAX_CONN_PER_IP`.
  Contadores atómicos globales sin locks, buckets hasheados por IP.
  Probado con carga real (`ab`, hasta 26k req/s) y aislamiento entre IPs
  reales distintas — 0 falsos rechazos, límite exacto respetado.~~
- [x] ~~**Rate-limiting de login fallido por IP** (fuerza bruta /
  credential stuffing) — `utils/auth/login_limit.h`/`.c`,
  `LOGIN_MAX_ATTEMPTS`/`LOGIN_WINDOW_SECONDS`. Rechaza con `429` antes de
  tocar la DB o calcular PBKDF2. De paso, `explicit_bzero` sobre passwords
  en texto plano en memoria (`login_ctx_t`/`register_user`).~~
- [x] ~~**SYN flood** — evaluado, no mitigable en la app (pasa antes de
  que llegue un byte). Host ya bien configurado (`tcp_syncookies=1`), AWS
  Shield Standard cubre EC2 gratis y automático. Se descartó a propósito
  forzar el sysctl vía Docker: con `userland-proxy` activo sería placebo
  (el handshake real lo recibe el host, no el contenedor).~~
- [x] ~~**Flags de hardening del compilador** — `-fstack-protector-strong
  -D_FORTIFY_SOURCE=2` y `-Wl,-z,relro,-z,now`. PIE ya viene por default
  del toolchain de Alpine/musl. Verificado sobre el binario real
  (disassembly, `readelf`), no asumido.~~
- [x] ~~`sprintf` suelto + lectura sin validar `PQgetlength` en
  `controllers/home.h` — corregido a `snprintf` + guard `len < 4`.~~
- [x] ~~**Guardrail contra IDOR** (CWE-639) — `route_require_owner()`
  (`utils/http/router.h`) + endpoint de ejemplo `GET /api/me/:id`. Probado
  con dos usuarios reales: propio id → `200`, id ajeno → `403`.~~
- [x] ~~**Perfil de seccomp propio** (`seccomp-cerver.json`), reemplaza
  `seccomp:unconfined` — default de Docker (`moby/profiles`) + los tres
  syscalls de `io_uring` que le faltaban (`io_uring_setup`/`enter`/
  `register`, confirmados por `grep` y por `strace` real). Nota: con
  `io_uring`, un seccomp por-syscall no controla lo que pasa *adentro* de
  una operación ya sometida al anillo (`openat2`/`statx` nunca aparecen
  como syscall propio del proceso) — mismo motivo por el que `io_uring`
  genera desconfianza en seguridad en general.~~
- [x] ~~**Decisión sobre `userland-proxy` de Docker**: no se arregla a
  nivel de host. Con nginx planeado delante, el problema se reintroduce
  ahí igual — ver pendiente de `X-Forwarded-For` arriba.~~

**Confiabilidad**
- [x] ~~**Reconexión automática a Postgres** si la DB se cae y vuelve —
  `reconnect_if_dead()` (`config/db.c`). Probado con `docker compose
  restart db` bajo carga: el pool se recupera solo, cero intervención
  manual.~~
- [x] ~~**Reinicio automático de proceso ante un crash real** —
  `restart: unless-stopped` ya estaba, pero `init_db()` hacía `exit(1)`
  inmediato si Postgres no respondía al primer intento (crash-loop
  potencial). Agregado `connect_with_retry()` con backoff (~20s de
  presupuesto). Probado con un crash real disparado desde adentro del
  proceso (`SIGSEGV` vía endpoint temporal, revertido después): recupera
  solo incluso con la DB caída al momento del crash.~~
- [x] ~~Manejo de señales `SIGTERM`/`SIGINT` — apagado graceful, drena
  conexiones en curso antes de salir (`g_shutdown`, `SHUTDOWN_GRACE_SECONDS`).~~

**Router / auth / observabilidad**
- [x] ~~Parámetros de path (`/recursos/:id`), estilo Express —
  `path_matches()` en `utils/http/router.h`.~~
- [x] ~~Puerto configurable vía `PORT` (antes hardcodeado a 8080).~~
- [x] ~~Métodos `post`/`put`/`patch`/`del` verificados — de paso, arreglado
  un bug real de truncado silencioso de bodies grandes (ahora `413`
  explícito en vez de corromper datos).~~
- [x] ~~**JWT propio (HS256) end-to-end** — registro, login, rutas
  protegidas, PBKDF2 con sal por usuario, comparación en tiempo
  constante, sin vector de confusión de algoritmo.~~
- [x] ~~Healthcheck real `/healthz`, separado del chequeo de DB (`/api`).~~

**Performance / configuración**
- [x] ~~Cache en memoria para `actors/top` (30x mejora, 1.8k → 55k req/s).~~
- [x] ~~Constantes hardcodeadas → variables de entorno (la mayoría; ver
  pendiente de `INITIAL_READ_BUF_SIZE` abajo).~~
- [x] ~~`DB_POOL_SIZE`/`DB_PENDING_QUEUE_SIZE` → env vars, arreglos
  convertidos a memoria dinámica.~~
- [x] ~~**Tuneo de producción real** (instancia chica por cliente,
  `t4g.small`/`micro`) — `shared_buffers`/`work_mem`/`max_connections` de
  Postgres bajados del perfil de benchmark al de un cliente real,
  `DB_POOL_SIZE` default 16→4. Medido con `docker stats` bajo carga real:
  2.3MB el backend, 31MB Postgres — muy por debajo de lo estimado.~~

**Proceso / documentación**
- [x] ~~`README.md` reescrito corto, repo limpiado (`build/` borrado,
  Sakila removido del boilerplate, `tools/dbfiller` generaliza).~~

## Backlog (no bloqueante)

- [ ] Métricas mínimas (requests/s, latencia, pool de DB en uso) — depende de
      a dónde se van a mandar (Prometheus, CloudWatch, algo propio).
- [ ] `INITIAL_READ_BUF_SIZE` (`utils/http/http.h`, hoy 4096) a variable de
      entorno — atado a la validación de `Content-Length`, requiere retestear
      ese caso puntual si se toca.
- [ ] Métrica de éxito del fallback SPA vs 404 reales, para detectar rutas de
      frontend rotas.
- [ ] HTTPS/TLS in-app: decidido dejarlo afuera, asumido detrás de nginx.
- [ ] Tamaño de imagen (245KB → 2.56MB tras auth + `/docs`): evaluado y
      aceptado por ahora. Opciones si algún día importa: target `dev`/`prod`
      separados en el `Dockerfile`, o reemplazar Swagger UI por un renderer
      de solo lectura.
