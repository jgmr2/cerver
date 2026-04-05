#include "sakila.h"

void Sakila_get_top_films_async(struct io_uring *r, int client_fd, cb callback) {
    db_query_prepared_fmt_async(r, client_fd, "sakila_top_films_bin", 1, callback);
}

void Sakila_get_top_actors_async(struct io_uring *r, int client_fd, cb callback) {
    db_query_prepared_fmt_async(r, client_fd, "sakila_top_actors_bin", 1, callback);
}
