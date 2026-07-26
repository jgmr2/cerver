/* Generado por dbfiller -- ver tools/dbfiller. No editar a mano si vas a re-generar. */
/* dbfiller:sha256:4f5af0dd98256984a278d629ecf5543b3e2c3e3b130b08eab50906e4674a804d */

/*
 * utils/auth/auth_model.h - SQL y prepared statements de autenticacion
 *
 * Generado a partir de la tabla 'user' (columna de identidad
 * 'email', columna de password 'password_hash',
 * PK 'id') -- ver tools/dbfiller/README.md, seccion "Autenticacion".
 * Los nombres de funcion son fijos (no dependen del nombre de tabla):
 * utils/auth/auth.c los llama siempre igual, sin importar que esquema
 * los genero.
 */
#ifndef UTILS_AUTH_AUTH_MODEL_H
#define UTILS_AUTH_AUTH_MODEL_H

#include <liburing.h>
#include "../../config/db.h"

void auth_model_register(void);

/*
 * Auth_find_by_identity_async - busca por la columna de identidad.
 * Si PQntuples(res) == 1: columna 0 = id (texto), columna 1 =
 * password ya hasheado (texto). Cero filas = no existe.
 */
void Auth_find_by_identity_async(struct io_uring *r, int client_fd, const char *identity, cb callback, void *userdata);

/* Cantidad exacta de elementos que debe tener 'values' en Auth_create_async. */
#define AUTH_CREATE_PARAMS 7
/*
 * Auth_create_async - crea un registro nuevo. 'values' trae, en este
 * orden: [0] identidad, [1] password ya hasheado (ver
 * utils/auth/password.h), [2] first_name, [3] last_name, [4] auth_code, [5] fk_department, [6] fk_role.
 * Resultado (si la insercion tuvo exito): columna 0 = id.
 */
void Auth_create_async(struct io_uring *r, int client_fd, const char *const *values, cb callback, void *userdata);

#endif
