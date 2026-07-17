/*
 * models/sakila.c - SQL y registro de prepared statements de Sakila
 *
 * NOMBRE
 *     sakila.c - implementa sakila_register y las consultas asincronas
 *     de top peliculas / top actores
 *
 * DESCRIPCION
 *     Cada consulta esta envuelta en row_to_json/json_agg para que
 *     Postgres devuelva directamente el arreglo JSON final como una
 *     unica columna de texto; controllers/sakila.c no decodifica ni
 *     escapa nada, solo reenvia ese texto. COALESCE evita que una tabla
 *     vacia devuelva NULL en vez de un arreglo vacio ('[]').
 */
#include "sakila.h"

#define SAKILA_STMT_TOP_FILMS  "sakila_top_films"
#define SAKILA_STMT_TOP_ACTORS "sakila_top_actors"

/*
 * sakila_register - registra los dos prepared statements de este modelo
 * (ver sakila.h). Llamado una vez desde register_models() antes de crear
 * los hilos worker.
 */
void sakila_register(void) {
    db_register_prepared(SAKILA_STMT_TOP_FILMS,
        "SELECT COALESCE(json_agg(row_to_json(t)), '[]'::json) FROM ("
        "SELECT title, length, release_year, rating "
        "FROM film "
        "WHERE length IS NOT NULL "
        "ORDER BY length DESC, title ASC "
        "LIMIT 10"
        ") t;");

    db_register_prepared(SAKILA_STMT_TOP_ACTORS,
        "SELECT COALESCE(json_agg(row_to_json(t)), '[]'::json) FROM ("
        "SELECT a.first_name, a.last_name, COUNT(fa.film_id) AS films "
        "FROM actor a "
        "JOIN film_actor fa ON fa.actor_id = a.actor_id "
        "GROUP BY a.actor_id, a.first_name, a.last_name "
        "ORDER BY films DESC, a.last_name ASC, a.first_name ASC "
        "LIMIT 10"
        ") t;");
}

/*
 * Sakila_get_top_films_async - ver sakila.h
 *
 * Formato texto (no binario): el resultado ya es JSON armado por
 * Postgres, no hay nada que decodificar en C.
 */
void Sakila_get_top_films_async(struct io_uring *r, int client_fd, cb callback) {
    db_query_prepared_async(r, client_fd, SAKILA_STMT_TOP_FILMS, callback, NULL);
}

/*
 * Sakila_get_top_actors_async - ver sakila.h
 */
void Sakila_get_top_actors_async(struct io_uring *r, int client_fd, cb callback) {
    db_query_prepared_async(r, client_fd, SAKILA_STMT_TOP_ACTORS, callback, NULL);
}
