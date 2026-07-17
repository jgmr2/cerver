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

/*
 * sakila_refresh_actors_cache - dispara una consulta de top actores cuyo
 * resultado se guarda en el cache en memoria del hilo actual, en vez de
 * responderle a ningun cliente
 *
 * Pensada para llamarse periodicamente desde el timer de refresco de
 * cache del hilo (ver core/server.c) y una vez al arrancar el hilo, para
 * que get_sakila_top_actors pueda responder desde memoria sin tocar
 * Postgres en el camino caliente. Ver el comentario sobre el cache en
 * controllers/sakila.c para el porque (la consulta de top actores hace
 * un JOIN + GROUP BY sobre toda film_actor, medido en esta misma sesion
 * como el cuello de botella real del servidor bajo carga: ~1800 req/s
 * sin importar la concurrencia, con Postgres al 443% CPU mientras el
 * proceso C estaba practicamente ocioso).
 *
 * Parametros:
 *   r - anillo io_uring del hilo actual
 */
void sakila_refresh_actors_cache(struct io_uring *r);

#endif
