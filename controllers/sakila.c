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
 *
 *     get_sakila_top_actors es la excepcion: en vez de consultar
 *     Postgres en cada request, responde desde un cache en memoria por
 *     hilo (actors_cache_json) que se refresca periodicamente (ver
 *     sakila_refresh_actors_cache, llamada desde el timer de
 *     core/server.c). La consulta de top actores hace un
 *     JOIN + GROUP BY sobre toda film_actor; medido en esta misma
 *     sesion de trabajo, esa query es el cuello de botella real del
 *     servidor bajo carga (~1800 req/s sin importar la concurrencia,
 *     con Postgres al 443% CPU mientras el proceso C estaba
 *     practicamente ocioso) — y como el resultado (top 10 actores) es
 *     identico en cada llamada mientras no cambien los datos de base,
 *     recalcularlo en cada request es trabajo desperdiciado. top_films
 *     no tiene este problema (escala normalmente con la concurrencia,
 *     ver README) y sigue consultando Postgres en cada request.
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
static int on_top_films_fetched(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {
    (void)userdata;
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
static int on_top_actors_fetched(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {
    (void)userdata;
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
 * actors_cache_json / actors_cache_ready - cache en memoria del top de
 * actores, por hilo (__thread: cada uno de los 8 hilos worker tiene su
 * propia copia y su propio ciclo de refresco independiente, sin locks ni
 * coordinacion entre ellos — mismo principio de "nada compartido entre
 * hilos" que el resto del motor).
 *
 * actors_cache_ready arranca en 0: hasta que el primer refresco (ver
 * sakila_refresh_actors_cache, disparado una vez al arrancar el hilo en
 * core/server.c) complete, get_sakila_top_actors cae al camino en vivo
 * en vez de responder un cache vacio.
 */
static __thread char actors_cache_json[16384] = "{\"data\":[]}";
static __thread int actors_cache_ready = 0;

/*
 * on_actors_cache_refreshed - callback de la consulta periodica de
 * refresco del cache de top actores
 *
 * A diferencia de on_top_actors_fetched, no le responde a ningun
 * cliente: sakila_refresh_actors_cache dispara la consulta con
 * client_fd=-1 (sentinela, nunca se usa como fd real) precisamente para
 * que nada intente escribir una respuesta HTTP sobre el. Solo actualiza
 * actors_cache_json/actors_cache_ready.
 *
 * Si la consulta de refresco falla, el cache existente se deja tal cual
 * (servir un dato levemente desactualizado es preferible a pisarlo con
 * un error) y se loguea para que quede visible que un ciclo de refresco
 * fallo.
 *
 * Parametros:
 *   r         - anillo io_uring del hilo actual, sin uso aca
 *   client_fd - sentinela -1, sin uso
 *   res       - resultado de Postgres (una fila, una columna JSON), o
 *               NULL si la consulta fallo
 *   userdata  - sin uso
 *
 * Retorna:
 *   0 siempre (ver el contrato de cb en config/db.h).
 */
static int on_actors_cache_refreshed(struct io_uring *r, int client_fd, PGresult *res, void *userdata) {
    (void)r;
    (void)client_fd;
    (void)userdata;

    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1) {
        fprintf(stderr, "cache de actors/top: refresco fallido, se mantiene el valor anterior\n");
        return 0;
    }

    int n = snprintf(actors_cache_json, sizeof(actors_cache_json), "{\"data\":%s}", PQgetvalue(res, 0, 0));
    if (n < 0 || (size_t)n >= sizeof(actors_cache_json)) {
        fprintf(stderr, "cache de actors/top: resultado no entro en el buffer, se mantiene el valor anterior\n");
        return 0;
    }

    actors_cache_ready = 1;
    return 0;
}

/*
 * sakila_refresh_actors_cache - ver sakila.h
 */
void sakila_refresh_actors_cache(struct io_uring *r) {
    Sakila_get_top_actors_async(r, -1, on_actors_cache_refreshed);
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
 * A diferencia de get_sakila_top_films, no dispara una consulta por
 * request: responde directo desde actors_cache_json si ya se lleno al
 * menos una vez (ver el comentario de arriba del archivo). Mientras el
 * cache no este listo (ventana breve al arrancar el hilo, antes de que
 * el primer refresco periodico complete) cae al camino en vivo de
 * siempre, para no responder un cache vacio ni un error.
 */
void get_sakila_top_actors(struct io_uring *r, int fd, const char *req_body, const char *req_buf) {
    (void)req_body;
    (void)req_buf;
    if (actors_cache_ready) {
        res_json(r, fd, actors_cache_json);
    } else {
        Sakila_get_top_actors_async(r, fd, on_top_actors_fetched);
    }
}
