#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <poll.h> // Necesario para POLLIN

// Helper para obtener una entrada en la cola (SQE) de manera segura
static inline struct io_uring_sqe *_get_sqe(struct io_uring *ring) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
    }
    return sqe;
}


__thread db_conn_t db_pool[POOL_SIZE];

// Manejador de la máquina de estados de BD
void handle_db_cqe(db_conn_t *ctx, struct io_uring_cqe *cqe) {
    if (cqe->res < 0) {
        fprintf(stderr, "Err polling DB socket\n");
        ctx->is_busy = 0;
        return;
    }

    if (!PQconsumeInput(ctx->conn)) {
        fprintf(stderr, "Err BD: %s\n", PQerrorMessage(ctx->conn));
        ctx->is_busy = 0;
        return;
    }

    if (PQisBusy(ctx->conn)) {
        // Los datos aún no llegan completos. Re-armamos el poll.
        struct io_uring_sqe *sqe = _get_sqe(ctx->ring);
        io_uring_prep_poll_add(sqe, PQsocket(ctx->conn), POLLIN);
        io_uring_sqe_set_data(sqe, ctx);
        io_uring_submit(ctx->ring);
        return;
    }
    
    PGresult *res, *f_res = NULL;
    while ((res = PQgetResult(ctx->conn))) { 
        if (f_res) PQclear(f_res); 
        f_res = res; 
    }
    
    if (f_res) {
        ctx->on_success(ctx->ring, ctx->client_fd, f_res);
        PQclear(f_res);
    }
    ctx->is_busy = 0;
}

void init_db(struct io_uring *ring) {
    const char *env = getenv("DATABASE_URL");
    if (!env) { fprintf(stderr, "🚨 Sin URL\n"); exit(1); }
    
    printf("🔌 Iniciando pool io_uring puro...\n");
    
    for (int i = 0; i < POOL_SIZE; i++) {
        db_pool[i].conn = PQconnectdb(env);
        if (PQstatus(db_pool[i].conn) == CONNECTION_OK) {
            PQsetnonblocking(db_pool[i].conn, 1);
            
            // --- CORRECCIÓN 1: ALINEACIÓN DE ESTRUCTURA ---
            db_pool[i].core_type = EVENT_DB_POLL; 
            // ----------------------------------------------
            
            db_pool[i].ring = ring;
            db_pool[i].is_busy = 0;
            printf("✅ pool[%d] OK\n", i);
        } else {
            fprintf(stderr, "❌ Err: %s\n", PQerrorMessage(db_pool[i].conn));
            exit(1);
        }
    }
}

void db_query_async(struct io_uring *ring, int client_fd, const char *q, db_callback cb) {
    db_conn_t *ctx = NULL;
    for (int i = 0; i < POOL_SIZE && !ctx; i++) {
        if (!db_pool[i].is_busy) ctx = &db_pool[i];
    }
    
    if (!ctx) {
        // Pool agotado, devolvemos NULL
        cb(ring, client_fd, NULL);
        return;
    }
    
    ctx->is_busy = 1;
    ctx->client_fd = client_fd;
    ctx->on_success = cb;
    
    // Enviamos el query a libpq
    if (PQsendQueryParams(ctx->conn, q, 0, NULL, NULL, NULL, NULL, 1)) {
        
        // --- CORRECCIÓN 2: FORZAR EL ENVÍO AL SOCKET NAT ---
        PQflush(ctx->conn);
        // ---------------------------------------------------
        
        struct io_uring_sqe *sqe = _get_sqe(ring);
        // Le pedimos al kernel que nos avise cuando Postgres nos responda (POLLIN)
        io_uring_prep_poll_add(sqe, PQsocket(ctx->conn), POLLIN);
        io_uring_sqe_set_data(sqe, ctx);
        io_uring_submit(ring);
    } else {
        fprintf(stderr, "Err: %s\n", PQerrorMessage(ctx->conn));
        ctx->is_busy = 0;
    }
}