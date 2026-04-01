#include "db.h"
#include <stdio.h>
#include <stdlib.h>

db_conn_t db_pool[POOL_SIZE];

static inline void on_db_readable(uv_poll_t* h, int s, int e) {
    db_conn_t *ctx = (db_conn_t *)h->data;
    if (!PQconsumeInput(ctx->conn)) return (void)fprintf(stderr, "Err BD: %s\n", PQerrorMessage(ctx->conn));
    if (PQisBusy(ctx->conn)) return;
    
    PGresult *res, *f_res = NULL;
    while ((res = PQgetResult(ctx->conn))) { f_res ? PQclear(f_res) : (void)0; f_res = res; }
    
    uv_poll_stop(h);
    f_res ? (ctx->on_success(ctx->client_stream, f_res), PQclear(f_res)) : (void)0;
    ctx->is_busy = 0;
}

void init_db(uv_loop_t *loop) {
    const char *env = getenv("DATABASE_URL");
    env ? printf("🔌 Iniciando pool ASYNC puro...\n") : (fprintf(stderr, "🚨 Sin URL\n"), exit(1), 0);
    
    for (int i = 0; i < POOL_SIZE; i++) {
        db_pool[i].conn = PQconnectdb(env);
        PQstatus(db_pool[i].conn) == CONNECTION_OK ? (
            PQsetnonblocking(db_pool[i].conn, 1),
            uv_poll_init(loop, &db_pool[i].poll_handle, PQsocket(db_pool[i].conn)),
            db_pool[i].poll_handle.data = &db_pool[i],
            db_pool[i].is_busy = 0,
            (void)printf("✅ pool[%d] OK\n", i)
        ) : (fprintf(stderr, "❌ Err: %s\n", PQerrorMessage(db_pool[i].conn)), exit(1), 0);
    }
}

void db_query_async(uv_stream_t *c, const char *q, db_callback cb) {
    db_conn_t *ctx = NULL;
    for (int i = 0; i < POOL_SIZE && !ctx; i++) !db_pool[i].is_busy ? (ctx = &db_pool[i]) : 0;
    
    !ctx ? cb(c, NULL) : (
        ctx->is_busy = 1, ctx->client_stream = c, ctx->on_success = cb,
        // Usamos PQsendQueryParams para pasar el formato 1 (binario) al final
        PQsendQueryParams(ctx->conn, q, 0, NULL, NULL, NULL, NULL, 1) ? 
            (void)uv_poll_start(&ctx->poll_handle, UV_READABLE, on_db_readable) 
            : (void)(fprintf(stderr, "Err: %s\n", PQerrorMessage(ctx->conn)), ctx->is_busy = 0)
    );
}