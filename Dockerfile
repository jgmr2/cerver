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

## ETAPA 1: COMPILACION SERVER C (TU API)
# Dependencias de compilacion: solo las que el Makefile realmente enlaza
# (ver LIBS en Makefile: -lpq -lpgcommon -lpgport -luring -lssl -lcrypto -lz).
# sqlite, libuv y jansson se usaban en versiones anteriores del proyecto
# (antes de migrar a io_uring y al parser JSON propio en utils/json.h) y ya
# no estan referenciadas por ningun .c/.h; cmake y git tampoco se usan, el
# build es un Makefile plano.
FROM alpine:latest AS build
RUN apk add --no-cache build-base linux-headers \
    openssl-dev openssl-libs-static zlib-dev zlib-static \
    liburing-dev postgresql-dev

COPY --from=pg-static-deps /opt/pgsql/lib/libpq.a /usr/lib/libpq.a
COPY --from=pg-static-deps /opt/pgsql/lib/libpgcommon.a /usr/lib/libpgcommon.a
COPY --from=pg-static-deps /opt/pgsql/lib/libpgport.a /usr/lib/libpgport.a
COPY --from=pg-static-deps /opt/pgsql/include/ /usr/include/

WORKDIR /app
COPY . .
# El codigo que se compila vive en src/ (el "boilerplate" que se replica
# en cada proyecto nuevo, ver README.md) -- tools/dbfiller/generate.py
# (corrido a mano, resultado commiteado -- ver su README) es lo que llena
# src/controllers/ y src/models/ con endpoints CRUD antes de este build,
# no una etapa de Docker: asi el codigo generado se puede editar a mano
# para agregar logica de negocio sin que un build futuro lo pise.
RUN make -C src

RUN echo "appuser:x:1000:1000:appuser:/home/appuser:/sbin/nologin" > /etc/passwd_app

## ETAPA 2: IMAGEN DE PRODUCCION (sin Swagger UI)
# Identica a "runtime" (mas abajo) salvo que no copia docs-ui/:
# mount_static("/docs", ...) (routes/index.h) sigue registrado en el
# binario (mismo binario que "runtime", ver etapa "build"), pero como la
# carpeta no existe en esta imagen, /docs devuelve 404 solo -- no hace
# falta ninguna flag de compilacion ni codigo condicional (ver TODO.md,
# "Tamaño de imagen"). Construir esta imagen explicito con:
#   docker build --target runtime-prod -t cerver:prod .
# NO es la etapa default a proposito -- tiene que ir ANTES de "runtime"
# en este archivo, porque `docker build .` sin --target siempre usa la
# ULTIMA etapa del Dockerfile, y la que tiene que seguir siendo default
# es "runtime" (con Swagger), para no romper el flujo de desarrollo
# actual con un cambio silencioso de comportamiento.
FROM scratch AS runtime-prod
COPY --from=build /etc/passwd_app /etc/passwd
COPY --from=build /app/src/app /bin/app

ENV PORT=8080
EXPOSE 8080
USER appuser
ENTRYPOINT ["./bin/app"]

## ETAPA 3: IMAGEN FINAL (dev/default -- incluye Swagger UI en /docs)
# Sin frontend de ejemplo: este boilerplate no monta nada en "/" (ver
# routes/index.h) -- si un proyecto agrega uno, agrega aca tambien su
# propio COPY --from=build /app/public /public. ULTIMA etapa del
# archivo a proposito (ver comentario de "runtime-prod" arriba): es la
# que se construye con `docker build .`/`docker compose build` sin
# --target.
FROM scratch AS runtime
COPY --from=build /etc/passwd_app /etc/passwd
COPY --from=build /app/src/app /bin/app
COPY --from=build /app/docs-ui /docs-ui

# El binario lee PORT al arrancar (ver g_port en core/server.h); si no
# esta definida usa 8080. EXPOSE es solo metadata para quien lea la
# imagen, no cambia en que puerto escucha el proceso — si se cambia PORT
# aca, hay que actualizar tambien el mapeo de puertos en docker-compose.yml.
ENV PORT=8080
EXPOSE 8080
USER appuser
ENTRYPOINT ["./bin/app"]