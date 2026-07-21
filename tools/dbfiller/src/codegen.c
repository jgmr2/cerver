#include "codegen.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- strbuf: buffer de texto que crece llamando a sb_append repetidas
   veces (snprintf-style). Generoso a proposito (ver sb_init): un
   archivo generado nunca se acerca a este tamano salvo una tabla con
   cientos de columnas. */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} strbuf_t;

static void sb_init(strbuf_t *sb, size_t cap) {
    sb->buf = malloc(cap);
    sb->buf[0] = '\0';
    sb->cap = cap;
    sb->len = 0;
}

static void sb_append(strbuf_t *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < sb->cap - sb->len) sb->len += (size_t)n;
}

/* ---- identificadores ---- */

static void to_upper_copy(char *dst, size_t dst_sz, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < dst_sz; i++) dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = '\0';
}

static void to_pascal_copy(char *dst, size_t dst_sz, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < dst_sz; i++) dst[i] = src[i];
    dst[i] = '\0';
    if (dst[0]) dst[0] = (char)toupper((unsigned char)dst[0]);
}

/* ---- columnas sensibles: excluidas de toda tabla, sin importar su
   nombre real (agnostico a que tabla sea - no hay ningun nombre de
   tabla/archivo hardcodeado aca, solo un patron sobre el NOMBRE DE
   COLUMNA, aplicado igual sea cual sea la tabla). Motivo: un modelo
   generado expone TODAS las columnas de la tabla via JSON por diseno
   (ver PUBLIC_COLUMNS mas abajo); sin este filtro, una tabla con una
   columna tipo password_hash quedaria expuesta en texto plano por un
   GET publico apenas alguien generara esa tabla, sin que nada lo
   avisara antes de compilar. Heuristica, no garantia: sigue siendo
   responsabilidad de quien revise el diff confirmar que no haya otra
   columna sensible con un nombre que no matchee este patron. */
static const char *SENSITIVE_COLUMN_NEEDLES[] = {
    "password", "passwd", "hash", "secret", "token", "api_key", "apikey", "credential"
};
#define SENSITIVE_COLUMN_NEEDLE_COUNT (int)(sizeof(SENSITIVE_COLUMN_NEEDLES) / sizeof(SENSITIVE_COLUMN_NEEDLES[0]))

int is_sensitive_column(const char *name) {
    char lower[MAX_NAME_LEN];
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof(lower); i++) lower[i] = (char)tolower((unsigned char)name[i]);
    lower[i] = '\0';
    for (int n = 0; n < SENSITIVE_COLUMN_NEEDLE_COUNT; n++) {
        if (strstr(lower, SENSITIVE_COLUMN_NEEDLES[n])) return 1;
    }
    return 0;
}

/* ---- clasificacion de columnas (ver README/plan: insertable = sin
   default; actualizable = insertable menos la PK) ---- */

typedef struct {
    int all[MAX_COLUMNS];
    int all_count;
    int insertable[MAX_COLUMNS];
    int insertable_count;
    int updatable[MAX_COLUMNS];
    int updatable_count;
    int skipped[MAX_COLUMNS]; /* columnas sensibles excluidas, ver is_sensitive_column */
    int skipped_count;
} Classification;

static void classify(const PgTable *t, Classification *c) {
    c->all_count = c->insertable_count = c->updatable_count = c->skipped_count = 0;
    for (int i = 0; i < t->column_count; i++) {
        if (is_sensitive_column(t->columns[i].name)) {
            c->skipped[c->skipped_count++] = i;
            continue;
        }
        c->all[c->all_count++] = i;
        if (!t->columns[i].has_default) {
            c->insertable[c->insertable_count++] = i;
            if (!t->columns[i].is_pk) c->updatable[c->updatable_count++] = i;
        }
    }
}

static void join_names(char *dst, size_t dst_sz, const PgTable *t, const int *idx, int count, const char *sep) {
    size_t pos = 0;
    dst[0] = '\0';
    for (int i = 0; i < count; i++) {
        int n = snprintf(dst + pos, dst_sz - pos, "%s%s", i ? sep : "", t->columns[idx[i]].name);
        if (n > 0 && (size_t)n < dst_sz - pos) pos += (size_t)n;
    }
}

static void join_placeholders(char *dst, size_t dst_sz, int count, int start) {
    size_t pos = 0;
    dst[0] = '\0';
    for (int i = 0; i < count; i++) {
        int n = snprintf(dst + pos, dst_sz - pos, "%s$%d", i ? ", " : "", start + i);
        if (n > 0 && (size_t)n < dst_sz - pos) pos += (size_t)n;
    }
}

static void join_set_clause(char *dst, size_t dst_sz, const PgTable *t, const int *idx, int count) {
    size_t pos = 0;
    dst[0] = '\0';
    for (int i = 0; i < count; i++) {
        int n = snprintf(dst + pos, dst_sz - pos, "%s%s = $%d", i ? ", " : "", t->columns[idx[i]].name, i + 1);
        if (n > 0 && (size_t)n < dst_sz - pos) pos += (size_t)n;
    }
}

/* ---- generacion de los 4 archivos ---- */

static void gen_model_h(strbuf_t *sb, const PgTable *t, const char *TABLE, const char *Table, int has_update) {
    sb_append(sb, "%s\n", DBFILLER_GENERATED_MARKER);
    sb_append(sb,
        "/*\n"
        " * models/%s.h - modelo CRUD generado para la tabla '%s'\n"
        " */\n"
        "#ifndef MODELS_%s_H\n"
        "#define MODELS_%s_H\n\n"
        "#include <liburing.h>\n"
        "#include \"../config/db.h\"\n\n"
        "void %s_register(void);\n\n"
        "void %s_list_async(struct io_uring *r, int client_fd, cb callback, void *userdata);\n"
        "void %s_get_async(struct io_uring *r, int client_fd, const char *id, cb callback, void *userdata);\n"
        "void %s_create_async(struct io_uring *r, int client_fd, const char *const *values, cb callback, void *userdata);\n",
        t->name, t->name, TABLE, TABLE, t->name, Table, Table, Table);
    if (has_update) {
        sb_append(sb, "void %s_update_async(struct io_uring *r, int client_fd, const char *const *values, const char *id, cb callback, void *userdata);\n", Table);
    }
    sb_append(sb, "void %s_delete_async(struct io_uring *r, int client_fd, const char *id, cb callback, void *userdata);\n\n#endif\n", Table);
}

static void gen_model_c(strbuf_t *sb, const PgTable *t, const Classification *cl, const char *TABLE, const char *Table, int has_update) {
    const char *pk = t->columns[t->pk_index].name;

    char public_cols[4096];
    join_names(public_cols, sizeof(public_cols), t, cl->all, cl->all_count, ", ");

    char insertable_cols[4096], placeholders[2048];
    join_names(insertable_cols, sizeof(insertable_cols), t, cl->insertable, cl->insertable_count, ", ");
    join_placeholders(placeholders, sizeof(placeholders), cl->insertable_count, 1);

    char set_clause[4096];
    join_set_clause(set_clause, sizeof(set_clause), t, cl->updatable, cl->updatable_count);

    sb_append(sb, "%s\n#include \"%s.h\"\n\n", DBFILLER_GENERATED_MARKER, t->name);
    sb_append(sb, "#define %s_STMT_LIST   \"%s_list\"\n", TABLE, t->name);
    sb_append(sb, "#define %s_STMT_GET    \"%s_get\"\n", TABLE, t->name);
    sb_append(sb, "#define %s_STMT_CREATE \"%s_create\"\n", TABLE, t->name);
    if (has_update) sb_append(sb, "#define %s_STMT_UPDATE \"%s_update\"\n", TABLE, t->name);
    sb_append(sb, "#define %s_STMT_DELETE \"%s_delete\"\n\n", TABLE, t->name);

    if (cl->skipped_count > 0) {
        char skipped_names[2048];
        join_names(skipped_names, sizeof(skipped_names), t, cl->skipped, cl->skipped_count, ", ");
        sb_append(sb,
            "/* Excluidas automaticamente por nombre (password/hash/secret/token/\n"
            "   api_key, sin importar mayusculas, ver is_sensitive_column en\n"
            "   tools/dbfiller/src/codegen.c) - no se leen ni se aceptan via estos\n"
            "   endpoints: %s.\n"
            "   Si de verdad hace falta exponerlas, agregalas a mano a %s_COLUMNS\n"
            "   (list/get) y a los INSERT/UPDATE de este archivo (create/update). */\n",
            skipped_names, TABLE);
    }
    sb_append(sb,
        "/* Columnas expuestas via JSON en list/get/create/update. Si esta\n"
        "   tabla tiene otras columnas sensibles que el filtro automatico no\n"
        "   detecto, sacalas de aca antes de compilar. */\n"
        "#define %s_COLUMNS \"%s\"\n\n", TABLE, public_cols);

    sb_append(sb, "void %s_register(void) {\n", t->name);
    sb_append(sb,
        "    db_register_prepared(%s_STMT_LIST,\n"
        "        \"SELECT COALESCE(json_agg(row_to_json(t)), '[]'::json) FROM (SELECT \" %s_COLUMNS \" FROM %s ORDER BY %s LIMIT 100) t;\");\n\n",
        TABLE, TABLE, t->name, pk);
    sb_append(sb,
        "    db_register_prepared(%s_STMT_GET,\n"
        "        \"SELECT row_to_json(t) FROM (SELECT \" %s_COLUMNS \" FROM %s WHERE %s = $1) t;\");\n\n",
        TABLE, TABLE, t->name, pk);

    if (cl->insertable_count > 0) {
        sb_append(sb,
            "    db_register_prepared(%s_STMT_CREATE,\n"
            "        \"WITH ins AS (INSERT INTO %s (%s) VALUES (%s) RETURNING \" %s_COLUMNS \") SELECT row_to_json(ins) FROM ins;\");\n\n",
            TABLE, t->name, insertable_cols, placeholders, TABLE);
    } else {
        sb_append(sb,
            "    db_register_prepared(%s_STMT_CREATE,\n"
            "        \"WITH ins AS (INSERT INTO %s DEFAULT VALUES RETURNING \" %s_COLUMNS \") SELECT row_to_json(ins) FROM ins;\");\n\n",
            TABLE, t->name, TABLE);
    }

    if (has_update) {
        sb_append(sb,
            "    db_register_prepared(%s_STMT_UPDATE,\n"
            "        \"WITH upd AS (UPDATE %s SET %s WHERE %s = $%d RETURNING \" %s_COLUMNS \") SELECT row_to_json(upd) FROM upd;\");\n\n",
            TABLE, t->name, set_clause, pk, cl->updatable_count + 1, TABLE);
    }

    sb_append(sb, "    db_register_prepared(%s_STMT_DELETE,\n        \"DELETE FROM %s WHERE %s = $1;\");\n}\n\n", TABLE, t->name, pk);

    sb_append(sb, "void %s_list_async(struct io_uring *r, int client_fd, cb callback, void *userdata) {\n", Table);
    sb_append(sb, "    db_query_prepared_async(r, client_fd, %s_STMT_LIST, callback, userdata);\n}\n\n", TABLE);

    sb_append(sb, "void %s_get_async(struct io_uring *r, int client_fd, const char *id, cb callback, void *userdata) {\n", Table);
    sb_append(sb, "    const char *params[1] = { id };\n");
    sb_append(sb, "    db_query_prepared_params_async(r, client_fd, %s_STMT_GET, 1, params, 0, callback, userdata);\n}\n\n", TABLE);

    sb_append(sb, "void %s_create_async(struct io_uring *r, int client_fd, const char *const *values, cb callback, void *userdata) {\n", Table);
    if (cl->insertable_count > 0) {
        sb_append(sb, "    db_query_prepared_params_async(r, client_fd, %s_STMT_CREATE, %d, values, 0, callback, userdata);\n}\n\n", TABLE, cl->insertable_count);
    } else {
        sb_append(sb, "    (void)values;\n    db_query_prepared_async(r, client_fd, %s_STMT_CREATE, callback, userdata);\n}\n\n", TABLE);
    }

    if (has_update) {
        sb_append(sb, "void %s_update_async(struct io_uring *r, int client_fd, const char *const *values, const char *id, cb callback, void *userdata) {\n", Table);
        sb_append(sb, "    const char *params[%d];\n", cl->updatable_count + 1);
        sb_append(sb, "    for (int i = 0; i < %d; i++) params[i] = values[i];\n", cl->updatable_count);
        sb_append(sb, "    params[%d] = id;\n", cl->updatable_count);
        sb_append(sb, "    db_query_prepared_params_async(r, client_fd, %s_STMT_UPDATE, %d, params, 0, callback, userdata);\n}\n\n", TABLE, cl->updatable_count + 1);
    }

    sb_append(sb, "void %s_delete_async(struct io_uring *r, int client_fd, const char *id, cb callback, void *userdata) {\n", Table);
    sb_append(sb, "    const char *params[1] = { id };\n");
    sb_append(sb, "    db_query_prepared_params_async(r, client_fd, %s_STMT_DELETE, 1, params, 0, callback, userdata);\n}\n", TABLE);
}

static void gen_controller_h(strbuf_t *sb, const PgTable *t, const char *TABLE, int has_update) {
    sb_append(sb, "%s\n", DBFILLER_GENERATED_MARKER);
    sb_append(sb,
        "/*\n"
        " * controllers/%s.h - endpoints CRUD generados para la tabla '%s'\n"
        " */\n"
        "#ifndef CONTROLLERS_%s_H\n"
        "#define CONTROLLERS_%s_H\n\n"
        "#include <liburing.h>\n\n"
        "void list_%s(struct io_uring *r, int fd, const char *m, const char *b);\n"
        "void get_%s(struct io_uring *r, int fd, const char *m, const char *b);\n"
        "void create_%s(struct io_uring *r, int fd, const char *m, const char *b);\n",
        t->name, t->name, TABLE, TABLE, t->name, t->name, t->name);
    if (has_update) sb_append(sb, "void update_%s(struct io_uring *r, int fd, const char *m, const char *b);\n", t->name);
    sb_append(sb, "void delete_%s(struct io_uring *r, int fd, const char *m, const char *b);\n\n#endif\n", t->name);
}

/* Genera, para un conjunto de columnas (insertable en create, updatable
   en update), el bloque de parseo de body JSON: una linea json_body_field
   por columna + el arreglo values[] final, respetando nullable/NOT NULL. */
static void gen_body_parse_block(strbuf_t *sb, const PgTable *t, const int *idx, int count) {
    sb_append(sb, "    jsmntok_t tokens[%d];\n", count * 2 + 4);
    sb_append(sb, "    const char *json_body = NULL;\n");
    sb_append(sb, "    int ntok = http_parse_json_body(b, tokens, sizeof(tokens) / sizeof(tokens[0]), &json_body);\n");
    sb_append(sb, "    if (ntok < 1 || !json_body) { send_res(r, fd, \"400 Bad Request\", \"text/plain\", \"400\"); return; }\n\n");

    for (int i = 0; i < count; i++) {
        const PgColumn *c = &t->columns[idx[i]];
        sb_append(sb, "    char buf_%s[256];\n", c->name);
        sb_append(sb, "    int st_%s = json_body_field(json_body, tokens, ntok, \"%s\", buf_%s, sizeof(buf_%s));\n", c->name, c->name, c->name, c->name);
        sb_append(sb, "    if (st_%s < 0) { send_res(r, fd, \"400 Bad Request\", \"text/plain\", \"400\"); return; }\n", c->name);
    }
    sb_append(sb, "\n    const char *values[%d];\n", count);
    for (int i = 0; i < count; i++) {
        const PgColumn *c = &t->columns[idx[i]];
        sb_append(sb, "    if (st_%s == 0 || st_%s == 2) {\n", c->name, c->name);
        if (c->nullable) {
            sb_append(sb, "        values[%d] = NULL;\n", i);
        } else {
            sb_append(sb, "        send_res(r, fd, \"400 Bad Request\", \"text/plain\", \"400\"); return;\n");
        }
        sb_append(sb, "    } else {\n        values[%d] = buf_%s;\n    }\n", i, c->name);
    }
}

/* Genera el bloque que arma el body "{"data":...}" a partir de la fila
   0/columna 0 de res (siempre texto JSON armado por Postgres, ver
   models/<tabla>.c) y lo manda. El buffer se reserva del tamano exacto
   que hace falta (strlen + margen para el envoltorio) en vez de un
   arreglo de tamano fijo: un body mas grande que cualquier limite fijo
   arbitrario (una tabla ancha, muchas filas en el list) no debe
   truncarse en silencio. use_201 = 1 para el 201 Created de create,
   0 para el 200 OK de list/get/update (via res_json). */
static void gen_data_body_and_send(strbuf_t *sb, int use_201) {
    sb_append(sb,
        "    const char *json = PQgetvalue(res, 0, 0);\n"
        "    size_t len = strlen(json);\n"
        "    char *body = malloc(len + 16);\n"
        "    if (!body) { send_res(r, client_fd, \"500 Internal Server Error\", \"text/plain\", \"500\"); return 0; }\n"
        "    snprintf(body, len + 16, \"{\\\"data\\\":%%s}\", json);\n");
    if (use_201) {
        sb_append(sb, "    send_res(r, client_fd, \"201 Created\", \"application/json\", body);\n");
    } else {
        sb_append(sb, "    res_json(r, client_fd, body);\n");
    }
    sb_append(sb, "    free(body);\n    return 0;\n}\n\n");
}

static void gen_controller_c(strbuf_t *sb, const PgTable *t, const Classification *cl, const char *Table, int has_update) {
    sb_append(sb, "%s\n#include \"%s.h\"\n\n", DBFILLER_GENERATED_MARKER, t->name);
    sb_append(sb,
        "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n\n"
        "#include \"../config/db.h\"\n"
        "#include \"../models/%s.h\"\n"
        "#include \"../utils/http/router.h\"\n"
        "#include \"../utils/http/json_body.h\"\n\n",
        t->name);

    /* list */
    sb_append(sb,
        "static int on_%s_list(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {\n"
        "    (void)userdata;\n"
        "    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {\n"
        "        res_json(r, client_fd, \"{\\\"error\\\":\\\"%s query failed\\\"}\");\n"
        "        return 0;\n    }\n",
        t->name, t->name);
    gen_data_body_and_send(sb, 0);
    sb_append(sb,
        "void list_%s(struct io_uring *r, int fd, const char *m, const char *b) {\n"
        "    (void)m; (void)b;\n    %s_list_async(r, fd, on_%s_list, NULL);\n}\n\n",
        t->name, Table, t->name);

    /* get */
    sb_append(sb,
        "static int on_%s_get(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {\n"
        "    (void)userdata;\n"
        "    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {\n"
        "        send_res(r, client_fd, \"404 Not Found\", \"text/plain\", \"404\");\n        return 0;\n    }\n",
        t->name);
    gen_data_body_and_send(sb, 0);
    sb_append(sb,
        "void get_%s(struct io_uring *r, int fd, const char *m, const char *b) {\n"
        "    (void)m; (void)b;\n"
        "    const char *id = route_param(\"id\");\n"
        "    if (!id) { send_res(r, fd, \"400 Bad Request\", \"text/plain\", \"400\"); return; }\n"
        "    %s_get_async(r, fd, id, on_%s_get, NULL);\n}\n\n",
        t->name, Table, t->name);

    /* create */
    sb_append(sb,
        "static int on_%s_created(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {\n"
        "    (void)userdata;\n"
        "    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {\n"
        "        send_res(r, client_fd, \"409 Conflict\", \"text/plain\", \"409\");\n        return 0;\n    }\n",
        t->name);
    gen_data_body_and_send(sb, 1);
    sb_append(sb, "void create_%s(struct io_uring *r, int fd, const char *m, const char *b) {\n    (void)m;\n", t->name);
    if (cl->insertable_count > 0) {
        gen_body_parse_block(sb, t, cl->insertable, cl->insertable_count);
        sb_append(sb, "\n    %s_create_async(r, fd, values, on_%s_created, NULL);\n}\n\n", Table, t->name);
    } else {
        sb_append(sb, "    (void)b;\n    %s_create_async(r, fd, NULL, on_%s_created, NULL);\n}\n\n", Table, t->name);
    }

    /* update */
    if (has_update) {
        sb_append(sb,
            "static int on_%s_updated(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {\n"
            "    (void)userdata;\n"
            "    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {\n"
            "        send_res(r, client_fd, \"404 Not Found\", \"text/plain\", \"404\");\n        return 0;\n    }\n",
            t->name);
        gen_data_body_and_send(sb, 0);
        sb_append(sb, "void update_%s(struct io_uring *r, int fd, const char *m, const char *b) {\n    (void)m;\n", t->name);
        sb_append(sb, "    const char *id = route_param(\"id\");\n    if (!id) { send_res(r, fd, \"400 Bad Request\", \"text/plain\", \"400\"); return; }\n\n");
        gen_body_parse_block(sb, t, cl->updatable, cl->updatable_count);
        sb_append(sb, "\n    %s_update_async(r, fd, values, id, on_%s_updated, NULL);\n}\n\n", Table, t->name);
    }

    /* delete */
    sb_append(sb,
        "static int on_%s_deleted(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {\n"
        "    (void)userdata;\n"
        "    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {\n"
        "        send_res(r, client_fd, \"500 Internal Server Error\", \"text/plain\", \"500\");\n        return 0;\n    }\n"
        "    if (atoi(PQcmdTuples(res)) < 1) {\n"
        "        send_res(r, client_fd, \"404 Not Found\", \"text/plain\", \"404\");\n        return 0;\n    }\n"
        "    send_res(r, client_fd, \"200 OK\", \"text/plain\", \"ok\");\n    return 0;\n}\n\n",
        t->name);
    sb_append(sb,
        "void delete_%s(struct io_uring *r, int fd, const char *m, const char *b) {\n"
        "    (void)m; (void)b;\n"
        "    const char *id = route_param(\"id\");\n"
        "    if (!id) { send_res(r, fd, \"400 Bad Request\", \"text/plain\", \"400\"); return; }\n"
        "    %s_delete_async(r, fd, id, on_%s_deleted, NULL);\n}\n",
        t->name, Table, t->name);
}

/* ---- escritura a disco, con proteccion de archivos escritos a mano ---- */

static int file_starts_with_marker(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0; /* no existe: no hay nada que proteger */
    char line[256];
    int ok = fgets(line, sizeof(line), f) && strncmp(line, DBFILLER_GENERATED_MARKER, strlen(DBFILLER_GENERATED_MARKER)) == 0;
    fclose(f);
    return ok;
}

static int write_file(const char *path, const char *content, int force, char *err, size_t err_len) {
    struct stat st;
    if (stat(path, &st) == 0 && !force && !file_starts_with_marker(path)) {
        snprintf(err, err_len, "'%s' ya existe y no fue generado por dbfiller (falta el marcador) -- usa --force para sobreescribir", path);
        return -1;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(err, err_len, "no se pudo abrir '%s' para escritura", path);
        return -1;
    }
    fputs(content, f);
    fclose(f);
    return 0;
}

int codegen_write_table(const PgTable *table, const char *repo_root, int force, int *has_update_out, char *err, size_t err_len) {
    Classification cl;
    classify(table, &cl);
    int has_update = cl.updatable_count > 0;
    *has_update_out = has_update;

    char TABLE[MAX_NAME_LEN], Table[MAX_NAME_LEN];
    to_upper_copy(TABLE, sizeof(TABLE), table->name);
    to_pascal_copy(Table, sizeof(Table), table->name);

    strbuf_t model_h, model_c, controller_h, controller_c;
    sb_init(&model_h, 8192);
    sb_init(&model_c, 65536);
    sb_init(&controller_h, 8192);
    sb_init(&controller_c, 131072);

    gen_model_h(&model_h, table, TABLE, Table, has_update);
    gen_model_c(&model_c, table, &cl, TABLE, Table, has_update);
    gen_controller_h(&controller_h, table, TABLE, has_update);
    gen_controller_c(&controller_c, table, &cl, Table, has_update);

    char path[1024];
    int rc = 0;

    snprintf(path, sizeof(path), "%s/models/%s.h", repo_root, table->name);
    rc |= write_file(path, model_h.buf, force, err, err_len);
    if (rc == 0) {
        snprintf(path, sizeof(path), "%s/models/%s.c", repo_root, table->name);
        rc |= write_file(path, model_c.buf, force, err, err_len);
    }
    if (rc == 0) {
        snprintf(path, sizeof(path), "%s/controllers/%s.h", repo_root, table->name);
        rc |= write_file(path, controller_h.buf, force, err, err_len);
    }
    if (rc == 0) {
        snprintf(path, sizeof(path), "%s/controllers/%s.c", repo_root, table->name);
        rc |= write_file(path, controller_c.buf, force, err, err_len);
    }

    free(model_h.buf);
    free(model_c.buf);
    free(controller_h.buf);
    free(controller_c.buf);
    return rc;
}
