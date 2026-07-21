## ETAPA 0: COMPILACIÓN DE LIBRERÍAS ESTÁTICAS DE POSTGRES
FROM alpine:latest AS pg-static-deps
RUN apk add --no-cache build-base curl bison flex linux-headers
RUN curl -L https://ftp.postgresql.org/pub/source/v16.2/postgresql-16.2.tar.gz | tar xz
WORKDIR /postgresql-16.2
RUN ./configure --prefix=/opt/pgsql --without-readline --without-zlib --without-icu
RUN make -C src/common install && \
    make -C src/port install && \
    make -C src/interfaces/libpq install && \
    make -C src/include install

## ETAPA 1: COMPILACION DEL BACKEND DE DBFILLER-WEB
# Mismas dependencias de build que el Dockerfile estandar generado por
# scaffold_new_project (liburing/openssl/zlib estatico, postgresql-dev) --
# dbfiller-web usa el mismo motor cerver, solo le agrega sus propios
# controllers (ver controllers/dbfiller_*.c) y el codigo compartido de
# tools/dbfiller/src/ (symlinks relativos en dbfiller_core/, ver ese
# directorio) montado dentro del mismo contexto de build.
FROM alpine:latest AS build
RUN apk add --no-cache build-base linux-headers \
    openssl-dev openssl-libs-static zlib-dev zlib-static \
    liburing-dev postgresql-dev

COPY --from=pg-static-deps /opt/pgsql/lib/libpq.a /usr/lib/libpq.a
COPY --from=pg-static-deps /opt/pgsql/lib/libpgcommon.a /usr/lib/libpgcommon.a
COPY --from=pg-static-deps /opt/pgsql/lib/libpgport.a /usr/lib/libpgport.a
COPY --from=pg-static-deps /opt/pgsql/include/ /usr/include/

# Contexto de build = tools/dbfiller/ (ver docker-compose.yml, context:
# ../..) para que este stage tenga acceso tanto a webapp/backend/ como a
# src/. src/dbfiller_core/*.c/.h (dentro de webapp/backend/) son symlinks
# RELATIVOS "../../../../src/<archivo>" (ver ese directorio) -- pensados
# para resolver "tools/dbfiller/src/<archivo>" al pararse en
# tools/dbfiller/webapp/backend/src/dbfiller_core/ en el HOST. Copiados
# tal cual (COPY preserva symlinks) a /app/src/dbfiller_core/, el mismo
# target relativo resuelve a /src/<archivo> dentro de la imagen -- por
# eso src/ (la de tools/dbfiller/, no la de webapp/backend/) se copia
# justo ahi, no a cualquier otro lado.
WORKDIR /app
COPY webapp/backend/ .
COPY src/ /src/
RUN make

RUN echo "appuser:x:1000:1000:appuser:/home/appuser:/sbin/nologin" > /etc/passwd_app

## ETAPA 2: IMAGEN FINAL
# A diferencia del Dockerfile estandar (FROM scratch): dbfiller-web
# necesita shell + docker CLI + ab (apache2-utils) para poder correr
# "docker build"/"docker compose up"/"docker run ... psql"/"ab" como
# subprocesos (ver controllers/dbfiller_jobs.c) -- Docker-outside-of-
# Docker, controla el daemon del HOST via el socket montado
# (docker-compose.yml), no anida uno propio. Por eso el runtime es
# Alpine, no scratch.
FROM alpine:latest AS runtime
# docker-cli-compose es un paquete APARTE de docker-cli en Alpine (el
# plugin "docker compose", no viene incluido) -- confirmado con un
# intento real que fallo con "docker: unknown command: docker compose".
RUN apk add --no-cache docker-cli docker-cli-compose apache2-utils ca-certificates
COPY --from=build /etc/passwd_app /etc/passwd
COPY --from=build /app/app /bin/dbfiller-web
COPY --from=build /app/public /public

ENV PORT=8080
EXPOSE 8080
ENTRYPOINT ["/bin/dbfiller-web"]
