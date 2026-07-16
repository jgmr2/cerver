/*
 * controllers/sakila.h - interfaz publica de los endpoints de Sakila
 *
 * NOMBRE
 *     sakila.h - declara los handlers HTTP de /api/sakila/{films,actors}/top
 *
 * DESCRIPCION
 *     Estos handlers siguen la firma handler_t de utils/http/router.h y
 *     se registran en routes/index.h. La implementacion vive en
 *     controllers/sakila.c.
 */
#ifndef CONTROLLERS_SAKILA_H
#define CONTROLLERS_SAKILA_H

#include <liburing.h>

/*
 * get_sakila_top_films - handler de GET /api/sakila/films/top
 *
 * Responde con las 10 peliculas de mayor duracion (ver el prepared
 * statement sakila_top_films_bin en config/db.c).
 *
 * Parametros:
 *   r        - anillo io_uring del hilo actual
 *   fd       - file descriptor del cliente
 *   req_body - sin uso en este handler (no lee body)
 *   req_buf  - sin uso en este handler (no lee headers crudos)
 */
void get_sakila_top_films(struct io_uring *r, int fd, const char *req_body, const char *req_buf);

/*
 * get_sakila_top_actors - handler de GET /api/sakila/actors/top
 *
 * Responde con los 10 actores con mas peliculas (ver el prepared
 * statement sakila_top_actors_bin en config/db.c).
 *
 * Parametros:
 *   r        - anillo io_uring del hilo actual
 *   fd       - file descriptor del cliente
 *   req_body - sin uso en este handler (no lee body)
 *   req_buf  - sin uso en este handler (no lee headers crudos)
 */
void get_sakila_top_actors(struct io_uring *r, int fd, const char *req_body, const char *req_buf);

#endif
