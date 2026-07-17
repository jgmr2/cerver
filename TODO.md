# TODO — cimientos de cerver

Lista de puntos a revisar antes de apoyar trabajo de negocio real sobre este
boilerplate. Organizada por prioridad, no por orden cronológico. Marcados los
que ya se resolvieron en esta sesión.

## Seguridad

- [x] Sacar `.env` del tracking de git (`git rm --cached`).
- [x] Purgar `.env` y `labsuser.pem` de todo el historial (`git filter-repo`) y
      forzar el push a las 4 ramas remotas.
- [x] Agregar `*.pem` a `.gitignore`. (Ampliado: el patrón original solo
      cubría `.cache/*.pem`, no `*.pem` en la raíz — ver `.gitignore`.)
- [ ] **Rotar credenciales.** Purgar el historial no invalida lo que ya se filtró
      a GitHub — `POSTGRES_PASSWORD`/`DATABASE_URL` hay que cambiarlos igual, y
      si `labsuser.pem` sigue siendo una llave activa en AWS, revocarla.
      **Esto no lo puede hacer un asistente de código: requiere acceso a AWS y
      al Postgres real.**
- [x] **Arreglar la inyección de comandos en `.github/workflows/docker-image.yml:26`.**
      El paso completo se eliminó en vez de sanearlo: `commit_msg` no se
      consumía en ningún otro paso del workflow, era código muerto con una
      vulnerabilidad viva.
- [x] Revisar si hay más secrets hardcodeados fuera de `.env` — grep de
      patrones de credenciales sobre todo `.c`/`.h`/`.yml` del repo, ninguno
      encontrado. Esto es una foto de hoy, no un gate: sigue sin haber nada
      automático que lo garantice a futuro (ver punto de CI más abajo).

## Confiabilidad

- [x] **Probar reconexión a Postgres.** Confirmado el problema sospechado:
      `docker compose restart db` con `ab -k` corriendo en paralelo (60k
      requests) daba 25,319 fallos (42%) y el proceso quedaba degradado para
      siempre — cada consulta posterior devolvía `200` con
      `{"error":"Sakila query failed"}` en el body hasta reiniciar el
      contenedor a mano. Arreglado: `reconnect_if_dead()` (`config/db.c`),
      invocado desde `start_query_with_ctx` antes de cada envío, detecta
      `PQstatus() != CONNECTION_OK` y reconecta (con `connect_timeout=3`,
      re-preparando todos los statements del registro). Repetido el mismo
      test tras el fix: 8,642 fallos (la ventana real de la caída, inevitable
      sin una cola de reintentos a nivel HTTP) pero el pool se recupera solo
      — cero intervención manual. Es la única excepción a "nada bloquea al
      hilo worker" en todo el motor, documentada en el propio
      `reconnect_if_dead()` (`config/db.c`), porque una reconexión async
      completa (`PQconnectStart`/`PQconnectPoll`) es mucho
      más código para un evento raro; si las caídas de Postgres son
      frecuentes en producción, ahí sí conviene escribir esa versión.
- [ ] Definir política de reintentos/circuit breaker cuando el pool de un hilo
      se agota (`DB_PENDING_Q` lleno) bajo carga sostenida, más allá de lo que
      ya se probó en el stress test puntual. Sigue pendiente: el
      comportamiento actual (devolver `NULL` al callback apenas la cola de
      8192 también se llena) es razonable pero no se probó bajo saturación
      sostenida real, y no hay backoff ni jitter si muchos requests fallan
      a la vez.
- [x] Decidir manejo de señales (`SIGTERM`/`SIGINT`): implementado. Cada hilo
      dejaba de responder en seco (`docker stop` mataba conexiones en curso a
      mitad de escritura); ahora `g_shutdown` (`utils/events.h`) hace que cada
      hilo deje de aceptar conexiones nuevas, drene las existentes hasta que
      terminan su request en curso, y salga — con un plazo máximo de
      `SHUTDOWN_GRACE_SECONDS=5` (`core/server.c`) por si algo no termina
      nunca. Probado con `ab` corriendo y `docker stop -t 10` en paralelo: los
      8 hilos completaron el drenado en 1.26s, muy por debajo del plazo.

## Testing (hoy: cero)

- [ ] Elegir una estrategia mínima viable: al menos tests de integración que
      levanten el binario contra una Postgres de prueba y peguen a los
      endpoints reales (lo que hice a mano con `curl`/`ab` en esta sesión,
      pero repetible y en CI).
- [ ] Tests unitarios para las funciones puras que ya existen y son fáciles de
      aislar: `url_decode_path`, `path_has_hidden_segment`,
      `path_looks_like_asset` (`utils/http/static.h`), `mime_type_for_path`
      (`utils/http/mime.h`), `path_is_api` (`utils/http/router.h`).
- [ ] Agregar un job de test al workflow de GitHub Actions — hoy
      `docker-image.yml` solo hace build+push, nada valida el código antes de
      publicarlo.
- [ ] Test de humo post-deploy (golpear `/api` y devolver el build si falla).

## Router / funcionalidad core faltante

- [x] **Parámetros de path** (`/recursos/:id`). Implementado con sintaxis
      estilo Express (`:nombre`, un segmento por parámetro, sin wildcards ni
      segmentos opcionales — la cantidad de segmentos tiene que coincidir
      exacto). `path_matches()` reemplaza el `strcmp` exacto de `dispatch()`
      (`utils/http/router.h`) y sigue siendo compatible con las rutas
      literales existentes (un patrón sin `:` se comporta igual que antes).
      Los valores capturados se consultan desde el handler con
      `route_param("nombre")`; documentado el riesgo de usarlos para armar
      SQL a mano (tienen que ir como parámetro real de un prepared
      statement). Agregado `GET /api/echo/:msg` (`controllers/home.h`) como
      ejemplo mínimo y para probar el feature end-to-end — confirmado que
      escapa correctamente comillas/backslash en la respuesta JSON, y que
      rutas con y sin parámetros conviven bien bajo carga (0 fallos, 20k
      requests c=100 por cada tipo de ruta).
- [x] Puerto HTTP hardcodeado a 8080 — ahora configurable vía `PORT`
      (`g_port`, `core/server.h`/`main.c`), con default 8080 si no está
      definida o es inválida. `Dockerfile` declara `ENV PORT=8080` y
      `EXPOSE 8080` (antes decía `EXPOSE 80`, que ni siquiera coincidía con
      el puerto real). Probado corriendo la imagen con `PORT=9090` por fuera
      de `docker-compose.yml`.
- [x] Verificado el soporte de métodos (`post`/`put`/`patch`/`del`) registrando
      handlers de prueba temporales para los cuatro. El mecanismo de ruteo
      funciona bien (método+path exacto, body accesible vía el buffer crudo).
      **Se encontró y arregló un bug real en el camino**: un `POST` con un
      body más grande de lo que entra en el único read de
      `INITIAL_READ_BUF_SIZE` (~4KB, headers incluidos) se truncaba en
      silencio y el servidor respondía `200` igual, como si el request
      hubiera llegado completo — corrupción silenciosa de datos, no un
      simple error de parseo. Arreglado en `core/server.c`: ahora se parsea
      el valor real de `Content-Length` y se compara contra los bytes
      efectivamente recibidos; si no coincide, se responde `413 Payload Too
      Large` explícito en vez de despachar datos incompletos. Probado con un
      body de 6000 bytes (antes: `200` con 3954 bytes truncados; después:
      `413`) y con bodies chicos normales en los 4 métodos (siguen en `200`).
      Sigue habiendo un límite real: un body que no entra en un solo read
      nunca va a funcionar, aunque ahora al menos falla explícito en vez de
      corromper datos. Soportar bodies más grandes requeriría acumular
      varios reads antes de `dispatch()` — no implementado, es una feature
      bastante más grande si hace falta subir archivos o payloads de varios KB.

## Autenticación y autorización

- [x] **JWT propio (HS256), emitido por este mismo backend.** Implementado
      de punta a punta:
      - `utils/auth/password.c` — PBKDF2-HMAC-SHA256 (100k iteraciones) con
        sal aleatoria por usuario, comparación en tiempo constante
        (`CRYPTO_memcmp`).
      - `utils/auth/jwt.c` — creación/verificación de JWT, deliberadamente
        limitado a HS256 (nunca lee el `alg` del token entrante, cierra por
        diseño el vector de "confusión de algoritmo").
      - `db/init/01_auth_schema.sql` — tabla `users`.
      - `models/users.c` — lookup/creación de usuario, vía queries
        parametrizadas reales (`$1`, `$2`), no concatenadas.
      - `controllers/auth.c` — `POST /api/auth/register`, `POST
        /api/auth/login`, `GET /api/me` (demo de ruta protegida).
      - `utils/http/router.h` — `get_auth()`/`post_auth()`/etc. exigen JWT
        válido antes de invocar el handler; los claims se leen con
        `jwt_claim("sub")`/`jwt_claim("username")`.
      - Efecto colateral necesario: `config/db.h`'s `cb` ganó un parámetro
        `void *userdata` (y se actualizaron los 4 callbacks existentes) para
        poder llevar contexto (la contraseña en texto plano a verificar)
        hasta el callback async de la consulta — no había forma de hacerlo
        antes sin esto.
      - Bug encontrado y arreglado en el camino: `route_params`/
        `route_param_count` (router.h, parámetros de path) eran `static
        __thread` en vez de `extern __thread` — mismo bug que
        `conn_keep_alive` esta sesión, latente porque ningún handler con
        parámetro vivía todavía en un `.c` separado de `core/server.c`.
      - Probado end-to-end: registro, login, `/api/me` con token válido
        (200), sin token (401), con token adulterado (401), login con
        contraseña incorrecta (401) y usuario inexistente (401, misma
        respuesta — no revela si el username existe), registro duplicado
        (409), validación de input (username con caracteres inválidos,
        contraseña corta, body no-JSON — todos 400). Carga real sin
        regresión: `actors/top` 55k req/s, `films/top` 17k req/s, ambos con
        0 fallos después del refactor de `db.c`.
- [ ] Sigue pendiente: roles/permisos (todo lo protegido hoy es "logueado sí
      o no", sin niveles), y revocación de tokens (hoy un JWT es válido
      hasta que expira — 24h por defecto — no hay lista de revocación ni
      logout real del lado servidor, algo esperable en JWT stateless pero
      vale dejarlo anotado).

## Observabilidad

- [ ] Logging estructurado — hoy sigue siendo `printf`/`fprintf(stderr, ...)`
      suelto, sin niveles ni formato consistente (se agregaron un par de
      líneas más en `config/db.c` para la reconexión a Postgres, pero es la
      misma falta de estructura, no una solución). Sigue pendiente: elegir
      formato (¿JSON por línea? ¿syslog?) es una decisión de stack, no algo
      para inventar sin saber qué va a consumir esos logs.
- [ ] Métricas mínimas (requests/s, latencia, tamaño del pool de DB en uso,
      tamaño de `pending_q`) — ahora mismo la única forma de ver esto es un
      stress test manual con `docker stats` al lado. Sigue pendiente: depende
      de a dónde se van a mandar (Prometheus, CloudWatch, algo propio).
- [x] Un healthcheck real (separado de `/api`, que hoy pega a Postgres) para
      que el orquestador sepa distinguir "el proceso vive" de "la DB
      responde". Agregado `GET /healthz` (`controllers/home.h`,
      `routes/index.h`): responde `200 ok` sin tocar la base de datos. Si
      `/healthz` da 200 y `/api` no, el problema es Postgres, no el proceso.

## Proceso / documentación

- [x] `README.md` con propósito y objetivos del proyecto. Reescrito para
      quedar deliberadamente corto (qué es, por qué existe, filosofía de
      diseño, puesta en marcha mínima) en vez del manual técnico extenso
      que tenía antes — ese detalle (ciclo de vida de un request, capa de
      datos, cómo agregar un endpoint paso a paso) se sacó del repo; sigue
      recuperable en el historial de git (`git log` sobre `README.md`) si
      hace falta reconstruirlo.
- [x] Repo limpiado de archivos innecesarios: se borró `build/` (artefactos
      de compilación fuera de Docker, ya cubiertos por `.gitignore` pero
      habían quedado en disco) y se consolidó toda la documentación en
      exactamente dos `.md` (este archivo y `README.md`).
- [x] `CONCURRENCY.md` (ownership de `req_t`/`PGresult`, la regla
      `__thread`+`extern`, el patrón `io_uring_prep_link_timeout`, el
      protocolo de `g_shutdown`) se eliminó del working tree a pedido
      explícito, para no tener un tercer `.md`. Sigue commiteado en el
      historial de git (`git show e9b774d:CONCURRENCY.md`) si alguien
      necesita ese detalle para tocar el motor io_uring sin ayuda.
- [ ] Documentar en algún lado la política real de versionado de Sakila
      como dataset de ejemplo vs. cómo un cliente nuevo reemplaza ese modelo
      por el suyo — hoy Sakila está un poco entreverada con el boilerplate en
      sí (`db/sakila/`, `db/init/`). Con el README recortado, esto ya no
      tiene un lugar obvio donde vivir — decidir si va en un comentario en
      `db/init/README.md` o se deja para cuando exista el primer cliente
      real que necesite reemplazarlo.

## Performance

- [x] `/api/sakila/actors/top` se estancaba en ~1800 req/s sin importar la
      concurrencia (medido con `ab`), con Postgres al 443% CPU por el
      `JOIN`+`GROUP BY` sobre toda `film_actor` en cada request, mientras el
      proceso C estaba prácticamente ocioso. Agregado un cache en memoria por
      hilo (`actors_cache_json`, `controllers/sakila.c`), refrescado cada
      `CACHE_REFRESH_SECONDS=30` por un timer de `io_uring` independiente
      (`arm_cache_timer`, `core/server.c`) — mismo patrón que `init_routes()`:
      `core/server.c` no sabe que existe, solo llama a `refresh_caches()`
      (`routes/index.h`), y cada controlador que necesite cachear algo se
      registra ahí. Medido después del fix: 54,982 req/s (30x) con Postgres
      de vuelta en ~0% CPU. `films/top` no se tocó, no tenía este problema.

## Reutilización / configuración

- [x] **Mover constantes hardcodeadas a variables de entorno.** Se auditó
      todo el repo (`grep` de `#define` numéricos) y se separaron dos
      grupos: los que son tunables operativos reales (movidos hoy) y los
      que dimensionan arreglos estáticos en el hot path (`S` — tamaño del
      pool de Postgres por hilo, `DB_PENDING_Q`, `INITIAL_READ_BUF_SIZE`),
      que requieren convertir esos arreglos a memoria dinámica — quedan
      pendientes, ver el ítem de abajo.
      Movidos: `PORT` (ya estaba), `SHUTDOWN_GRACE_SECONDS`,
      `CACHE_REFRESH_SECONDS`, `DB_CONNECT_TIMEOUT_SECONDS`,
      `JWT_EXPIRES_SECONDS`, `PBKDF2_ITERATIONS` — todos con default igual
      al valor que tenían hardcodeado, resueltos una sola vez en `main()`
      (`read_long_env()`), sin recompilar para ajustarlos por deployment.
      Caso especial: `PBKDF2_ITERATIONS` no se podía mover ingenuamente —
      el número de iteraciones ahora viaja *dentro* del hash guardado
      (`"iteraciones:salt:hash"`, antes `"salt:hash"`), no se lee de la
      variable de entorno al verificar. Sin esto, cambiar la variable
      rompería el login de todos los usuarios ya registrados (se
      verificarían con un costo distinto al que se usó para crear su
      hash). Efecto secundario esperado y aceptado: los usuarios de
      prueba creados durante esta sesión (formato viejo) ya no pueden
      loguearse — son cuentas de prueba, sin impacto real.
      Probado con contenedor aparte (`PORT`, `JWT_EXPIRES_SECONDS=60` +
      espera real a que venza, `PBKDF2_ITERATIONS=5000` con tiempo de
      registro medido contra el default, `SHUTDOWN_GRACE_SECONDS=2` con
      `docker stop` en medio de tráfico) — todos los valores confirmados
      en uso real, no solo que compilan.
- [x] **`S` → `DB_POOL_SIZE` y `DB_PENDING_Q` → `DB_PENDING_QUEUE_SIZE`,
      movidos a variables de entorno.** Estos dos sí dimensionaban
      arreglos `__thread` estáticos (`db_t pool[S]`,
      `pending_req_t pending_q[DB_PENDING_Q]`) en el camino caliente de
      cada consulta a Postgres — no era un simple `getenv()`: `pool[]`,
      `free_pool[]` y `pending_q[]` pasaron de arreglos de tamaño fijo a
      punteros reservados una sola vez con `calloc()` en `init_db()`,
      usando el tamaño ya resuelto desde el entorno. `db_t *pool` (antes
      `db_t pool[S]`) es el único cambio de tipo visible desde afuera de
      `config/db.c`, y no tiene otros usos fuera de ese archivo (se
      verificó con grep antes de tocarlo).
      Probado con un contenedor aparte: `DB_POOL_SIZE=2` (default 16) dio
      exactamente 16 conexiones nuevas en `pg_stat_activity` (8 hilos ×
      2, no 8 × 16), y aguantó `ab -c 100` contra un pool de solo 16
      conexiones totales sin un solo fallo — la cola absorbió la
      sobrecarga como se esperaba. De paso, un load test post-cambio dio
      un resultado bajo (~4.6k req/s en `films/top`) que resultó ser
      ruido transitorio del arranque del contenedor, no una regresión:
      se repitió 3 veces más y volvió al rango esperado (17k+ req/s) de
      forma estable — anotado acá para que si alguien ve un número raro
      en el primer request después de levantar el contenedor, sepa que
      ya se investigó y no es un bug de esta sesión.
- [ ] Mismo caso para `INITIAL_READ_BUF_SIZE` (`utils/http/http.h`, hoy
      4096): dimensiona el buffer de lectura inicial de cada conexión, y
      está directamente atado a la validación de `Content-Length` que
      hoy responde `413` si el body no entra ahí. Configurable en teoría,
      pero cualquier cambio ahí hay que volver a probarlo contra ese caso
      específico para no reintroducir el bug de truncado silencioso que
      se arregló antes.

## Ideas para más adelante (no bloqueante)

- [ ] Rate limiting básico por IP/fd.
- [ ] Soporte HTTPS/TLS (hoy asume que hay un proxy tipo nginx delante).
- [ ] Métrica de éxito del fallback SPA vs 404 reales, para detectar rutas de
      frontend rotas.
- [ ] **Imagen del backend: 245KB → 2.56MB comprimida** tras agregar auth
      (OpenSSL estático, ~5.1MB crudo, `--gc-sections` no ayuda porque
      HMAC/PBKDF2/`RAND_bytes` arrastran el resto de la librería) y
      `/docs` (Swagger UI vendorizado, `swagger-ui-bundle.js` solo pesa
      1.5MB). Evaluado hoy y decidido dejarlo así por ahora — 2.56MB sigue
      siendo chico en términos absolutos. Si el tamaño vuelve a importar
      (más tenants por instancia, por ejemplo), las opciones que quedaron
      sobre la mesa son: (a) Dockerfile con target `dev` (con `docs-ui/`)
      y target `prod` (sin él, vuelve a ~500-600KB) — `docker-compose.yml`
      usando `dev` por default; o (b) reemplazar Swagger UI por un
      renderer de solo-lectura (sin "probar el endpoint"), bajando
      `docs-ui/` de 2MB a <100KB. El costo de OpenSSL no tiene una opción
      barata: reemplazarlo por una librería de crypto más chica es
      arriesgado para algo que firma contraseñas y tokens.
