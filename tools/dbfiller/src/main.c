/*
 * main.c - CLI de dbfiller
 *
 * Uso:
 *   dbfiller --table <nombre> [opciones]
 *   dbfiller --all            [opciones]
 *   dbfiller --scaffold <carpeta-destino>
 *
 * Opciones:
 *   --database-url <url>  Cadena de conexion a Postgres (si se omite, usa DATABASE_URL)
 *   --repo-root <path>    Raiz del repo cerver (default: directorio actual)
 *   --force                Sobreescribe archivos aunque no tengan la marca de dbfiller
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "introspect.h"
#include "generate.h"
#include "scaffold.h"

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Uso: %s --table <nombre> [--database-url <url>] [--repo-root <path>] [--force]\n"
        "   o: %s --all            [--database-url <url>] [--repo-root <path>] [--force]\n"
        "   o: %s --scaffold <carpeta-destino>\n"
        "\n"
        "Genera controllers/<tabla>.c/.h y models/<tabla>.c/.h con endpoints CRUD\n"
        "(list/get/create/update/delete) para una tabla de Postgres, y registra las\n"
        "rutas/el modelo en routes/index.h y models/registry.h.\n"
        "\n"
        "--scaffold crea un proyecto cerver nuevo (el esqueleto reutilizable, sin\n"
        "endpoints de negocio) en <carpeta-destino>, que debe no existir o estar vacia.\n"
        "No requiere conexion a Postgres ni --repo-root: es standalone.\n"
        "\n"
        "--database-url por defecto usa la variable de entorno DATABASE_URL.\n"
        "--repo-root por defecto es el directorio actual (correr desde la raiz del repo).\n",
        prog, prog, prog);
}

/* generate_one - llama a dbfiller_generate_table (generate.h, la misma
   funcion que usa la GUI) y traduce el resultado a stdout/stderr como
   ya hacia esta funcion antes de que la orquestacion se extrajera a un
   modulo compartido. Devuelve -1 si la tabla no existe o no tiene una
   PK simple usable (no aborta --all por una tabla saltada). */
static int generate_one(PGconn *conn, const char *table_name, const char *repo_root, int force) {
    GenerateResult res;
    dbfiller_generate_table(conn, table_name, repo_root, force, &res);

    if (!res.ok) {
        fprintf(stderr, "%s: %s\n", table_name, res.message);
        return -1;
    }

    printf("%s: OK (controllers/%s.c/.h, models/%s.c/.h, rutas registradas%s)\n",
           table_name, table_name, table_name, res.has_update ? "" : ", sin update: no tiene columnas actualizables");
    return 0;
}

int main(int argc, char **argv) {
    const char *table_name = NULL;
    const char *database_url = NULL;
    const char *repo_root = ".";
    const char *scaffold_dest = NULL;
    int do_all = 0;
    int force = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--table") == 0 && i + 1 < argc) table_name = argv[++i];
        else if (strcmp(argv[i], "--all") == 0) do_all = 1;
        else if (strcmp(argv[i], "--scaffold") == 0 && i + 1 < argc) scaffold_dest = argv[++i];
        else if (strcmp(argv[i], "--database-url") == 0 && i + 1 < argc) database_url = argv[++i];
        else if (strcmp(argv[i], "--repo-root") == 0 && i + 1 < argc) repo_root = argv[++i];
        else if (strcmp(argv[i], "--force") == 0) force = 1;
        else if (strcmp(argv[i], "--help") == 0) { print_usage(argv[0]); return 0; }
        else { fprintf(stderr, "Opcion desconocida: %s\n", argv[i]); print_usage(argv[0]); return 1; }
    }

    if (scaffold_dest) {
        char err[MAX_ERROR_LEN];
        if (scaffold_new_project(scaffold_dest, err, sizeof(err)) != 0) {
            fprintf(stderr, "Error al crear el proyecto: %s\n", err);
            return 1;
        }
        printf("Proyecto nuevo creado en '%s'.\n", scaffold_dest);
        printf("Falta: copiar .env.example a .env y completar credenciales/JWT_SECRET.\n");
        return 0;
    }

    if (!table_name && !do_all) {
        print_usage(argv[0]);
        return 1;
    }

    char err[MAX_ERROR_LEN];
    PGconn *conn = pg_connect(database_url, err, sizeof(err));
    if (!conn) {
        fprintf(stderr, "Error al conectar: %s\n", err);
        return 1;
    }

    int failed = 0;
    if (do_all) {
        char names[MAX_TABLES][MAX_NAME_LEN];
        int n = pg_list_tables(conn, names, MAX_TABLES, err, sizeof(err));
        if (n < 0) {
            fprintf(stderr, "Error al listar tablas: %s\n", err);
            PQfinish(conn);
            return 1;
        }
        if (n == 0) {
            printf("El schema 'public' no tiene tablas.\n");
        }
        for (int i = 0; i < n; i++) {
            if (generate_one(conn, names[i], repo_root, force) != 0) failed++;
        }
    } else {
        if (generate_one(conn, table_name, repo_root, force) != 0) failed = 1;
    }

    PQfinish(conn);
    return failed > 0 ? 1 : 0;
}
