/*
 * controllers/sakila.c - handlers HTTP de /api/sakila/{films,actors}/top
 *
 * NOMBRE
 *     sakila.c - reenvia como respuesta el JSON que ya arma Postgres
 *
 * DESCRIPCION
 *     Las consultas de models/sakila.c devuelven una sola fila con una
 *     sola columna de texto: el arreglo JSON completo, armado en SQL con
 *     row_to_json/json_agg. Este archivo no decodifica columnas ni
 *     escapa texto; solo valida que la consulta haya tenido exito y
 *     envuelve ese texto en {"data": ...}.
 *
 *     El flujo es asincrono: get_sakila_top_* dispara la consulta
 *     (models/sakila.c -> config/db.c) y el callback
 *     on_top_*_fetched se invoca desde config/db.c cuando el resultado
 *     esta listo.
 */
#include "sakila.h"

#include <stdio.h>

#include "../config/db.h"
#include "../models/sakila.h"
#include "../utils/http/router.h"

/*
 * on_top_films_fetched - callback de la consulta de top peliculas
 *
 * Parametros:
 *   r         - anillo io_uring del hilo actual
 *   client_fd - file descriptor del cliente que espera la respuesta
 *   res       - resultado de Postgres (una fila, una columna JSON), o
 *               NULL si la consulta fallo
 *
 * Retorna:
 *   0 siempre: este callback no se queda con la propiedad de 'res' (ver
 *   el contrato de cb en config/db.h), config/db.c lo libera despues.
 */
static int on_top_films_fetched(struct io_uring *r, int client_fd, PGresult *res) {
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {
        res_json(r, client_fd, "{\"error\":\"Sakila query failed\"}");
        return 0;
    }

    char body[16384];
    snprintf(body, sizeof(body), "{\"data\":%s}", PQgetvalue(res, 0, 0));
    res_json(r, client_fd, body);
    return 0;
}

/*
 * on_top_actors_fetched - callback de la consulta de top actores
 *
 * Parametros y retorno: iguales a on_top_films_fetched.
 */
static int on_top_actors_fetched(struct io_uring *r, int client_fd, PGresult *res) {
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {
        res_json(r, client_fd, "{\"error\":\"Sakila query failed\"}");
        return 0;
    }

    char body[16384];
    snprintf(body, sizeof(body), "{\"data\":%s}", PQgetvalue(res, 0, 0));
    res_json(r, client_fd, body);
    return 0;
}

/*
 * get_sakila_top_films - handler de GET /api/sakila/films/top (ver sakila.h)
 *
 * No lee body ni request crudo (peticion GET sin parametros); solo
 * dispara la consulta asincrona y deja que on_top_films_fetched mande la
 * respuesta cuando llegue el resultado.
 */
void get_sakila_top_films(struct io_uring *r, int fd, const char *req_body, const char *req_buf) {
    (void)req_body;
    (void)req_buf;
    Sakila_get_top_films_async(r, fd, on_top_films_fetched);
}

/*
 * get_sakila_top_actors - handler de GET /api/sakila/actors/top (ver sakila.h)
 *
 * Analogo a get_sakila_top_films pero para el top de actores.
 */
void get_sakila_top_actors(struct io_uring *r, int fd, const char *req_body, const char *req_buf) {
    (void)req_body;
    (void)req_buf;
    Sakila_get_top_actors_async(r, fd, on_top_actors_fetched);
}
