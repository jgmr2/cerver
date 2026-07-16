/*
 * models/sakila.h - interfaz publica del modelo Sakila
 *
 * NOMBRE
 *     sakila.h - declara el registro de statements y las consultas
 *     asincronas del dominio Sakila
 *
 * DESCRIPCION
 *     Este modelo es el unico dueno del SQL de Sakila: nombres de
 *     prepared statement, texto de las consultas y su forma de salida
 *     (json_agg/row_to_json, ver models/sakila.c). config/db.h/.c no
 *     conocen nada de esto; solo ofrecen el pool de conexiones y el
 *     registro generico de prepared statements.
 */
#ifndef MODELS_SAKILA_H
#define MODELS_SAKILA_H

#include <liburing.h>
#include "../config/db.h"

/*
 * sakila_register - registra los prepared statements de este modelo
 *
 * Debe llamarse una sola vez, antes de crear los hilos worker (ver
 * register_models() en models/registry.h, invocada desde main.c).
 */
void sakila_register(void);

/*
 * Sakila_get_top_films_async - dispara la consulta de top peliculas
 *
 * Parametros:
 *   r         - anillo io_uring del hilo actual
 *   client_fd - file descriptor del cliente que espera el resultado
 *   callback  - invocado cuando la consulta termine (o falle)
 */
void Sakila_get_top_films_async(struct io_uring *r, int client_fd, cb callback);

/*
 * Sakila_get_top_actors_async - dispara la consulta de top actores
 *
 * Parametros: iguales a Sakila_get_top_films_async.
 */
void Sakila_get_top_actors_async(struct io_uring *r, int client_fd, cb callback);

#endif
