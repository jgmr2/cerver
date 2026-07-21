#include "dbfiller_connection.h"
#include "dbfiller_state.h"
#include "dbfiller_jobs.h"
#include "dbfiller_json.h"

#include <stdlib.h>
#include <string.h>

#include "../utils/http/router.h"
#include "../utils/http/json_body.h"
#include "../dbfiller_core/introspect.h"
#include "../dbfiller_core/testdb.h"

#define MAX_JSON_TOKENS 64

void testdb_up_handler(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m; (void)b;
    char compose_path[512];
    if (testdb_write_compose_file(compose_path, sizeof(compose_path)) != 0) {
        send_res(r, f, "500 Internal Server Error", "application/json",
                  "{\"error\":\"no se pudo escribir el compose temporal del Postgres de prueba\"}");
        return;
    }

    char cmd[700];
    snprintf(cmd, sizeof(cmd), "docker compose -f %s -p " TESTDB_COMPOSE_PROJECT " up -d --wait 2>&1", compose_path);
    int id = job_start(cmd);

    char json[64];
    snprintf(json, sizeof(json), "{\"job_id\":%d}", id);
    send_json(r, f, json);
}

void testdb_down_handler(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m; (void)b;
    char compose_path[512];
    if (testdb_write_compose_file(compose_path, sizeof(compose_path)) != 0) {
        send_res(r, f, "500 Internal Server Error", "application/json",
                  "{\"error\":\"no se pudo escribir el compose temporal del Postgres de prueba\"}");
        return;
    }

    char cmd[700];
    snprintf(cmd, sizeof(cmd), "docker compose -f %s -p " TESTDB_COMPOSE_PROJECT " down -v 2>&1", compose_path);
    int id = job_start(cmd);

    char json[64];
    snprintf(json, sizeof(json), "{\"job_id\":%d}", id);
    send_json(r, f, json);
}

/*
 * connect_handler - POST /api/connect {database_url?}
 *
 * database_url ausente o vacio -> pg_connect(NULL, ...) usa la variable
 * de entorno DATABASE_URL (mismo fallback que ya tenia
 * on_connect_clicked). Si ya habia una conexion activa la cierra primero
 * (no acumular conexiones en reconexiones sucesivas).
 */
void connect_handler(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m;
    jsmntok_t tokens[MAX_JSON_TOKENS];
    const char *body = NULL;
    int ntok = http_parse_json_body(b, tokens, MAX_JSON_TOKENS, &body);

    char database_url[700] = "";
    if (ntok > 0) json_body_field(body, tokens, ntok, "database_url", database_url, sizeof(database_url));

    char err[MAX_ERROR_LEN];
    PGconn *conn = pg_connect(database_url[0] ? database_url : NULL, err, sizeof(err));
    if (!conn) {
        char escaped[MAX_ERROR_LEN * 2];
        dbfiller_json_escape(escaped, sizeof(escaped), err);
        char json[MAX_ERROR_LEN * 2 + 64];
        snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", escaped);
        send_res(r, f, "400 Bad Request", "application/json", json);
        return;
    }

    char names[MAX_TABLES][MAX_NAME_LEN];
    int n = pg_list_tables(conn, names, MAX_TABLES, err, sizeof(err));
    if (n < 0) {
        char escaped[MAX_ERROR_LEN * 2];
        dbfiller_json_escape(escaped, sizeof(escaped), err);
        char json[MAX_ERROR_LEN * 2 + 64];
        snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", escaped);
        send_res(r, f, "400 Bad Request", "application/json", json);
        PQfinish(conn);
        return;
    }

    /* Si database_url vino vacio, pg_connect(NULL, ...) ya uso la
       variable de entorno DATABASE_URL -- se guarda esa misma (no un
       string vacio) para que las acciones que dependen de g_database_url
       como texto (docker run ... psql, ver dbfiller_schema.c/
       dbfiller_seed.c) tengan algo real con que trabajar. */
    const char *url_to_store = database_url[0] ? database_url : getenv("DATABASE_URL");

    pthread_mutex_lock(&g_conn_mutex);
    if (g_conn) PQfinish(g_conn);
    g_conn = conn;
    snprintf(g_database_url, sizeof(g_database_url), "%s", url_to_store ? url_to_store : "");
    pthread_mutex_unlock(&g_conn_mutex);

    size_t cap = (size_t)n * (MAX_NAME_LEN + 4) + 64;
    char *json = malloc(cap);
    size_t pos = (size_t)snprintf(json, cap, "{\"ok\":true,\"tables\":[");
    for (int i = 0; i < n; i++) {
        pos += (size_t)snprintf(json + pos, cap - pos, "%s\"%s\"", i ? "," : "", names[i]);
    }
    snprintf(json + pos, cap - pos, "]}");
    send_json(r, f, json);
    free(json);
}

/*
 * databases_handler - POST /api/databases {database_url}
 *
 * Conecta con dbname forzado a "postgres" (pg_conninfo_with_dbname, ver
 * introspect.h) solo para poder listar pg_database -- no toca g_conn.
 */
void databases_handler(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m;
    jsmntok_t tokens[MAX_JSON_TOKENS];
    const char *body = NULL;
    int ntok = http_parse_json_body(b, tokens, MAX_JSON_TOKENS, &body);

    char raw_url[700] = "";
    if (ntok > 0) json_body_field(body, tokens, ntok, "database_url", raw_url, sizeof(raw_url));
    if (!raw_url[0]) {
        send_res(r, f, "400 Bad Request", "application/json",
                  "{\"error\":\"database_url requerida (host/usuario/contrasena -- el nombre de base se ignora para este paso)\"}");
        return;
    }

    char err[MAX_ERROR_LEN];
    char admin_conninfo[700];
    if (pg_conninfo_with_dbname(raw_url, "postgres", admin_conninfo, sizeof(admin_conninfo), err, sizeof(err)) != 0) {
        char escaped[MAX_ERROR_LEN * 2];
        dbfiller_json_escape(escaped, sizeof(escaped), err);
        char json[MAX_ERROR_LEN * 2 + 64];
        snprintf(json, sizeof(json), "{\"error\":\"%s\"}", escaped);
        send_res(r, f, "400 Bad Request", "application/json", json);
        return;
    }

    PGconn *admin_conn = pg_connect(admin_conninfo, err, sizeof(err));
    if (!admin_conn) {
        char escaped[MAX_ERROR_LEN * 2];
        dbfiller_json_escape(escaped, sizeof(escaped), err);
        char json[MAX_ERROR_LEN * 2 + 64];
        snprintf(json, sizeof(json), "{\"error\":\"%s\"}", escaped);
        send_res(r, f, "400 Bad Request", "application/json", json);
        return;
    }

    char names[MAX_TABLES][MAX_NAME_LEN];
    int n = pg_list_databases(admin_conn, names, MAX_TABLES, err, sizeof(err));
    PQfinish(admin_conn);
    if (n < 0) {
        char escaped[MAX_ERROR_LEN * 2];
        dbfiller_json_escape(escaped, sizeof(escaped), err);
        char json[MAX_ERROR_LEN * 2 + 64];
        snprintf(json, sizeof(json), "{\"error\":\"%s\"}", escaped);
        send_res(r, f, "400 Bad Request", "application/json", json);
        return;
    }

    size_t cap = (size_t)n * (MAX_NAME_LEN + 4) + 64;
    char *json = malloc(cap);
    size_t pos = (size_t)snprintf(json, cap, "{\"databases\":[");
    for (int i = 0; i < n; i++) {
        pos += (size_t)snprintf(json + pos, cap - pos, "%s\"%s\"", i ? "," : "", names[i]);
    }
    snprintf(json + pos, cap - pos, "]}");
    send_json(r, f, json);
    free(json);
}
