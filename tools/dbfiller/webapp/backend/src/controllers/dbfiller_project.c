#include "dbfiller_project.h"
#include "dbfiller_state.h"
#include "dbfiller_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../utils/http/router.h"
#include "../utils/http/json_body.h"
#include "../dbfiller_core/scaffold.h"
#include "../dbfiller_core/introspect.h" /* MAX_ERROR_LEN */

#define MAX_JSON_TOKENS 32

void project_get_handler(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m; (void)b;
    char escaped[DBFILLER_REPO_ROOT_LEN * 2];
    dbfiller_json_escape(escaped, sizeof(escaped), g_repo_root);
    char json[DBFILLER_REPO_ROOT_LEN * 2 + 32];
    snprintf(json, sizeof(json), "{\"repo_root\":\"%s\"}", escaped);
    send_json(r, f, json);
}

void project_set_handler(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m;
    jsmntok_t tokens[MAX_JSON_TOKENS];
    const char *body = NULL;
    int ntok = http_parse_json_body(b, tokens, MAX_JSON_TOKENS, &body);

    char repo_root[DBFILLER_REPO_ROOT_LEN] = "";
    if (ntok > 0) json_body_field(body, tokens, ntok, "repo_root", repo_root, sizeof(repo_root));

    pthread_mutex_lock(&g_conn_mutex);
    snprintf(g_repo_root, sizeof(g_repo_root), "%s", repo_root);
    pthread_mutex_unlock(&g_conn_mutex);

    send_json(r, f, "{\"ok\":true}");
}

void project_scaffold_handler(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m;
    jsmntok_t tokens[MAX_JSON_TOKENS];
    const char *body = NULL;
    int ntok = http_parse_json_body(b, tokens, MAX_JSON_TOKENS, &body);

    char dest[DBFILLER_REPO_ROOT_LEN] = "";
    if (ntok > 0) json_body_field(body, tokens, ntok, "dest", dest, sizeof(dest));
    if (!dest[0]) {
        send_res(r, f, "400 Bad Request", "application/json", "{\"error\":\"dest requerido\"}");
        return;
    }

    char err[MAX_ERROR_LEN];
    if (scaffold_new_project(dest, err, sizeof(err)) != 0) {
        char escaped[MAX_ERROR_LEN * 2];
        dbfiller_json_escape(escaped, sizeof(escaped), err);
        char json[MAX_ERROR_LEN * 2 + 64];
        snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", escaped);
        send_res(r, f, "400 Bad Request", "application/json", json);
        return;
    }
    send_json(r, f, "{\"ok\":true}");
}

/* read_env_value - mismo algoritmo que la vieja GUI GTK4 (gui_main.c):
   busca "KEY=valor" en <repo_root>/.env, una asignacion por linea, sin
   espacios alrededor del '='. */
static int read_env_value(const char *repo_root, const char *key, char *out, size_t out_sz) {
    out[0] = '\0';
    char path[1024];
    snprintf(path, sizeof(path), "%s/.env", repo_root);

    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    char line[512];
    size_t key_len = strlen(key);
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            char *val = line + key_len + 1;
            size_t len = strlen(val);
            while (len > 0 && (val[len - 1] == '\n' || val[len - 1] == '\r')) val[--len] = '\0';
            snprintf(out, out_sz, "%s", val);
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

void env_get_handler(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m; (void)b;
    const char *repo_root = dbfiller_repo_root();

    char user[128], password[128], db[128], jwt[128];
    read_env_value(repo_root, "POSTGRES_USER", user, sizeof(user));
    read_env_value(repo_root, "POSTGRES_PASSWORD", password, sizeof(password));
    read_env_value(repo_root, "POSTGRES_DB", db, sizeof(db));
    read_env_value(repo_root, "JWT_SECRET", jwt, sizeof(jwt));

    char e_user[256], e_password[256], e_db[256], e_jwt[256];
    dbfiller_json_escape(e_user, sizeof(e_user), user);
    dbfiller_json_escape(e_password, sizeof(e_password), password);
    dbfiller_json_escape(e_db, sizeof(e_db), db);
    dbfiller_json_escape(e_jwt, sizeof(e_jwt), jwt);

    char json[1200];
    snprintf(json, sizeof(json), "{\"user\":\"%s\",\"password\":\"%s\",\"db\":\"%s\",\"jwt\":\"%s\"}",
             e_user, e_password, e_db, e_jwt);
    send_json(r, f, json);
}

/* random_hex - mismo algoritmo que la vieja GUI GTK4: n_bytes de
   /dev/urandom, hex-encodados. Para POSTGRES_PASSWORD/JWT_SECRET por
   defecto hace falta aleatoriedad real, no rand()/g_random_int. */
static int random_hex(char *out, size_t out_size, int n_bytes) {
    unsigned char buf[64];
    if ((size_t)n_bytes > sizeof(buf) || out_size < (size_t)(n_bytes * 2 + 1)) return -1;

    FILE *fp = fopen("/dev/urandom", "rb");
    if (!fp) return -1;
    size_t got = fread(buf, 1, (size_t)n_bytes, fp);
    fclose(fp);
    if (got != (size_t)n_bytes) return -1;

    for (int i = 0; i < n_bytes; i++) snprintf(out + i * 2, 3, "%02x", buf[i]);
    return 0;
}

void env_defaults_handler(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m; (void)b;
    char password[32] = "", jwt[80] = "";
    random_hex(password, sizeof(password), 12);
    random_hex(jwt, sizeof(jwt), 32);

    char json[256];
    snprintf(json, sizeof(json), "{\"user\":\"app\",\"db\":\"app\",\"password\":\"%s\",\"jwt\":\"%s\"}", password, jwt);
    send_json(r, f, json);
}

void env_save_handler(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m;
    jsmntok_t tokens[MAX_JSON_TOKENS];
    const char *body = NULL;
    int ntok = http_parse_json_body(b, tokens, MAX_JSON_TOKENS, &body);

    char user[128] = "", password[128] = "", db[128] = "", jwt[128] = "", overwrite_str[8] = "";
    if (ntok > 0) {
        json_body_field(body, tokens, ntok, "user", user, sizeof(user));
        json_body_field(body, tokens, ntok, "password", password, sizeof(password));
        json_body_field(body, tokens, ntok, "db", db, sizeof(db));
        json_body_field(body, tokens, ntok, "jwt", jwt, sizeof(jwt));
        json_body_field(body, tokens, ntok, "overwrite", overwrite_str, sizeof(overwrite_str));
    }
    int overwrite = strcmp(overwrite_str, "true") == 0;

    if (!user[0] || !password[0] || !db[0] || !jwt[0]) {
        send_res(r, f, "400 Bad Request", "application/json",
                  "{\"error\":\"faltan POSTGRES_USER/POSTGRES_PASSWORD/POSTGRES_DB/JWT_SECRET (o usa /api/env/defaults)\"}");
        return;
    }

    const char *repo_root = dbfiller_repo_root();
    char env_path[1200];
    snprintf(env_path, sizeof(env_path), "%s/.env", repo_root);

    FILE *existing = fopen(env_path, "r");
    if (existing) {
        fclose(existing);
        if (!overwrite) {
            char escaped[1300];
            dbfiller_json_escape(escaped, sizeof(escaped), env_path);
            char json[1400];
            snprintf(json, sizeof(json), "{\"error\":\"ya existe '%s' -- mandar overwrite:true si de verdad se quiere reemplazar\"}", escaped);
            send_res(r, f, "409 Conflict", "application/json", json);
            return;
        }
    }

    char content[4096];
    snprintf(content, sizeof(content),
        "# Credenciales\n"
        "POSTGRES_USER=%s\n"
        "POSTGRES_PASSWORD=%s\n"
        "POSTGRES_DB=%s\n"
        "\n"
        "# Unix Domain Socket compartido con el contenedor de Postgres (ver\n"
        "# docker-compose.yml, volumen pg_socket) - no lleva host:puerto.\n"
        "DATABASE_URL=postgres://%s:%s@/%s?host=/var/run/postgresql\n"
        "\n"
        "# Secreto para firmar/verificar JWT (HS256, ver utils/auth/jwt.c).\n"
        "JWT_SECRET=%s\n"
        "\n"
        "# Tunables operativos opcionales - si no se definen aca, docker-compose.yml\n"
        "# les pone el default que se ve al lado de cada una.\n"
        "# PORT=8080\n"
        "# SHUTDOWN_GRACE_SECONDS=5\n"
        "# CACHE_REFRESH_SECONDS=30\n"
        "# DB_CONNECT_TIMEOUT_SECONDS=3\n"
        "# JWT_EXPIRES_SECONDS=86400\n"
        "# PBKDF2_ITERATIONS=100000\n"
        "# DB_POOL_SIZE=16\n"
        "# DB_PENDING_QUEUE_SIZE=8192\n",
        user, password, db, user, password, db, jwt);

    FILE *fp = fopen(env_path, "w");
    if (!fp) {
        char escaped[1300];
        dbfiller_json_escape(escaped, sizeof(escaped), env_path);
        char json[1400];
        snprintf(json, sizeof(json), "{\"error\":\"no se pudo escribir '%s'\"}", escaped);
        send_res(r, f, "500 Internal Server Error", "application/json", json);
        return;
    }
    fputs(content, fp);
    fclose(fp);

    send_json(r, f, "{\"ok\":true}");
}
