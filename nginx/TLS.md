# TLS por instancia

Este proyecto no activa HTTPS por default: `nginx/nginx.conf` (lo que
efectivamente corre con `docker compose up`) solo escucha en `:80`. Este
documento es el paso a paso para activarlo en una instancia puntual,
una vez que ese cliente ya tiene un dominio propio apuntando a ella —
antes de eso no hay nada que hacer acá.

## Qué ya está preparado

- `nginx/nginx-tls.conf.template` — la config completa con `:443` +
  redirect de `:80`, con `${DOMAIN}` como placeholder.
- `docker-compose.yml`, perfil `tls` (no arranca con un `docker compose
  up` normal, hay que pedirlo explícito con `--profile tls`):
  - `certbot` — obtiene el certificado real (Let's Encrypt, desafío
    HTTP-01 vía `--webroot`).
  - `certbot-renew` — loop que renueva cada 12hs mientras corra.
  - `self-signed-cert` — certificado autofirmado, para probar HTTPS
    local sin depender de un dominio real.
- `nginx/nginx.conf` ya sirve `/.well-known/acme-challenge/` (necesario
  para el desafío de certbot), sin efecto mientras no se use.

## Probar en local con certificado autofirmado (sin dominio real)

```bash
DOMAIN=localhost docker compose --profile tls run --rm self-signed-cert

# Reemplazar ${DOMAIN} y activar la config HTTPS:
DOMAIN=localhost envsubst '$DOMAIN' < nginx/nginx-tls.conf.template > nginx/nginx.conf.tls-active
cp nginx/nginx.conf.tls-active nginx/nginx.conf   # o ajustar nginx/Dockerfile para copiar este archivo

docker compose up -d --build
curl -k https://localhost/healthz   # -k: el navegador/curl no va a confiar en un autofirmado, esperado
```

El navegador va a marcar el certificado como no confiable (es
autofirmado) — sirve para validar que nginx sirve HTTPS de verdad, no
para un cliente real.

**Revertir después de probar:** `git checkout -- nginx/nginx.conf` (o
volver a copiar el `nginx.conf` original) y `docker compose down -v &&
docker compose up -d --build` para no dejar el proyecto en modo TLS de
prueba.

## Activar para un cliente real

1. **DNS primero.** El dominio del cliente (ej. `app.cliente.com`) tiene
   que resolver a la IP pública de esta instancia *antes* del paso
   siguiente — Let's Encrypt valida el desafío haciendo una petición HTTP
   real a ese dominio.

2. **Certificado real:**
   ```bash
   DOMAIN=app.cliente.com CERTBOT_EMAIL=vos@tu-empresa.com \
     docker compose --profile tls run --rm certbot
   ```
   Si falla, revisar que el puerto 80 de esta instancia ya esté
   accesible públicamente en ese dominio (`docker compose up -d` corriendo,
   sin firewall/security group bloqueando el 80) — certbot necesita
   llegar hasta `/.well-known/acme-challenge/` sobre HTTP plano.

3. **Activar la config HTTPS:**
   ```bash
   DOMAIN=app.cliente.com envsubst '$DOMAIN' < nginx/nginx-tls.conf.template > nginx/nginx.conf
   ```
   (Sobreescribe `nginx/nginx.conf` — el `nginx.conf` HTTP-only original
   queda en el historial de git si hace falta volver atrás.)

4. **Publicar el puerto 443 y reconstruir:**
   Descomentar `- "443:443"` en el servicio `nginx` de
   `docker-compose.yml`, después:
   ```bash
   docker compose up -d --build
   curl https://app.cliente.com/healthz
   ```

5. **Dejar la renovación corriendo:**
   ```bash
   docker compose --profile tls up -d certbot-renew
   ```
   **Importante:** `certbot renew` deja el certificado nuevo en el
   volumen `certbot_certs`, pero nginx no relee ese archivo solo — hace
   falta un reload después de cada renovación real (Let's Encrypt
   renueva cada ~60-90 días, no en cada corrida de `certbot renew`, que
   solo actúa si falta poco para el vencimiento). Dos formas de
   resolverlo, elegir una:
   - Cron del host (fuera de Docker): `0 3 * * * docker compose exec
     nginx nginx -s reload` una vez por día, barato y sin exponer nada
     nuevo.
   - Dar a `certbot-renew` acceso al socket de Docker
     (`/var/run/docker.sock`) y un `--deploy-hook` que corra `docker
     exec nginx nginx -s reload` — funciona sin nada en el host, pero
     un contenedor con el socket de Docker montado puede controlar
     cualquier otro contenedor del host: evaluar si el trade-off vale
     la pena para esta instancia en particular antes de hacerlo.

## Notas

- `MAX_CONN_PER_IP`/`TRUST_PROXY_HEADERS` (ver `README.md`) no cambian
  con TLS activado: nginx sigue siendo la única vía de entrada, HTTPS
  o no.
- Este documento asume **una instancia = un dominio**, acorde al modelo
  de despliegue de este proyecto (ver `TODO.md`, "Estrategia de
  actualización entre forks"). Múltiples dominios en la misma instancia
  (varios `server_name`/certificados en un mismo nginx) no está
  contemplado acá.
