/* Generado por dbfiller -- ver tools/dbfiller. No editar a mano si vas a re-generar. */
/* dbfiller:sha256:db9bf5d44cdce720f88462958ef6f33a28422a25a667eb99021df07aa6ea8af7 */
#include "auth_model.h"

#define AUTH_STMT_FIND_BY_IDENTITY "auth_find_by_identity"
#define AUTH_STMT_CREATE           "auth_create"

void auth_model_register(void) {
    db_register_prepared(AUTH_STMT_FIND_BY_IDENTITY,
        "SELECT \"id\", \"password_hash\" FROM \"user\" WHERE \"email\" = $1;");

    db_register_prepared(AUTH_STMT_CREATE,
        "INSERT INTO \"user\" (\"email\", \"password_hash\", \"first_name\", \"last_name\", \"auth_code\", \"fk_department\", \"fk_role\") VALUES ($1, $2, $3, $4, $5, $6, $7) RETURNING \"id\";");

}

void Auth_find_by_identity_async(struct io_uring *r, int client_fd, const char *identity, cb callback, void *userdata) {
    const char *params[1] = { identity };
    db_query_prepared_params_async(r, client_fd, AUTH_STMT_FIND_BY_IDENTITY, 1, params, 0, callback, userdata);
}

void Auth_create_async(struct io_uring *r, int client_fd, const char *const *values, cb callback, void *userdata) {
    db_query_prepared_params_async(r, client_fd, AUTH_STMT_CREATE, AUTH_CREATE_PARAMS, values, 0, callback, userdata);
}
