#pragma once
#include <postgresql/libpq-fe.h>
#include <uv.h>

#define POOL_SIZE 64

typedef void (*db_callback)(uv_stream_t*, PGresult*);

typedef struct {
    PGconn *conn;
    uv_poll_t poll_handle;
    uv_stream_t *client_stream;
    db_callback on_success;
    char is_busy;
} db_conn_t;

extern db_conn_t db_pool[POOL_SIZE];
void init_db(uv_loop_t *loop);
void db_query_async(uv_stream_t *c, const char *query, db_callback cb);