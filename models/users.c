/*
 * models/users.c - implementacion de users_register y las consultas
 * asincronas de usuarios (ver models/users.h)
 */
#include "users.h"

#define USERS_STMT_FIND_BY_USERNAME "users_find_by_username"
#define USERS_STMT_CREATE "users_create"

void users_register(void) {
    db_register_prepared(USERS_STMT_FIND_BY_USERNAME,
        "SELECT id, password_hash FROM users WHERE username = $1;");

    db_register_prepared(USERS_STMT_CREATE,
        "INSERT INTO users (username, password_hash) VALUES ($1, $2) RETURNING id;");
}

void Users_find_by_username_async(struct io_uring *r, int client_fd, const char *username, cb callback, void *userdata) {
    const char *params[1] = { username };
    db_query_prepared_params_async(r, client_fd, USERS_STMT_FIND_BY_USERNAME, 1, params, 0, callback, userdata);
}

void Users_create_async(struct io_uring *r, int client_fd, const char *username, const char *password_hash, cb callback, void *userdata) {
    const char *params[2] = { username, password_hash };
    db_query_prepared_params_async(r, client_fd, USERS_STMT_CREATE, 2, params, 0, callback, userdata);
}
