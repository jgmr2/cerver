/* Generado por dbfiller -- ver tools/dbfiller. No editar a mano si vas a re-generar. */
/* dbfiller:sha256:7a9baa650eada60cb86c92a0f194b30e146e90b835244f5c0ebb358e1d96977e */

/*
 * utils/auth/auth.c - implementacion de register_user/login_user/me
 * (ver utils/auth/auth.h)
 *
 * Generado a partir de la tabla 'user' (columna de
 * identidad 'email', ver tools/dbfiller/README.md, seccion
 * "Autenticacion"). El body de POST /api/auth/register siempre usa
 * las claves "username"/"password" (nombres fijos del framework,
 * no derivados del esquema -- el valor real detras de "username"
 * puede ser un email), mas las demas columnas requeridas por esa
 * tabla: first_name, last_name, auth_code, fk_department, fk_role.
 */
#include "auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../config/db.h"
#include "auth_model.h"
#include "../http/router.h"
#include "../http/http.h"
#include "../http/json_body.h"
#include "../net/conn_limit.h"
#include "password.h"
#include "jwt.h"
#include "login_limit.h"

#define AUTH_IDENTITY_BUF 151

/*
 * identity_is_valid - chequeo generico sobre el identificador de
 * login (puede ser un email, un username, etc. segun el esquema --
 * ver auth_model.c): largo razonable y sin comillas/backslash/
 * caracteres de control, lo minimo para poder incrustarlo sin
 * escapar en el payload JSON armado a mano de jwt_create
 * (utils/auth/jwt.c).
 */
static int identity_is_valid(const char *u, size_t len) {
    if (len < 1 || len >= AUTH_IDENTITY_BUF) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)u[i];
        if (c == '"' || c == '\\' || c < 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

/*
 * parse_credentials - extrae {"username":"...","password":"..."}
 * del body JSON de un request (login: no acepta columnas extra, a
 * diferencia de register_user).
 */
static int parse_credentials(const char *buf, char *identity_out, size_t identity_sz, char *password_out, size_t password_sz) {
    jsmntok_t tokens[16];
    const char *body = NULL;
    int ntok = http_parse_json_body(buf, tokens, sizeof(tokens) / sizeof(tokens[0]), &body);
    if (ntok < 1 || !body) return 0;
    int st_identity = json_body_field(body, tokens, ntok, "username", identity_out, identity_sz);
    int st_password = json_body_field(body, tokens, ntok, "password", password_out, password_sz);
    return st_identity == 1 && st_password == 1;
}

/*
 * issue_token_response - arma un JWT y lo manda como {"token":"..."}
 * Punto unico compartido por register_user y login_user.
 */
static void issue_token_response(struct io_uring *r, int fd, const char *id_str, const char *identity) {
    const char *secret = getenv("JWT_SECRET");
    char token[JWT_TOKEN_BUF_SIZE];
    if (!secret || !jwt_create(secret, id_str, identity, g_jwt_expires_seconds, token, sizeof(token))) {
        send_res(r, fd, "500 Internal Server Error", "text/plain", "500");
        return;
    }

    char body[JWT_TOKEN_BUF_SIZE + 64];
    snprintf(body, sizeof(body), "{\"token\":\"%s\"}", token);
    res_json(r, fd, body);
}

typedef struct {
    char identity[AUTH_IDENTITY_BUF];
} register_ctx_t;

static int on_auth_created(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {
    register_ctx_t *ctx = (register_ctx_t *)userdata;
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {
        send_res(r, client_fd, "409 Conflict", "text/plain", "409");
        free(ctx);
        return 0;
    }
    issue_token_response(r, client_fd, PQgetvalue(res, 0, 0), ctx->identity);
    free(ctx);
    return 0;
}

void register_user(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m;

    jsmntok_t tokens[18];
    const char *json_body = NULL;
    int ntok = http_parse_json_body(b, tokens, sizeof(tokens) / sizeof(tokens[0]), &json_body);
    if (ntok < 1 || !json_body) { send_res(r, f, "400 Bad Request", "text/plain", "400"); return; }

    char identity[AUTH_IDENTITY_BUF];
    int st_identity = json_body_field(json_body, tokens, ntok, "username", identity, sizeof(identity));
    char password[128];
    int st_password = json_body_field(json_body, tokens, ntok, "password", password, sizeof(password));
    char buf_first_name[256];
    int st_first_name = json_body_field(json_body, tokens, ntok, "first_name", buf_first_name, sizeof(buf_first_name));
    char buf_last_name[256];
    int st_last_name = json_body_field(json_body, tokens, ntok, "last_name", buf_last_name, sizeof(buf_last_name));
    char buf_auth_code[256];
    int st_auth_code = json_body_field(json_body, tokens, ntok, "auth_code", buf_auth_code, sizeof(buf_auth_code));
    char buf_fk_department[256];
    int st_fk_department = json_body_field(json_body, tokens, ntok, "fk_department", buf_fk_department, sizeof(buf_fk_department));
    char buf_fk_role[256];
    int st_fk_role = json_body_field(json_body, tokens, ntok, "fk_role", buf_fk_role, sizeof(buf_fk_role));

    if (st_identity != 1 ||
        !identity_is_valid(identity, strlen(identity)) ||
        st_password != 1 ||
        strlen(password) < 8 ||
        st_first_name != 1 ||
        st_last_name != 1 ||
        st_auth_code < 0 ||
        st_fk_department < 0 ||
        st_fk_role != 1) {
        explicit_bzero(password, sizeof(password));
        send_res(r, f, "400 Bad Request", "text/plain", "400");
        return;
    }

    char hash[PASSWORD_HASH_BUF_SIZE];
    int hashed = password_hash(password, hash, sizeof(hash));
    explicit_bzero(password, sizeof(password));
    if (!hashed) {
        send_res(r, f, "500 Internal Server Error", "text/plain", "500");
        return;
    }

    register_ctx_t *ctx = calloc(1, sizeof(register_ctx_t));
    if (!ctx) {
        send_res(r, f, "500 Internal Server Error", "text/plain", "500");
        return;
    }
    snprintf(ctx->identity, sizeof(ctx->identity), "%s", identity);

    const char *values[AUTH_CREATE_PARAMS];
    values[0] = identity;
    values[1] = hash;
    values[2] = (st_first_name == 1) ? buf_first_name : NULL;
    values[3] = (st_last_name == 1) ? buf_last_name : NULL;
    values[4] = (st_auth_code == 1) ? buf_auth_code : NULL;
    values[5] = (st_fk_department == 1) ? buf_fk_department : NULL;
    values[6] = (st_fk_role == 1) ? buf_fk_role : NULL;

    Auth_create_async(r, f, values, on_auth_created, ctx);
}

typedef struct {
    char identity[AUTH_IDENTITY_BUF];
    char password[128];
    uint32_t ip;
} login_ctx_t;

static void free_login_ctx(login_ctx_t *ctx) {
    explicit_bzero(ctx->password, sizeof(ctx->password));
    free(ctx);
}

static int on_auth_found_for_login(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {
    login_ctx_t *ctx = (login_ctx_t *)userdata;
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {
        login_limit_record_failure(ctx->ip);
        send_res(r, client_fd, "401 Unauthorized", "text/plain", "401");
        free_login_ctx(ctx);
        return 0;
    }

    const char *id_str = PQgetvalue(res, 0, 0);
    const char *stored_hash = PQgetvalue(res, 0, 1);

    if (!password_verify(ctx->password, stored_hash)) {
        login_limit_record_failure(ctx->ip);
        send_res(r, client_fd, "401 Unauthorized", "text/plain", "401");
        free_login_ctx(ctx);
        return 0;
    }

    login_limit_record_success(ctx->ip);
    issue_token_response(r, client_fd, id_str, ctx->identity);
    free_login_ctx(ctx);
    return 0;
}

void login_user(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m;

    uint32_t ip = conn_limit_get_ip(f);
    if (g_trust_proxy_headers) {
        uint32_t forwarded = extract_forwarded_ip(b);
        if (forwarded) ip = forwarded;
    }
    if (!login_limit_allowed(ip)) {
        send_res(r, f, "429 Too Many Requests", "text/plain", "429");
        return;
    }

    login_ctx_t *ctx = calloc(1, sizeof(login_ctx_t));
    if (!ctx) {
        send_res(r, f, "500 Internal Server Error", "text/plain", "500");
        return;
    }
    ctx->ip = ip;

    if (!parse_credentials(b, ctx->identity, sizeof(ctx->identity), ctx->password, sizeof(ctx->password))) {
        free_login_ctx(ctx);
        send_res(r, f, "400 Bad Request", "text/plain", "400");
        return;
    }

    Auth_find_by_identity_async(r, f, ctx->identity, on_auth_found_for_login, ctx);
}

void me(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m;
    (void)b;

    char body[320];
    snprintf(body, sizeof(body), "{\"sub\":\"%s\",\"username\":\"%s\"}", jwt_claim("sub"), jwt_claim("username"));
    res_json(r, f, body);
}
