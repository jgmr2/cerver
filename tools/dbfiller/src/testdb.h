/*
 * testdb.h - contenedor Postgres desechable para pruebas internas de dbfiller
 *
 * DESCRIPCION
 *     Separado a proposito de cualquier conexion externa: el campo
 *     DATABASE_URL de la pestana Tablas sigue aceptando cualquier cadena
 *     (la base real de un cliente, la de este mismo repo via
 *     docker-compose.yml, lo que sea) sin que este modulo la toque. Esto
 *     solo levanta/baja un Postgres aparte (otro nombre de proyecto de
 *     compose, otro puerto) para probar introspeccion/codegen sin
 *     depender de tener a mano una base real.
 *
 *     Todo vive en memoria (string embebido) para no depender de que el
 *     binario de dbfiller tenga un archivo .yml al lado - mismo motivo que
 *     boilerplate_zip.h. Header-only porque es un solo string constante,
 *     sin logica propia: quien lo use (gui_main.c) arma el comando
 *     "docker compose -f <tmp> -p dbfiller_test ..." igual que ya arma el
 *     resto de sus comandos Docker (ver on_docker_refresh_clicked).
 */
#ifndef DBFILLER_TESTDB_H
#define DBFILLER_TESTDB_H

#include <stdio.h>
#include <string.h>

/* Nombre de proyecto de compose fijo, distinto al del repo cerver
   (docker-compose.yml no define "name:", asi que su project name por
   defecto es el nombre de la carpeta) - evita que "down" de uno afecte
   los contenedores del otro. */
#define TESTDB_COMPOSE_PROJECT "dbfiller_testdb"
#define TESTDB_CONNINFO "postgresql://dbfiller_test:dbfiller_test@localhost:5433/dbfiller_test"

/* tmpfs para el data dir: la base es intencionalmente desechable (se
   reinicia vacia en cada "up" despues de un "down"), pensada solo para
   probar que la introspeccion/generacion de dbfiller anden bien, nunca
   para guardar nada real. */
static const char *const TESTDB_COMPOSE_YAML =
    "services:\n"
    "  db:\n"
    "    image: postgres:16-alpine\n"
    "    environment:\n"
    "      POSTGRES_USER: dbfiller_test\n"
    "      POSTGRES_PASSWORD: dbfiller_test\n"
    "      POSTGRES_DB: dbfiller_test\n"
    "    ports:\n"
    "      - \"5433:5432\"\n"
    "    tmpfs:\n"
    "      - /var/lib/postgresql/data\n"
    "    healthcheck:\n"
    "      test: [\"CMD-SHELL\", \"pg_isready -U dbfiller_test -d dbfiller_test\"]\n"
    "      interval: 2s\n"
    "      timeout: 5s\n"
    "      retries: 10\n";

/*
 * testdb_write_compose_file - vuelca TESTDB_COMPOSE_YAML a out_path
 * (tamano out_path_size). Un path fijo en /tmp alcanza: es un archivo de
 * un solo usuario local, no hace falta un nombre unico por proceso.
 *
 * Retorna 0 en exito, -1 si no se pudo escribir.
 */
static inline int testdb_write_compose_file(char *out_path, size_t out_path_size) {
    snprintf(out_path, out_path_size, "/tmp/dbfiller_testdb_compose.yml");
    FILE *f = fopen(out_path, "w");
    if (!f) return -1;
    size_t len = strlen(TESTDB_COMPOSE_YAML);
    size_t written = fwrite(TESTDB_COMPOSE_YAML, 1, len, f);
    fclose(f);
    return written == len ? 0 : -1;
}

#endif
