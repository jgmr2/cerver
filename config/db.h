#ifndef DB_CONFIG_H
#define DB_CONFIG_H

#include <postgresql/libpq-fe.h>
#include <uv.h> 

#define POOL_SIZE 100

extern PGconn *db_pool[POOL_SIZE];
extern unsigned int pool_index;

void init_db();

typedef void (*db_callback)(uv_stream_t *c, PGresult *r);

struct db_ctx {
    uv_work_t req;
    uv_stream_t *c;
    const char *query;
    PGresult *r;
    db_callback on_success;
};

void db_query_async(uv_stream_t *c, const char *query, db_callback cb);

#endif
