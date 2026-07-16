/*
 * config/db.c - pool de conexiones Postgres asincrono sobre io_uring
 *
 * NOMBRE
 *     db.c - implementa init_db, handle_db_cqe y las funciones
 *     db_query*_async declaradas en db.h
 *
 * DESCRIPCION
 *     Cada hilo worker mantiene S conexiones libpq en modo no bloqueante
 *     (pool[]) y una cola circular de peticiones pendientes
 *     (pending_q[]) para cuando las S conexiones estan ocupadas. El ciclo
 *     de vida de una conexion es:
 *
 *       libre -> se le asigna una consulta (start_query_with_ctx)
 *             -> DB_FLUSHING si el envio no cupo de una vez (PQflush)
 *             -> DB_READING mientras espera que Postgres responda
 *             -> se invoca el callback del caller con el resultado
 *             -> si hay algo en pending_q, se reutiliza para esa
 *                peticion (recycle_or_release_db); si no, vuelve a
 *                free_pool
 *
 *     Todo el avance de estado ocurre en handle_db_cqe, llamado desde
 *     core/server.c cada vez que el kernel avisa (via poll de io_uring)
 *     que el socket de una conexion esta listo para leer o escribir.
 */
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>

/* Estados posibles del campo db_t.b. */
#define DB_FREE     0
#define DB_READING  1
#define DB_FLUSHING 2
/* Capacidad de la cola de peticiones en espera de una conexion libre. */
#define DB_PENDING_Q 8192

/*
 * pending_req_t - una peticion en espera de conexion libre
 *
 * Campos:
 *   f             - file descriptor del cliente que espera el resultado
 *   s             - callback a invocar cuando la consulta termine
 *   sql_or_stmt   - texto SQL o nombre de prepared statement, segun 'prepared'
 *   prepared      - 1 si sql_or_stmt es un nombre de prepared statement
 *   result_format - 0 = texto, 1 = binario
 */
typedef struct {
    int f;
    cb s;
    const char *sql_or_stmt;
    unsigned char prepared;
    unsigned char result_format;
} pending_req_t;

/* Pool de conexiones del hilo (declarado extern en db.h). */
__thread db_t pool[S];
/* Pila de punteros a conexiones libres dentro de pool[]. */
__thread db_t *free_pool[S];
__thread int free_count = 0;
/* Cola circular FIFO de peticiones que esperan una conexion libre. */
__thread pending_req_t pending_q[DB_PENDING_Q];
__thread int pending_head = 0;
__thread int pending_tail = 0;
__thread int pending_count = 0;

/*
 * prepared_decl_t - una entrada del registro generico de prepared statements
 *
 * Poblado por db_register_prepared() (llamada desde cada modelo, vía
 * register_models() en models/registry.h) antes de que arranque ningun
 * hilo worker. init_db() recorre este arreglo para preparar cada
 * conexion nueva, sin que db.c conozca de que tabla o dominio viene cada
 * statement.
 */
typedef struct {
    const char *name;
    const char *sql;
} prepared_decl_t;

/* Registro global (no __thread): es solo metadata constante compartida
 * por todos los hilos, poblada una sola vez antes de crearlos. */
static prepared_decl_t prepared_registry[DB_MAX_PREPARED];
static int prepared_registry_count = 0;

/*
 * db_register_prepared - agrega un statement al registro generico (ver db.h)
 *
 * No conecta a Postgres ni prepara nada todavia: solo guarda el nombre y
 * el SQL para que init_db() los prepare en cada conexion que abra.
 *
 * Parametros: ver db.h
 */
void db_register_prepared(const char *name, const char *sql) {
    if (prepared_registry_count >= DB_MAX_PREPARED) {
        fprintf(stderr, "FATAL: DB_MAX_PREPARED (%d) alcanzado registrando '%s'\n", DB_MAX_PREPARED, name);
        exit(1);
    }
    prepared_registry[prepared_registry_count].name = name;
    prepared_registry[prepared_registry_count].sql = sql;
    prepared_registry_count++;
}

/*
 * prepare_stmt - prepara un statement con nombre en una conexion
 *
 * Parametros:
 *   c    - conexion Postgres ya abierta
 *   name - nombre con el que luego se invoca via PQsendQueryPrepared
 *   sql  - texto SQL del statement
 *
 * Retorna:
 *   1 si Postgres confirmo el prepared (PGRES_COMMAND_OK), 0 en cualquier
 *   otro caso (incluyendo que PQprepare no haya devuelto resultado).
 */
static int prepare_stmt(PGconn *c, const char *name, const char *sql) {
    PGresult *res = PQprepare(c, name, sql, 0, NULL);
    if (!res) return 0;

    int ok = PQresultStatus(res) == PGRES_COMMAND_OK;
    PQclear(res);
    return ok;
}

/*
 * G - obtiene un SQE (Submission Queue Entry) libre, sometiendo si hace falta
 *
 * io_uring_get_sqe devuelve NULL si la cola de envio esta llena; en ese
 * caso se somete lo ya encolado (io_uring_submit) para liberar espacio y
 * se reintenta una vez.
 *
 * Parametros:
 *   r - anillo io_uring del hilo actual
 *
 * Retorna:
 *   un SQE listo para usar (no se contempla el caso de fallo tras el
 *   submit, ya que en ese punto siempre deberia haber espacio).
 */
static inline struct io_uring_sqe *G(struct io_uring *r) {
    struct io_uring_sqe *s = io_uring_get_sqe(r);
    if (!s) { io_uring_submit(r); s = io_uring_get_sqe(r); }
    return s;
}

/*
 * release_db - devuelve una conexion al pool de libres
 *
 * Parametros:
 *   ctx - conexion a liberar; se marca DB_FREE y se apila en free_pool
 */
static inline void release_db(db_t *ctx) {
    ctx->b = DB_FREE;
    free_pool[free_count++] = ctx;
}

/*
 * enqueue_pending - encola una peticion cuando no hay conexiones libres
 *
 * Parametros:
 *   f             - file descriptor del cliente que espera el resultado
 *   sql_or_stmt   - texto SQL o nombre de prepared statement
 *   s             - callback a invocar cuando la consulta termine
 *   prepared      - distinto de 0 si sql_or_stmt es un prepared statement
 *   result_format - distinto de 0 para pedir resultados en binario
 *
 * Retorna:
 *   1 si se encolo, 0 si la cola esta llena (DB_PENDING_Q alcanzado)
 */
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

/*
 * dequeue_pending - saca la siguiente peticion en espera (FIFO)
 *
 * Parametros:
 *   out - donde copiar la peticion desencolada
 *
 * Retorna:
 *   1 si habia una peticion y se copio en *out, 0 si la cola esta vacia
 */
static inline int dequeue_pending(pending_req_t *out) {
    if (pending_count <= 0) return 0;

    *out = pending_q[pending_head];
    pending_head = (pending_head + 1) % DB_PENDING_Q;
    pending_count--;
    return 1;
}

/*
 * submit_db_wait - arma el poll de io_uring que hace avanzar la conexion
 *
 * Llama a PQflush para saber si aun queda algo por escribir en el socket
 * de libpq y arma un io_uring_prep_poll_add sobre POLLOUT o POLLIN segun
 * corresponda. Totalmente asincrono: el kernel avisa cuando el socket de
 * libpq esta listo para continuar, sin bloquear el hilo worker.
 *
 * Parametros:
 *   ctx - conexion cuyo estado (ctx->b) se actualiza segun el resultado
 *   r   - anillo io_uring donde se arma el poll
 */
static inline void submit_db_wait(db_t *ctx, struct io_uring *r) {
    int flush_res = PQflush(ctx->c);
    struct io_uring_sqe *sq = G(r);

    if (flush_res == 1) {
        ctx->b = DB_FLUSHING;
        io_uring_prep_poll_add(sq, PQsocket(ctx->c), POLLOUT);
    } else {
        ctx->b = DB_READING;
        io_uring_prep_poll_add(sq, PQsocket(ctx->c), POLLIN);
    }
    io_uring_sqe_set_data(sq, ctx);
}

/*
 * start_query_with_ctx - envia una consulta sobre una conexion ya asignada
 *
 * Parametros:
 *   ctx           - conexion del pool a usar
 *   r             - anillo io_uring del hilo actual
 *   sql_or_stmt   - texto SQL o nombre de prepared statement
 *   prepared      - distinto de 0 para usar PQsendQueryPrepared
 *   result_format - 0 = texto, 1 = binario (solo aplica a prepared)
 *
 * Retorna:
 *   1 si libpq acepto encolar el envio (y ya se armo el poll con
 *   submit_db_wait), 0 si PQsendQuery/PQsendQueryPrepared fallo
 */
static inline int start_query_with_ctx(db_t *ctx, struct io_uring *r, const char *sql_or_stmt, int prepared, int result_format) {
    int sent = prepared
        ? PQsendQueryPrepared(ctx->c, sql_or_stmt, 0, NULL, NULL, NULL, result_format)
        : PQsendQuery(ctx->c, sql_or_stmt);

    if (!sent) return 0;

    submit_db_wait(ctx, r);
    return 1;
}

/*
 * recycle_or_release_db - reasigna una conexion libre o la devuelve al pool
 *
 * Si hay una peticion esperando en pending_q, la conexion se reutiliza
 * directamente para ella (evita el costo de liberar y volver a tomar del
 * pool); si el envio de esa nueva consulta falla, se avisa al callback
 * con res=NULL antes de liberar. Si no hay nada pendiente, la conexion
 * vuelve a free_pool.
 *
 * Parametros:
 *   ctx - conexion que acaba de quedar libre
 */
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

/*
 * handle_db_cqe - avanza el estado de una conexion tras un evento de poll
 *
 * Se invoca desde core/server.c para todo CQE con tipo EVENT_DB_POLL.
 * Segun el estado de la conexion:
 *   DB_FLUSHING - sigue vaciando el buffer de envio (PQflush); si ya
 *                 termino, pasa a DB_READING y arma poll de lectura
 *   DB_READING  - consume la entrada disponible (PQconsumeInput); si
 *                 Postgres todavia esta ocupado (PQisBusy) vuelve a armar
 *                 poll de lectura; si ya hay resultado, drena todos los
 *                 PGresult hasta el ultimo (el ultimo es el que importa
 *                 para un solo statement), invoca el callback y recicla
 *                 o libera la conexion
 *
 * El ownership del PGresult final se mantiene en esta capa: el callback
 * solo lo consume y, salvo que devuelva 1 (se queda con el resultado),
 * no debe llamar PQclear sobre el.
 *
 * Parametros:
 *   x - conexion asociada a este evento
 *   e - CQE recibido; e->res < 0 fuerza la rama de error
 */
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

        /* PQgetResult puede devolver varios resultados para un mismo
         * envio; para un solo statement el ultimo es el que vale, asi
         * que se van liberando los anteriores a medida que se avanza. */
        PGresult *res, *last = NULL;
        while ((res = PQgetResult(x->c))) {
            if (last) PQclear(last);
            last = res;
        }

        /* El ownership de PGresult se mantiene en la capa DB.
         * El callback solo consume datos y no debe llamar PQclear,
         * salvo que devuelva 1 para quedarse con el resultado. */
        int keep_result = 0;
        if (x->s) keep_result = x->s(x->r, x->f, last);

        if (last && !keep_result) PQclear(last);
        recycle_or_release_db(x);
        return;
    }

error:
    /* Conexion en error: si tenia un callback pendiente, se le avisa con
     * res=NULL antes de reciclar o liberar la conexion. */
    if (x->b != DB_FREE && x->s) (void)x->s(x->r, x->f, NULL);
    recycle_or_release_db(x);
}

/*
 * init_db - conecta y prepara el pool de conexiones del hilo actual
 *
 * Lee DATABASE_URL del entorno y abre S conexiones Postgres, cada una en
 * modo no bloqueante. En cada conexion prepara todos los statements
 * presentes en el registro generico (ver db_register_prepared), sin
 * conocer a que modelo o tabla pertenece cada uno.
 *
 * Termina el proceso (exit(1)) si falta DATABASE_URL, si alguna conexion
 * falla o si algun prepare falla: sin base de datos operativa no hay
 * forma razonable de seguir arrancando el hilo.
 *
 * Parametros:
 *   r - anillo io_uring del hilo actual, guardado en cada db_t del pool
 */
void init_db(struct io_uring *r) {
    char *u = getenv("DATABASE_URL");
    if (!u) {
        fprintf(stderr, "FATAL: DATABASE_URL no encontrada\n");
        exit(1);
    }

    /* Reinicia el contador de libres: init_db solo se llama una vez por
     * hilo, pero se deja explicito por seguridad ante cambios futuros. */
    free_count = 0;

    for (int i = 0; i < S; i++) {
        PGconn *conn = PQconnectdb(u);
        if (PQstatus(conn) != CONNECTION_OK) {
            fprintf(stderr, "FATAL: Error conectando a DB: %s\n", PQerrorMessage(conn));
            exit(1);
        }

        for (int j = 0; j < prepared_registry_count; j++) {
            if (!prepare_stmt(conn, prepared_registry[j].name, prepared_registry[j].sql)) {
                fprintf(stderr, "FATAL: Error preparando statement '%s': %s\n",
                        prepared_registry[j].name, PQerrorMessage(conn));
                exit(1);
            }
        }

        PQsetnonblocking(conn, 1);

        /* Se llenan los campos uno a uno (en vez de un literal compuesto)
         * para no pisar por accidente el puntero 'c' ya asignado. */
        pool[i].t = EVENT_DB_POLL;
        pool[i].c = conn;
        pool[i].r = r;
        pool[i].f = 0;
        pool[i].s = NULL;
        pool[i].b = DB_FREE;

        free_pool[free_count++] = &pool[i];
    }
}

/*
 * db_query_async - lanza una consulta de texto plano (ver db.h)
 *
 * Si no hay conexiones libres, intenta encolar la peticion; si tampoco
 * hay espacio en la cola, avisa al callback con res=NULL de inmediato.
 * Si el envio a una conexion libre falla, la libera y avisa al callback.
 *
 * Parametros: ver db.h
 */
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

/*
 * db_query_prepared_async - lanza un prepared statement en formato texto
 * (ver db.h). Delega en db_query_prepared_fmt_async con result_format=0.
 */
void db_query_prepared_async(struct io_uring *r, int f, const char *stmt, cb s) {
    db_query_prepared_fmt_async(r, f, stmt, 0, s);
}

/*
 * db_query_prepared_fmt_async - lanza un prepared statement con formato
 * elegido (ver db.h). Misma logica de pool/cola que db_query_async.
 *
 * Parametros: ver db.h
 */
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
