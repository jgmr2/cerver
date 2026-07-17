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
RUN make

RUN echo "appuser:x:1000:1000:appuser:/home/appuser:/sbin/nologin" > /etc/passwd_app

## ETAPA 2: IMAGEN FINAL
# public/ es contenido estatico plano (HTML/CSS servido directo por static.h,
# ver mount_static en routes/index.h), no el resultado de un build de Svelte:
# se copia tal cual desde el repo, sin ninguna etapa de compilacion de
# frontend previa.
FROM scratch AS runtime
COPY --from=build /etc/passwd_app /etc/passwd
COPY --from=build /app/app /bin/app
COPY --from=build /app/public /public
COPY --from=build /app/docs-ui /docs-ui

# El binario lee PORT al arrancar (ver g_port en core/server.h); si no
# esta definida usa 8080. EXPOSE es solo metadata para quien lea la
# imagen, no cambia en que puerto escucha el proceso — si se cambia PORT
# aca, hay que actualizar tambien el mapeo de puertos en docker-compose.yml.
ENV PORT=8080
EXPOSE 8080
USER appuser
ENTRYPOINT ["./bin/app"]