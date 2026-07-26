# Changelog

Registro de checkpoints de este repo (`cerver`), para que cada fork de
cliente (ver [`FORKING.md`](FORKING.md)) sepa qué trae cada versión
antes de mergear. Formato libre, no estrictamente
[Keep a Changelog](https://keepachangelog.com/) — lo importante es que
cada entrada diga qué cambió y por qué te importaría traerlo.

## [Unreleased]

- Fix: `db_query_prepared_params_async` (`src/config/db.c`) ahora
  encola bajo pool de Postgres agotado, igual que la variante sin
  parámetros — antes devolvía 404/500 falsos en get/create/update/delete
  de cualquier tabla generada, y en login, bajo concurrencia. Probado a
  1000 conexiones concurrentes / 200k requests, 0 fallos.
- Nuevo: TLS por instancia, sin activar por default —
  `nginx/nginx-tls.conf.template` + perfil `tls` en `docker-compose.yml`
  (`certbot`, `certbot-renew`, `self-signed-cert`). Ver `nginx/TLS.md`.
- Nuevo: `FORKING.md` + `tools/sync-upstream.sh` — flujo para crear el
  fork de un cliente nuevo y traer fixes de este repo después.

## v1.0.0

Primer checkpoint: autenticación derivada del esquema (sin tabla
hardcodeada), nginx delante (única vía de entrada, sirve el frontend y
hace reverse proxy), frontend Svelte funcional (login/registro/CRUD),
build de producción sin Swagger UI, `X-Forwarded-For` confiado de forma
segura detrás de nginx. Ver `README.md` para el estado completo.
