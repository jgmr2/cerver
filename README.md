# cerver

Servidor HTTP en C puro sobre `io_uring`, sin framework, para exponer APIs JSON sobre Postgres.

Nace para un caso de uso concreto: backends a medida para pequeñas empresas, corriendo en instancias de AWS baratas. Ahí el costo por MB de RAM y por vCPU importa más que exprimir el último 10% de throughput — un binario estático de unos cientos de KB permite meter muchos más clientes por instancia que un runtime de JVM o Node.

## Objetivos

- **Footprint mínimo**: binario estático, sin runtime pesado, pensado para correr varias instancias del backend (una por cliente) en la misma máquina.
- **Sin bloqueo, nunca**: todo I/O (red, disco, Postgres) pasa por `io_uring`; ningún hilo worker espera una syscall bloqueante.
- **Sin framework, sin ORM**: menos capas que auditar. El SQL vive a mano, envuelto para que Postgres arme el JSON final (`row_to_json`/`json_agg`).
- **El motor se replica, el negocio no**: `core/`, `config/`, `utils/` son genéricos y se copian igual entre proyectos; cada API nueva solo agrega archivos en `models/`, `controllers/` y `routes/`.

## Qué incluye hoy

- Servidor HTTP/1.1 thread-per-core (`SO_REUSEPORT`), sin locks entre hilos.
- Pool de conexiones a Postgres 100% asíncrono, con reconexión automática si la DB se cae y vuelve.
- Autenticación JWT (HS256) propia — registro, login, rutas protegidas.
- Servidor de estáticos con fallback SPA, protegido contra path traversal.
- Documentación interactiva de la API en `/docs` (Swagger UI).
- Apagado graceful, healthcheck separado del chequeo de DB, headers de seguridad, mitigación de slowloris y de HTTP request smuggling.

## Puesta en marcha

```bash
# crear .env en la raiz con POSTGRES_USER, POSTGRES_PASSWORD, POSTGRES_DB,
# DATABASE_URL y JWT_SECRET (el proceso no arranca si falta DATABASE_URL o JWT_SECRET)
docker compose up -d
curl http://localhost:8080/healthz
```

Todo lo demás es configurable por variable de entorno con un default razonable si no se define — `PORT`, `SHUTDOWN_GRACE_SECONDS`, `CACHE_REFRESH_SECONDS`, `DB_CONNECT_TIMEOUT_SECONDS`, `JWT_EXPIRES_SECONDS`, `PBKDF2_ITERATIONS`, `DB_POOL_SIZE`, `DB_PENDING_QUEUE_SIZE` (ver `docker-compose.yml` y `.env`) — nada de esto requiere recompilar para ajustarlo por deployment.

Ver `/docs` (Swagger UI) para el detalle de cada endpoint, y [TODO.md](TODO.md) para qué está resuelto y qué falta antes de usar esto en producción real.

## Créditos de terceros

- [picohttpparser](https://github.com/h2o/picohttpparser) — Kazuho Oku, Tokuhiro Matsuno, Daisuke Murase, Shigeo Mitsunari (MIT).
- [jsmn](https://github.com/zserge/jsmn) — Serge Zaitsev (MIT), variante minificada en `utils/json.h`.
- [Swagger UI](https://swagger.io/tools/swagger-ui/) — SmartBear Software (Apache 2.0), vendorizado en `docs-ui/`.
