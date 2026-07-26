# TODO — pendientes de cerver

Backlog de lo que falta. Lo que el proyecto ya resuelve hoy está descrito en
[`README.md`](README.md), no acá — esta lista es solo lo pendiente.

Sin críticos pendientes por ahora — los últimos tres (404 falso bajo
pool agotado, TLS por instancia, estrategia de forks) quedaron
resueltos; ver `README.md`, `nginx/TLS.md` y `FORKING.md`
respectivamente.

## Backlog (no bloqueante)

- [ ] **Logging estructurado.** Sigue siendo `printf`/`fprintf(stderr,
      ...)` suelto. Sin esto, si algún mecanismo de rate-limit/rechazo/
      crash se dispara en producción, no queda rastro consultable —
      relevante en particular al soportar varias instancias
      independientes a la vez.
- [ ] **Roles/permisos + revocación de JWT.** Hoy todo lo protegido es
      "logueado sí o no" (distinto del guardrail de IDOR, que resuelve
      "¿es dueño de ESTE recurso?", no "¿puede hacer X en general?").
      JWT válido hasta que expira (24h default), sin lista de
      revocación ni logout real del lado servidor.
- [ ] **Circuit breaker cuando el pool de DB se agota bajo carga
      sostenida** (`DB_PENDING_QUEUE_SIZE` lleno). Ya no hay 404/fallos
      falsos por no encolar (`db_query_prepared_params_async` ahora
      encola igual que la variante sin parámetros, copiando los
      parámetros con `strdup` — ver `src/config/db.c`), pero si la cola
      misma se llena bajo carga sostenida extrema, sigue devolviendo
      `NULL` al callback sin backoff ni jitter.
- [ ] Métricas mínimas (requests/s, latencia, pool de DB en uso) —
      depende de a dónde se van a mandar (Prometheus, CloudWatch, algo
      propio).
- [ ] `INITIAL_READ_BUF_SIZE` (`src/utils/http/http.h`, hoy 4096) a
      variable de entorno — atado a la validación de `Content-Length`,
      requiere retestear ese caso puntual si se toca.
- [ ] Métrica de éxito del fallback SPA vs 404 reales del frontend, para
      detectar rutas rotas (el caso de un asset estático faltante ya da
      404 real, ver `nginx/nginx.conf`; falta instrumentar el resto).
- [ ] Reload automático de nginx tras una renovación real de TLS (ver
      `nginx/TLS.md`, sección de renovación) — hoy `certbot-renew`
      renueva el certificado en el volumen compartido, pero nginx no lo
      relee solo; queda como paso manual (cron del host, o exponerle a
      `certbot-renew` el socket de Docker, con el trade-off de
      seguridad que eso implica).
- [ ] `resolver` dinámico de nginx (`nginx/nginx.conf` usa `proxy_pass
      http://backend:8080` con hostname literal, resuelto una sola vez
      al arrancar nginx): si el backend se reinicia solo sin que nginx
      se reinicie también, nginx puede quedar con una IP vieja
      cacheada. Mitigable con `resolver 127.0.0.11 valid=10s;` +
      `proxy_pass` con variable.
