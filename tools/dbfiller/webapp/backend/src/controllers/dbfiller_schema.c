#include "dbfiller_schema.h"
#include "dbfiller_state.h"
#include "dbfiller_jobs.h"
#include "dbfiller_json.h"
#include "dbfiller_shell.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../utils/http/router.h"
#include "../utils/http/json_body.h"

#define MAX_JSON_TOKENS 64
#define MAX_SCHEMA_FILES 256

/* is_safe_filename - sin '/' (nada de subcarpetas ni traversal) y sin
   arrancar con '.' (nada de dotfiles ni ".."). A diferencia de la GUI
   GTK4 (donde 'filename' siempre salia de un readdir() propio o de un
   GtkFileDialog nativo, nunca de texto arbitrario), aca 'filename' llega
   directo del body JSON de un cliente HTTP -- superficie de ataque nueva
   que la version de escritorio no tenia. */
static int is_safe_filename(const char *name) {
    if (!name || !*name) return 0;
    if (name[0] == '.') return 0;
    if (strchr(name, '/')) return 0;
    return 1;
}

static int cmp_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* next_schema_prefix - mismo algoritmo que next_schema_prefix() de la
   vieja GUI GTK4: siguiente prefijo numerico libre en db/init/,
   arrancando en 2 (1 es 01_auth_schema.sql, infraestructura fija del
   boilerplate). */
static int next_schema_prefix(const char *repo_root) {
    char dir_path[1024];
    snprintf(dir_path, sizeof(dir_path), "%s/db/init", repo_root);

    DIR *dir = opendir(dir_path);
    if (!dir) return 2;

    int max_prefix = 1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int n;
        if (sscanf(entry->d_name, "%2d_", &n) == 1 && n > max_prefix) max_prefix = n;
    }
    closedir(dir);
    return max_prefix + 1;
}

void schema_files_handler(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m; (void)b;
    char dir_path[1024];
    snprintf(dir_path, sizeof(dir_path), "%s/db/init", dbfiller_repo_root());

    char names[MAX_SCHEMA_FILES][256];
    int count = 0;

    DIR *dir = opendir(dir_path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && count < MAX_SCHEMA_FILES) {
            size_t len = strlen(entry->d_name);
            if (len < 4 || strcmp(entry->d_name + len - 4, ".sql") != 0) continue;
            if (strcmp(entry->d_name, "01_auth_schema.sql") == 0) continue;
            snprintf(names[count++], sizeof(names[0]), "%s", entry->d_name);
        }
        closedir(dir);
    }
    qsort(names, (size_t)count, sizeof(names[0]), cmp_names);

    size_t cap = (size_t)count * 280 + 32;
    char *json = malloc(cap);
    size_t pos = (size_t)snprintf(json, cap, "{\"files\":[");
    for (int i = 0; i < count; i++) {
        pos += (size_t)snprintf(json + pos, cap - pos, "%s\"%s\"", i ? "," : "", names[i]);
    }
    snprintf(json + pos, cap - pos, "]}");
    send_json(r, f, json);
    free(json);
}

/*
 * apply_schema_cmd - arma el comando "docker run ... psql < archivo" que
 * ya usaba apply_schema_file() de la GUI GTK4 (mismo motivo documentado
 * ahi: el host/contenedor no tiene psql nativo, se usa un
 * postgres:16-alpine desechable con --network=host).
 */
static void apply_schema_cmd(char *cmd, size_t cmd_sz, const char *db_url, const char *sql_path) {
    char q_url[DBFILLER_DB_URL_LEN + 8];
    char q_path[1200 + 8];
    shell_quote_into(q_url, sizeof(q_url), db_url);
    shell_quote_into(q_path, sizeof(q_path), sql_path);
    snprintf(cmd, cmd_sz, "docker run --rm -i --network=host postgres:16-alpine psql %s < %s 2>&1", q_url, q_path);
}

void schema_apply_handler(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m;
    if (!g_database_url[0]) {
        send_res(r, f, "400 Bad Request", "application/json",
                  "{\"error\":\"conecta primero -- el esquema se aplica contra la base activa\"}");
        return;
    }

    jsmntok_t tokens[MAX_JSON_TOKENS];
    const char *body = NULL;
    int ntok = http_parse_json_body(b, tokens, MAX_JSON_TOKENS, &body);
    char filename[256] = "";
    if (ntok > 0) json_body_field(body, tokens, ntok, "filename", filename, sizeof(filename));

    if (!is_safe_filename(filename)) {
        send_res(r, f, "400 Bad Request", "application/json", "{\"error\":\"filename invalido\"}");
        return;
    }

    char sql_path[1200];
    snprintf(sql_path, sizeof(sql_path), "%s/db/init/%s", dbfiller_repo_root(), filename);

    char cmd[2600];
    apply_schema_cmd(cmd, sizeof(cmd), g_database_url, sql_path);
    int id = job_start(cmd);

    char json[64];
    snprintf(json, sizeof(json), "{\"job_id\":%d}", id);
    send_json(r, f, json);
}

/*
 * schema_upload_handler - POST /api/schema/upload {filename, content}
 *
 * 'filename' es el nombre base elegido por el cliente (sin prefijo
 * numerico); el servidor le antepone el proximo prefijo NN_ libre (mismo
 * criterio que on_schema_file_chosen de la GUI GTK4) y escribe el
 * archivo en db/init/. No aplica el esquema solo -- el cliente llama a
 * /api/schema/apply aparte con el nombre final devuelto, mismo patron de
 * dos pasos que ya exponen el resto de los endpoints (nada mágico
 * encadenado del lado del servidor).
 */
void schema_upload_handler(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m;
    jsmntok_t tokens[MAX_JSON_TOKENS];
    const char *body = NULL;
    int ntok = http_parse_json_body(b, tokens, MAX_JSON_TOKENS, &body);

    char filename[256] = "";
    static char raw_content[65536];
    static char content[65536];
    raw_content[0] = '\0';
    if (ntok > 0) {
        json_body_field(body, tokens, ntok, "filename", filename, sizeof(filename));
        json_body_field(body, tokens, ntok, "content", raw_content, sizeof(raw_content));
    }

    if (!is_safe_filename(filename) || strstr(filename, ".sql") == NULL) {
        send_res(r, f, "400 Bad Request", "application/json", "{\"error\":\"filename invalido (debe terminar en .sql)\"}");
        return;
    }
    if (!raw_content[0]) {
        send_res(r, f, "400 Bad Request", "application/json", "{\"error\":\"content vacio\"}");
        return;
    }
    dbfiller_json_unescape(content, sizeof(content), raw_content);

    const char *repo_root = dbfiller_repo_root();
    char dir_path[1024];
    snprintf(dir_path, sizeof(dir_path), "%s/db/init", repo_root);
    /* mkdir -p de un solo nivel (db/init puede no existir todavia si
       "Raiz del repo" apunta a un proyecto nuevo) -- si repo_root
       tampoco existe, este mkdir tambien falla y el fopen de abajo lo
       reporta con un error claro. */
    mkdir(dir_path, 0755);
    char parent[1024];
    snprintf(parent, sizeof(parent), "%s/db", repo_root);
    mkdir(parent, 0755);
    mkdir(dir_path, 0755);

    int prefix = next_schema_prefix(repo_root);
    char dest_name[300];
    snprintf(dest_name, sizeof(dest_name), "%02d_%s", prefix, filename);
    char dest_path[1200];
    snprintf(dest_path, sizeof(dest_path), "%s/%s", dir_path, dest_name);

    FILE *fp = fopen(dest_path, "w");
    if (!fp) {
        char json[600];
        snprintf(json, sizeof(json), "{\"error\":\"no se pudo escribir '%s': %s\"}", dest_path, strerror(errno));
        send_res(r, f, "500 Internal Server Error", "application/json", json);
        return;
    }
    fputs(content, fp);
    fclose(fp);

    char escaped[600];
    dbfiller_json_escape(escaped, sizeof(escaped), dest_name);
    char json[700];
    snprintf(json, sizeof(json), "{\"filename\":\"%s\"}", escaped);
    send_json(r, f, json);
}
