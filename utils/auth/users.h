/*
 * utils/auth/users.h - SQL y registro de prepared statements de usuarios
 *
 * NOMBRE
 *     users.h - consultas asincronas para registro/login (ver
 *     utils/auth/auth.c). Vive junto al resto de la infraestructura de
 *     auth (jwt.c, password.c) y no en models/, que queda reservado
 *     para modelos generados por tools/dbfiller.
 *
 * DESCRIPCION
 *     A diferencia de un modelo generado por tools/dbfiller (que
 *     devuelve JSON ya armado por Postgres via row_to_json), estas
 *     consultas devuelven columnas
 *     sueltas en formato texto: el caller necesita el valor de
 *     password_hash como string plano para verificarlo con
 *     utils/auth/password.h, no como parte de un JSON que habria que
 *     volver a parsear.
 *
 *     Ambas consultas van parametrizadas ($1, $2, ...); ver
 *     db_query_prepared_params_async en config/db.h para el porque
 *     (nunca concatenar un valor de un cliente HTTP al texto SQL).
 */
#ifndef UTILS_AUTH_USERS_H
#define UTILS_AUTH_USERS_H

#include <liburing.h>
#include "../../config/db.h"

/*
 * users_register - registra los prepared statements de este modelo
 * (ver models/registry.h). Llamado una vez desde register_models(),
 * antes de crear los hilos worker.
 */
void users_register(void);

/*
 * Users_find_by_username_async - busca un usuario por nombre de usuario
 *
 * El resultado (si PQntuples(res) == 1) trae dos columnas en formato
 * texto: columna 0 = id, columna 1 = password_hash. Cero filas
 * significa que el usuario no existe.
 *
 * Parametros:
 *   r        - anillo io_uring del hilo actual
 *   client_fd - file descriptor del cliente que espera el resultado
 *   username - nombre de usuario a buscar
 *   callback - invocado cuando el resultado esta listo (ver cb en config/db.h)
 *   userdata - puntero opaco transportado hasta 'callback' (ver cb en
 *              config/db.h); utils/auth/auth.c lo usa para llevar la
 *              contrasena en texto plano hasta el callback de login, que
 *              la necesita para verificarla contra password_hash
 */
void Users_find_by_username_async(struct io_uring *r, int client_fd, const char *username, cb callback, void *userdata);

/*
 * Users_create_async - crea un usuario nuevo
 *
 * El resultado (si la insercion tuvo exito) trae una columna: el id
 * generado para el usuario nuevo. Falla (PQresultStatus distinto de
 * PGRES_TUPLES_OK) si el username ya existe (UNIQUE constraint).
 *
 * Parametros:
 *   r             - anillo io_uring del hilo actual
 *   client_fd     - file descriptor del cliente que espera el resultado
 *   username      - nombre de usuario a crear
 *   password_hash - salida de password_hash() (utils/auth/password.h),
 *                   nunca la contrasena en texto plano
 *   callback      - invocado cuando el resultado esta listo
 *   userdata      - puntero opaco transportado hasta 'callback'
 */
void Users_create_async(struct io_uring *r, int client_fd, const char *username, const char *password_hash, cb callback, void *userdata);

#endif
