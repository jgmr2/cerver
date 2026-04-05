#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>

#define DB_FREE     0
#define DB_READING  1
#define DB_FLUSHING 2
#define DB_PENDING_Q 8192

typedef struct {
    int f;
    cb s;
    const char *sql_or_stmt;
    unsigned char prepared;
    unsigned char result_format;
} pending_req_t;

__thread db_t pool[S];
__thread db_t *free_pool[S]; 
__thread int free_count = 0; 
__thread pending_req_t pending_q[DB_PENDING_Q];
__thread int pending_head = 0;
__thread int pending_tail = 0;
__thread int pending_count = 0;

static int prepare_stmt(PGconn *c, const char *name, const char *sql) {
    PGresult *res = PQprepare(c, name, sql, 0, NULL);
    if (!res) return 0;

    int ok = PQresultStatus(res) == PGRES_COMMAND_OK;
    PQclear(res);
    return ok;
}

static inline struct io_uring_sqe *G(struct io_uring *r) {
    struct io_uring_sqe *s = io_uring_get_sqe(r);
    if (!s) { io_uring_submit(r); s = io_uring_get_sqe(r); }
    return s;
}

static inline void release_db(db_t *ctx) {
    ctx->b = DB_FREE;
    free_pool[free_count++] = ctx;
}

static inline int enqueue_pending(int f, const char *sql_or_stmt, cb s, int prepared, int result_format) {
    if (pending_count >= DB_PENDING_Q) return 0;

    pending_q[pending_tail].f = f;
    pending_q[pending_tail].s = s;
    pending_q[pending_tail].sql_or_stmt = sql_or_stmt;
    pending_q[pending_tail].prepared = (unsigned char)(prepared ? 1 : 0);
    pending_q[pending_tail].result_format = (unsigned char)((result_format != 0) ? 1 : 0);

    pending_tail = (pending_tail + 1) % DB_PENDING_Q;
    pending_count++;
    return 1;
}

static inline int dequeue_pending(pending_req_t *out) {
    if (pending_count <= 0) return 0;

    *out = pending_q[pending_head];
    pending_head = (pending_head + 1) % DB_PENDING_Q;
    pending_count--;
    return 1;
}

static inline void submit_db_wait(db_t *ctx, struct io_uring *r) {
    int flush_res = PQflush(ctx->c);
    struct io_uring_sqe *sq = G(r);

    // Totalmente asyncrono: el kernel nos avisa cuando el socket de libpq
    // esta listo para continuar sin bloquear workers.
    if (flush_res == 1) {
        ctx->b = DB_FLUSHING;
        io_uring_prep_poll_add(sq, PQsocket(ctx->c), POLLOUT);
    } else {
        ctx->b = DB_READING;
        io_uring_prep_poll_add(sq, PQsocket(ctx->c), POLLIN);
    }
    io_uring_sqe_set_data(sq, ctx);
}

static inline int start_query_with_ctx(db_t *ctx, struct io_uring *r, const char *sql_or_stmt, int prepared, int result_format) {
    int sent = prepared
        ? PQsendQueryPrepared(ctx->c, sql_or_stmt, 0, NULL, NULL, NULL, result_format)
        : PQsendQuery(ctx->c, sql_or_stmt);

    if (!sent) return 0;

    submit_db_wait(ctx, r);
    return 1;
}

static inline void recycle_or_release_db(db_t *ctx) {
    pending_req_t req;
    if (dequeue_pending(&req)) {
        ctx->f = req.f;
        ctx->s = req.s;
        if (start_query_with_ctx(ctx, ctx->r, req.sql_or_stmt, req.prepared, req.result_format)) {
            return;
        }

        if (ctx->s) (void)ctx->s(ctx->r, ctx->f, NULL);
    }

    release_db(ctx);
}

void handle_db_cqe(db_t *x, struct io_uring_cqe *e) {
    if (e->res < 0) goto error;

    if (x->b == DB_FLUSHING) {
        int flush_res = PQflush(x->c);
        if (flush_res < 0) goto error;
        
        struct io_uring_sqe *s = G(x->r);
        if (flush_res == 1) {
            io_uring_prep_poll_add(s, PQsocket(x->c), POLLOUT);
        } else {
            x->b = DB_READING;
            io_uring_prep_poll_add(s, PQsocket(x->c), POLLIN);
        }
        io_uring_sqe_set_data(s, x);
        return;
    }

    if (x->b == DB_READING) {
        if (!PQconsumeInput(x->c)) goto error;
        
        if (PQisBusy(x->c)) {
            struct io_uring_sqe *s = G(x->r);
            io_uring_prep_poll_add(s, PQsocket(x->c), POLLIN);
            io_uring_sqe_set_data(s, x);
            return;
        }

        PGresult *res, *last = NULL;
        while ((res = PQgetResult(x->c))) { 
            if (last) PQclear(last); 
            last = res; 
        }
        
        // El ownership de PGresult se mantiene en la capa DB.
        // El callback solo consume datos y no debe llamar PQclear.
        int keep_result = 0;
        if (x->s) keep_result = x->s(x->r, x->f, last);

        if (last && !keep_result) PQclear(last);
        recycle_or_release_db(x);
        return;
    }

error:
    if (x->b != DB_FREE && x->s) (void)x->s(x->r, x->f, NULL);
    recycle_or_release_db(x);
}

void init_db(struct io_uring *r) {
    char *u = getenv("DATABASE_URL");
    if (!u) {
        fprintf(stderr, "FATAL: DATABASE_URL no encontrada\n");
        exit(1);
    }

    // Reiniciamos el contador por seguridad del hilo
    free_count = 0;

    for (int i = 0; i < S; i++) {
        PGconn *conn = PQconnectdb(u);
        if (PQstatus(conn) != CONNECTION_OK) {
            fprintf(stderr, "FATAL: Error conectando a DB: %s\n", PQerrorMessage(conn));
            exit(1);
        }

        if (!prepare_stmt(
                conn,
                "sakila_top_films_bin",
                "SELECT title, length, release_year, rating "
                "FROM film "
                "WHERE length IS NOT NULL "
                "ORDER BY length DESC, title ASC "
                "LIMIT 10"
                ";") ||
            !prepare_stmt(
                conn,
                "sakila_top_actors_bin",
                "SELECT a.first_name, a.last_name, COUNT(fa.film_id) AS films "
                "FROM actor a "
                "JOIN film_actor fa ON fa.actor_id = a.actor_id "
                "GROUP BY a.actor_id, a.first_name, a.last_name "
                "ORDER BY films DESC, a.last_name ASC, a.first_name ASC "
                "LIMIT 10"
                ";")) {
            fprintf(stderr, "FATAL: Error preparando statements Sakila: %s\n", PQerrorMessage(conn));
            exit(1);
        }
        
        PQsetnonblocking(conn, 1);
        
        // CORRECCIÓN: Llenamos los campos uno a uno para no pisar el puntero 'c'
        pool[i].t = EVENT_DB_POLL;
        pool[i].c = conn;
        pool[i].r = r;
        pool[i].f = 0;
        pool[i].s = NULL;
        pool[i].b = DB_FREE;
        
        free_pool[free_count++] = &pool[i];
    }
}

void db_query_async(struct io_uring *r, int f, const char *q, cb s) {
    if (free_count <= 0) {
        if (!enqueue_pending(f, q, s, 0, 0) && s) (void)s(r, f, NULL);
        return; 
    }

    db_t *ctx = free_pool[--free_count];
    ctx->f = f; 
    ctx->s = s;

    if (!start_query_with_ctx(ctx, r, q, 0, 0)) {
        release_db(ctx);
        if (s) (void)s(r, f, NULL);
    }
}

void db_query_prepared_async(struct io_uring *r, int f, const char *stmt, cb s) {
    db_query_prepared_fmt_async(r, f, stmt, 0, s);
}

void db_query_prepared_fmt_async(struct io_uring *r, int f, const char *stmt, int result_format, cb s) {
    if (free_count <= 0) {
        if (!enqueue_pending(f, stmt, s, 1, result_format) && s) (void)s(r, f, NULL);
        return;
    }

    db_t *ctx = free_pool[--free_count];
    ctx->f = f;
    ctx->s = s;

    if (!start_query_with_ctx(ctx, r, stmt, 1, result_format)) {
        release_db(ctx);
        if (s) (void)s(r, f, NULL);
    }
}