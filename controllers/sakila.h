#ifndef CONTROLLERS_SAKILA_H
#define CONTROLLERS_SAKILA_H

#include <liburing.h>

void get_sakila_top_films(struct io_uring *r, int fd, const char *req_body, const char *req_buf);
void get_sakila_top_actors(struct io_uring *r, int fd, const char *req_body, const char *req_buf);

#endif
