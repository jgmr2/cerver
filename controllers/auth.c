/*
 * controllers/auth.c - implementacion de register_user/login_user
 * (ver controllers/auth.h)
 */
#include "auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../config/db.h"
#include "../models/users.h"
#include "../utils/http/router.h"
#include "../utils/http/http.h"
#include "../utils/auth/password.h"
#include "../utils/auth/jwt.h"

/* Vida del token emitido por login/registro. Boilerplate: subir/bajar
 * segun la politica real que necesite cada proyecto. */
#define JWT_EXPIRES_SECONDS (60L * 60L * 24L) /* 24 horas */

/*
 * username_is_valid - unico chequeo de formato sobre el username
 *
 * Restringido a alfanumerico + '_'/'-', 3 a 32 caracteres. Ademas de ser
 * una restriccion razonable para un username real, evita tener que
 * escapar el username al armar a mano el payload JSON del JWT
 * (utils/auth/jwt.c): si pudiera traer comillas o backslash, el token
 * emitido tendria un payload JSON invalido.
 */
static int username_is_valid(const char *u, size_t len) {
    if (len < 3 || len > 32) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = u[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return 0;
    }
    return 1;
}

/*
 * parse_credentials - extrae {"username":"...","password":"..."} del
 * body JSON de un request
 *
 * Parametros:
 *   buf           - buffer crudo del request (headers + body)
 *   username_out  - buffer de salida para el username
 *   username_sz   - tamano de username_out
 *   password_out  - buffer de salida para la contrasena
 *   password_sz   - tamano de password_out
 *
 * Retorna:
 *   1 si el body es JSON valido y trae ambos campos como strings que
 *   entran en los buffers de salida, 0 en cualquier otro caso.
 */
static int parse_credentials(const char *buf, char *username_out, size_t username_sz, char *password_out, size_t password_sz) {
    jsmntok_t tokens[16];
    const char *body = NULL;
    int ntok = http_parse_json_body(buf, tokens, 16, &body);
    if (ntok < 1 || !body || tokens[0].type != JSMN_OBJECT) return 0;

    int got_username = 0, got_password = 0;
    for (int i = 1; i + 1 < ntok; i += 2) {
        jsmntok_t *key = &tokens[i];
        jsmntok_t *val = &tokens[i + 1];
        if (val->type != JSMN_STRING) continue;
        size_t vlen = (size_t)(val->end - val->start);

        if (json_key_eq(body, key, "username")) {
            if (vlen >= username_sz) return 0;
            memcpy(username_out, body + val->start, vlen);
            username_out[vlen] = '\0';
            got_username = 1;
        } else if (json_key_eq(body, key, "password")) {
            if (vlen >= password_sz) return 0;
            memcpy(password_out, body + val->start, vlen);
            password_out[vlen] = '\0';
            got_password = 1;
        }
    }
    return got_username && got_password;
}

/*
 * issue_token_response - arma un JWT y lo manda como {"token":"..."}
 *
 * Punto unico compartido por register_user y login_user para no
 * duplicar la construccion de la respuesta de exito.
 *
 * Parametros:
 *   r        - anillo io_uring del hilo actual
 *   fd       - file descriptor del cliente
 *   id_str   - id de usuario (columna de Postgres, ya en texto) para el
 *              claim "sub"
 *   username - claim "username"
 */
static void issue_token_response(struct io_uring *r, int fd, const char *id_str, const char *username) {
    const char *secret = getenv("JWT_SECRET");
    char token[JWT_TOKEN_BUF_SIZE];
    if (!secret || !jwt_create(secret, id_str, username, JWT_EXPIRES_SECONDS, token, sizeof(token))) {
        send_res(r, fd, "500 Internal Server Error", "text/plain", "500");
        return;
    }

    char body[JWT_TOKEN_BUF_SIZE + 64];
    snprintf(body, sizeof(body), "{\"token\":\"%s\"}", token);
    res_json(r, fd, body);
}

/* Contexto que register_user necesita conservar hasta que la insercion
 * termine: el username, para el claim del JWT (el password ya se
 * hasheo antes de disparar la consulta, no hace falta conservarlo). */
typedef struct {
    char username[64];
} register_ctx_t;

static int on_user_created(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {
    register_ctx_t *ctx = (register_ctx_t *)userdata;

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {
        /* Causa mas probable: UNIQUE(username) ya existe. No se
         * distingue de otros errores de insercion en la respuesta, para
         * no revelar de mas sobre el motivo exacto de la falla. */
        send_res(r, client_fd, "409 Conflict", "text/plain", "409");
        free(ctx);
        return 0;
    }

    issue_token_response(r, client_fd, PQgetvalue(res, 0, 0), ctx->username);
    free(ctx);
    return 0;
}

void register_user(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m;

    char username[64], password[128];
    if (!parse_credentials(b, username, sizeof(username), password, sizeof(password)) ||
        !username_is_valid(username, strlen(username)) ||
        strlen(password) < 8) {
        send_res(r, f, "400 Bad Request", "text/plain", "400");
        return;
    }

    char hash[PASSWORD_HASH_BUF_SIZE];
    if (!password_hash(password, hash, sizeof(hash))) {
        send_res(r, f, "500 Internal Server Error", "text/plain", "500");
        return;
    }

    register_ctx_t *ctx = calloc(1, sizeof(register_ctx_t));
    if (!ctx) {
        send_res(r, f, "500 Internal Server Error", "text/plain", "500");
        return;
    }
    snprintf(ctx->username, sizeof(ctx->username), "%s", username);

    Users_create_async(r, f, username, hash, on_user_created, ctx);
}

/* Contexto que login_user necesita conservar hasta que la busqueda
 * termine: username (para el claim del JWT) y la contrasena en texto
 * plano (para verificarla contra el hash que devuelva la consulta). */
typedef struct {
    char username[64];
    char password[128];
} login_ctx_t;

static int on_user_found_for_login(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {
    login_ctx_t *ctx = (login_ctx_t *)userdata;

    /* Misma respuesta (401) tanto si el usuario no existe como si la
     * contrasena no coincide: no hay que dejarle saber a quien intenta
     * entrar si el username que probo existe o no. */
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {
        send_res(r, client_fd, "401 Unauthorized", "text/plain", "401");
        free(ctx);
        return 0;
    }

    const char *id_str = PQgetvalue(res, 0, 0);
    const char *stored_hash = PQgetvalue(res, 0, 1);

    if (!password_verify(ctx->password, stored_hash)) {
        send_res(r, client_fd, "401 Unauthorized", "text/plain", "401");
        free(ctx);
        return 0;
    }

    issue_token_response(r, client_fd, id_str, ctx->username);
    free(ctx);
    return 0;
}

void login_user(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m;

    login_ctx_t *ctx = calloc(1, sizeof(login_ctx_t));
    if (!ctx) {
        send_res(r, f, "500 Internal Server Error", "text/plain", "500");
        return;
    }

    if (!parse_credentials(b, ctx->username, sizeof(ctx->username), ctx->password, sizeof(ctx->password))) {
        free(ctx);
        send_res(r, f, "400 Bad Request", "text/plain", "400");
        return;
    }

    Users_find_by_username_async(r, f, ctx->username, on_user_found_for_login, ctx);
}

void me(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m;
    (void)b;

    /* dispatch() (utils/http/router.h) ya valido el JWT antes de llegar
     * aca (ruta registrada con get_auth): jwt_claim() siempre tiene algo
     * que devolver en este punto. */
    char body[192];
    snprintf(body, sizeof(body), "{\"sub\":\"%s\",\"username\":\"%s\"}", jwt_claim("sub"), jwt_claim("username"));
    res_json(r, f, body);
}
