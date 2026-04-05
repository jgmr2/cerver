#pragma once
#include <postgresql/libpq-fe.h>
#include <liburing.h>
#include "../utils/events.h"
#define S 16
typedef int(*cb)(struct io_uring*,int,PGresult*);
typedef struct{event_type_t t;PGconn*c;struct io_uring*r;int f;cb s;char b;int p;}db_t;
extern __thread db_t pool[S];
void init_db(struct io_uring*r);
void handle_db_cqe(db_t*x,struct io_uring_cqe*e);
void db_query_async(struct io_uring*r,int f,const char*q,cb s);
void db_query_prepared_async(struct io_uring *r, int f, const char *stmt, cb s);
void db_query_prepared_fmt_async(struct io_uring *r, int f, const char *stmt, int result_format, cb s);