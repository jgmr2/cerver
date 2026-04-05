#ifndef MODELS_SAKILA_H
#define MODELS_SAKILA_H

#include <liburing.h>
#include "../config/db.h"

void Sakila_get_top_films_async(struct io_uring *r, int client_fd, cb callback);
void Sakila_get_top_actors_async(struct io_uring *r, int client_fd, cb callback);

#endif
