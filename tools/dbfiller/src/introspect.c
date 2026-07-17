#include "introspect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PGconn *pg_connect(const char *conninfo, char *err, size_t err_len) {
    const char *conn_str = conninfo ? conninfo : getenv("DATABASE_URL");
    if (!conn_str || !*conn_str) {
        snprintf(err, err_len, "falta la cadena de conexion: pasa --database-url o define DATABASE_URL");
        return NULL;
    }

    PGconn *conn = PQconnectdb(conn_str);
    if (PQstatus(conn) != CONNECTION_OK) {
        snprintf(err, err_len, "%s", PQerrorMessage(conn));
        PQfinish(conn);
        return NULL;
    }
    return conn;
}

int pg_list_tables(PGconn *conn, char out_names[][MAX_NAME_LEN], int max_tables, char *err, size_t err_len) {
    const char *sql =
        "SELECT table_name FROM information_schema.tables "
        "WHERE table_schema = 'public' AND table_type = 'BASE TABLE' "
        "ORDER BY table_name;";

    PGresult *res = PQexec(conn, sql);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        snprintf(err, err_len, "%s", PQerrorMessage(conn));
        PQclear(res);
        return -1;
    }

    int n = PQntuples(res);
    int count = n < max_tables ? n : max_tables;
    for (int i = 0; i < count; i++) {
        snprintf(out_names[i], MAX_NAME_LEN, "%s", PQgetvalue(res, i, 0));
    }
    PQclear(res);
    return count;
}

/*
 * load_columns - llena out->columns/column_count desde information_schema.columns
 *
 * Retorna 0 en exito (incluido el caso de 0 columnas, que el llamador
 * interpreta como "la tabla no existe"), -1 si la consulta misma fallo.
 */
static int load_columns(PGconn *conn, const char *table_name, PgTable *out, char *err, size_t err_len) {
    const char *sql =
        "SELECT column_name, udt_name, is_nullable, (column_default IS NOT NULL) AS has_default "
        "FROM information_schema.columns "
        "WHERE table_schema = 'public' AND table_name = $1 "
        "ORDER BY ordinal_position;";
    const char *params[1] = { table_name };

    PGresult *res = PQexecParams(conn, sql, 1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        snprintf(err, err_len, "%s", PQerrorMessage(conn));
        PQclear(res);
        return -1;
    }

    int n = PQntuples(res);
    if (n > MAX_COLUMNS) n = MAX_COLUMNS;
    out->column_count = n;
    for (int i = 0; i < n; i++) {
        PgColumn *c = &out->columns[i];
        snprintf(c->name, MAX_NAME_LEN, "%s", PQgetvalue(res, i, 0));
        snprintf(c->data_type, MAX_NAME_LEN, "%s", PQgetvalue(res, i, 1));
        c->nullable = strcmp(PQgetvalue(res, i, 2), "YES") == 0;
        c->has_default = strcmp(PQgetvalue(res, i, 3), "t") == 0;
        c->is_pk = 0;
    }
    PQclear(res);
    return 0;
}

/*
 * load_pk - marca la columna PK en out->columns (via is_pk) y fija
 * out->pk_index; deja pk_index en -1 si no hay PK o es compuesta.
 *
 * Retorna 0 en exito (incluido "sin PK usable"), -1 si la consulta
 * misma fallo.
 */
static int load_pk(PGconn *conn, const char *table_name, PgTable *out, char *err, size_t err_len) {
    const char *sql =
        "SELECT kcu.column_name "
        "FROM information_schema.table_constraints tc "
        "JOIN information_schema.key_column_usage kcu "
        "  ON tc.constraint_name = kcu.constraint_name AND tc.table_schema = kcu.table_schema "
        "WHERE tc.table_schema = 'public' AND tc.table_name = $1 AND tc.constraint_type = 'PRIMARY KEY';";
    const char *params[1] = { table_name };

    PGresult *res = PQexecParams(conn, sql, 1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        snprintf(err, err_len, "%s", PQerrorMessage(conn));
        PQclear(res);
        return -1;
    }

    out->pk_index = -1;
    int n = PQntuples(res);
    if (n == 1) {
        const char *pk_name = PQgetvalue(res, 0, 0);
        for (int i = 0; i < out->column_count; i++) {
            if (strcmp(out->columns[i].name, pk_name) == 0) {
                out->columns[i].is_pk = 1;
                out->pk_index = i;
                break;
            }
        }
    }
    PQclear(res);
    return 0;
}

int pg_load_table(PGconn *conn, const char *table_name, PgTable *out, char *err, size_t err_len) {
    memset(out, 0, sizeof(*out));
    snprintf(out->name, MAX_NAME_LEN, "%s", table_name);
    out->pk_index = -1;

    if (load_columns(conn, table_name, out, err, err_len) != 0) return -1;
    if (out->column_count == 0) {
        snprintf(err, err_len, "la tabla '%s' no existe (o no tiene columnas) en el schema 'public'", table_name);
        return -1;
    }
    if (load_pk(conn, table_name, out, err, err_len) != 0) return -1;

    return 0;
}
