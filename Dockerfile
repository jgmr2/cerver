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

## ETAPA 1: COMPILACIÓN SERVER C (TU API)
FROM alpine:latest AS build
RUN apk add --no-cache build-base cmake git linux-headers \
    openssl-dev openssl-libs-static zlib-dev zlib-static \
    sqlite-dev sqlite-static libuv-dev libuv-static \
    jansson-dev ca-certificates liburing-dev postgresql-dev

COPY --from=pg-static-deps /opt/pgsql/lib/libpq.a /usr/lib/libpq.a
COPY --from=pg-static-deps /opt/pgsql/lib/libpgcommon.a /usr/lib/libpgcommon.a
COPY --from=pg-static-deps /opt/pgsql/lib/libpgport.a /usr/lib/libpgport.a
COPY --from=pg-static-deps /opt/pgsql/include/ /usr/include/

WORKDIR /app
COPY . . 
RUN make

RUN echo "appuser:x:1000:1000:appuser:/home/appuser:/sbin/nologin" > /etc/passwd_app



## ETAPA 4: IMAGEN FINAL, CARGA LA BUILD DE SVELTE CON SUS ASSETS PARA QUE EL SERVER C LA SIRVA
FROM scratch AS runtime
COPY --from=build /etc/passwd_app /etc/passwd
COPY --from=build /app/app /bin/app


EXPOSE 80
USER appuser
ENTRYPOINT ["./bin/app"]