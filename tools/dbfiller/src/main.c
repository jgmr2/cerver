#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db/db_backend.h"
#include "db_graph.h"
#include "generator.h"

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Uso: %s --sqlite <ruta.db> [--table <nombre>] --count <N>\n"
        "   o: %s --mysql --host <h> --port <p> --user <u> --password <pw> --database <db> [--table <nombre>] --count <N>\n"
        "  --table   Genera solo esta tabla (si se omite, se llenan TODAS las\n"
        "            tablas de la base en orden de dependencias por FK)\n"
        "  --count   Numero de registros a generar por tabla\n",
        prog, prog);
}

static const char *type_name(SqlType t) {
    switch (t) {
        case SQL_TYPE_INT: return "INT";
        case SQL_TYPE_BIGINT: return "BIGINT";
        case SQL_TYPE_REAL: return "REAL";
        case SQL_TYPE_TEXT: return "TEXT";
        case SQL_TYPE_VARCHAR: return "VARCHAR";
        case SQL_TYPE_BOOL: return "BOOL";
        case SQL_TYPE_DATE: return "DATE";
        case SQL_TYPE_DATETIME: return "DATETIME";
        case SQL_TYPE_ENUM: return "ENUM";
        case SQL_TYPE_BLOB: return "BLOB";
        default: return "UNKNOWN";
    }
}

static void print_schema(const DbTable *table) {
    printf("Esquema de '%s' (%d columnas):\n", table->name, table->column_count);
    for (int i = 0; i < table->column_count; i++) {
        const DbColumn *c = &table->columns[i];
        printf("  - %-20s %-9s%s%s%s%s%s\n", c->name, type_name(c->sql_type),
               c->is_pk ? " PK" : "",
               c->is_autoincrement ? " AUTOINCREMENT" : "",
               c->nullable ? "" : " NOT NULL",
               c->is_unique ? " UNIQUE" : "",
               c->has_fk ? " FK" : "");
        if (c->has_fk) printf("      -> referencia %s.%s\n", c->fk_table, c->fk_column);
        if (c->has_enum) {
            printf("      valores permitidos: ");
            for (int j = 0; j < c->enum_count; j++) printf("%s%s", j ? ", " : "", c->enum_values[j]);
            printf("\n");
        }
    }
}

int main(int argc, char **argv) {
    const char *sqlite_path = NULL;
    const char *table_name = NULL;
    int count = -1;
    int use_mysql = 0;
    DbConnParams params = {0};

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sqlite") == 0 && i + 1 < argc) sqlite_path = argv[++i];
        else if (strcmp(argv[i], "--mysql") == 0) use_mysql = 1;
        else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) snprintf(params.host, sizeof(params.host), "%s", argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) params.port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) snprintf(params.user, sizeof(params.user), "%s", argv[++i]);
        else if (strcmp(argv[i], "--password") == 0 && i + 1 < argc) snprintf(params.password, sizeof(params.password), "%s", argv[++i]);
        else if (strcmp(argv[i], "--database") == 0 && i + 1 < argc) snprintf(params.database, sizeof(params.database), "%s", argv[++i]);
        else if (strcmp(argv[i], "--table") == 0 && i + 1 < argc) table_name = argv[++i];
        else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) count = atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0) { print_usage(argv[0]); return 0; }
    }

    if (!sqlite_path && !use_mysql) {
        print_usage(argv[0]);
        return 1;
    }

    const DbBackend *backend = db_backend_for_engine(use_mysql ? DB_ENGINE_MYSQL : DB_ENGINE_SQLITE);
    if (!backend) {
        fprintf(stderr, "Error: backend no disponible en este binario.\n");
        return 1;
    }
    if (!use_mysql) snprintf(params.path, sizeof(params.path), "%s", sqlite_path);

    char err[MAX_ERROR_LEN];
    DbConn *conn = backend->connect(&params, err, sizeof(err));
    if (!conn) {
        fprintf(stderr, "Error al conectar: %s\n", err);
        return 1;
    }
    printf("Conectado via backend '%s'.\n", backend->name);

    char names[MAX_TABLES][MAX_NAME_LEN];
    int n = backend->list_tables(conn, names, MAX_TABLES);
    if (n < 0) {
        fprintf(stderr, "Error al listar tablas: %s\n", backend->last_error(conn));
        backend->disconnect(conn);
        return 1;
    }
    if (n == 0) {
        printf("La base no tiene tablas.\n");
        backend->disconnect(conn);
        return 0;
    }

    if (count < 0) {
        printf("¿Cuantos registros deseas generar por tabla? ");
        fflush(stdout);
        if (scanf("%d", &count) != 1 || count < 0) {
            fprintf(stderr, "Cantidad invalida.\n");
            backend->disconnect(conn);
            return 1;
        }
    }

    /* --table limits the target set to just that one table; otherwise every
       table in the database gets filled, in FK dependency order. */
    char selected[MAX_TABLES][MAX_NAME_LEN];
    int selected_count;
    if (table_name) {
        snprintf(selected[0], MAX_NAME_LEN, "%s", table_name);
        selected_count = 1;
    } else {
        memcpy(selected, names, sizeof(names));
        selected_count = n;
    }

    OrderedTables ordered;
    if (db_graph_load_ordered(backend, conn, selected, selected_count, &ordered, err, sizeof(err)) != 0) {
        fprintf(stderr, "Error al leer esquema: %s\n", err);
        backend->disconnect(conn);
        return 1;
    }

    int total_inserted = 0, total_failed = 0;
    for (int i = 0; i < ordered.count; i++) {
        const DbTable *table = &ordered.tables[i];
        print_schema(table);

        GenerateStats stats;
        generate_and_insert(backend, conn, table, count, &stats);
        printf("  -> insertados: %d, fallidos: %d%s%s\n\n", stats.rows_inserted, stats.rows_failed,
               stats.rows_failed > 0 ? ", ultimo error: " : "", stats.rows_failed > 0 ? stats.last_error : "");
        total_inserted += stats.rows_inserted;
        total_failed += stats.rows_failed;
    }

    printf("--- Resultado total (%d tabla%s) ---\n", ordered.count, ordered.count == 1 ? "" : "s");
    printf("Insertados: %d\n", total_inserted);
    printf("Fallidos:   %d\n", total_failed);

    db_graph_free(&ordered);
    backend->disconnect(conn);
    return (total_failed > 0 && total_inserted == 0) ? 1 : 0;
}
