/*
 * core/server.c - hilo worker: socket, io_uring y despacho de eventos
 *
 * NOMBRE
 *     worker_loop - atiende conexiones HTTP de forma asincrona con io_uring
 *
 * DESCRIPCION
 *     Cada hilo creado desde main.c ejecuta este archivo de forma
 *     independiente: abre su propio socket de escucha con SO_REUSEPORT,
 *     inicializa su propio anillo io_uring y su propio pool de conexiones
 *     a la base de datos (init_db) y tabla de rutas (init_routes). No hay
 *     memoria compartida entre hilos ni locks.
 *
 *     El nucleo es un bucle "submit and wait" clasico de io_uring:
 *       1. io_uring_submit_and_wait bloquea hasta que haya al menos un
 *          Completion Queue Event (CQE) listo.
 *       2. Cada CQE trae de vuelta, via io_uring_cqe_get_data, el puntero
 *          de contexto que se asocio al Submission Queue Entry (SQE)
 *          original. Ese contexto siempre empieza con un event_type_t
 *          (ver utils/events.h), lo que permite reinterpretar el puntero
 *          segun el tipo de evento sin necesidad de una tabla aparte.
 *       3. Segun el tipo se despacha a: re-armar un accept, parsear un
 *          request HTTP recien leido, continuar una consulta a Postgres
 *          (handle_db_cqe) o continuar una operacion de archivo/escritura
 *          (http_helper_handle_cqe, en utils/http/http.h).
 *
 *     Ningun syscall bloqueante corre en este hilo: todo I/O (accept,
 *     read, write, openat2, statx, poll de libpq) se somete al anillo y
 *     se resuelve de forma asincrona.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <strings.h>
#include <arpa/inet.h>
#include <liburing.h>

#include "server.h"
#include "../utils/events.h"
#include "../utils/http/picohttpparser.h"
#include "../config/db.h"
#include "../routes/index.h"

/*
 * accept_ctx_t - contexto persistente del accept() re-armado
 *
 * A diferencia del resto de los contextos de evento (que se piden con
 * calloc por cada operacion y se liberan al terminar), este se reserva
 * una sola vez por hilo y se reutiliza en cada ciclo accept -> re-arm,
 * porque el accept siempre vuelve a apuntar al mismo socket de escucha.
 */
typedef struct {
    event_type_t type;
    int sfd;
    struct sockaddr_in client_addr;
    socklen_t client_len;
} accept_ctx_t;

/*
 * worker_loop - punto de entrada de cada hilo worker (ver core/server.h)
 *
 * Parametros:
 *   arg - puntero a un int con el id del hilo (0..N-1), reservado con
 *         malloc por main.c; se libera aqui mismo apenas se lee.
 *
 * Retorna:
 *   NULL. En la practica el bucle while(1) no termina salvo error fatal
 *   al crear el socket (perror + return).
 */
void *worker_loop(void *arg) {
    /* Extraemos el id y liberamos de inmediato: si algo falla mas abajo
     * y se retorna temprano, no queremos fugar esta reserva. */
    if (!arg) return NULL;
    int tid = *(int*)arg;
    free(arg);

    struct io_uring r;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(8080);
    a.sin_addr.s_addr = INADDR_ANY;

    int opt = 1;
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return NULL; }

    /* SO_REUSEPORT es lo que permite que cada hilo tenga su propio socket
     * escuchando en el mismo puerto: el kernel balancea las conexiones
     * entrantes entre todos ellos sin que el proceso tenga que repartirlas. */
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, 4);
    setsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, &opt, 4);

    if (bind(sfd, (struct sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); return NULL; }
    listen(sfd, 1024);

    /* 2048 entradas de cola: soporta varios miles de operaciones en vuelo
     * (conexiones, reads, writes, timeouts, consultas a Postgres) por hilo. */
    io_uring_queue_init(2048, &r, 0);
    init_db(&r);       /* pool de conexiones Postgres de este hilo (config/db.c) */
    init_routes();     /* tabla de rutas de este hilo (routes/index.h)          */

    printf("Hilo #%d iniciado en puerto 8080...\n", tid);

    /* Prepara el contexto de aceptacion, reutilizado en cada ciclo. */
    accept_ctx_t *actx = calloc(1, sizeof(accept_ctx_t));
    actx->type = EVENT_ACCEPT;
    actx->sfd = sfd;
    actx->client_len = sizeof(struct sockaddr_in);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&r);
    io_uring_prep_accept(sqe, sfd, (struct sockaddr*)&actx->client_addr, &actx->client_len, 0);
    io_uring_sqe_set_data(sqe, actx);

    while (1) {
        struct io_uring_cqe *cqe;
        /* Bloquea hasta que haya al menos un CQE listo; procesar el resto
         * del batch abajo evita una syscall extra por cada evento suelto. */
        io_uring_submit_and_wait(&r, 1);

        unsigned head;
        io_uring_for_each_cqe(&r, head, cqe) {
            void *data = io_uring_cqe_get_data(cqe);
            if (!data) goto next;

            /* Todo contexto de evento empieza con un event_type_t, asi
             * que este cast es seguro sin importar la struct real detras. */
            event_type_t type = *(event_type_t*)data;

            if (type == EVENT_ACCEPT) {
                int cfd = cqe->res;

                /* Re-armar el accept ANTES de procesar la conexion nueva:
                 * si no se hace de inmediato, el socket de escucha deja de
                 * aceptar mientras se atiende a este cliente. */
                struct io_uring_sqe *s = io_uring_get_sqe(&r);
                io_uring_prep_accept(s, sfd, (struct sockaddr*)&actx->client_addr, &actx->client_len, 0);
                io_uring_sqe_set_data(s, actx);

                if (cfd >= 0) {
                    /* Mismo patron que _rearm_read: el primer read tras el accept
                     * tambien necesita timeout. Sin esto, un cliente que abre la
                     * conexion y nunca manda nada (slowloris) deja el fd colgado
                     * para siempre — no hay otro punto en el codigo que lo cierre. */
                    _rearm_read(&r, cfd);
                }
            }
            else if (type == EVENT_HTTP_READ_INITIAL) {
                /* Primer (o siguiente, en keep-alive) read de una conexion:
                 * el buffer ya trae los bytes crudos del request. */
                initial_read_ctx_t *rd = (initial_read_ctx_t*)data;
                if (cqe->res > 0) {
                    const char *method_ptr, *path_ptr;
                    size_t method_len, path_len;
                    int minor_version;
                    struct phr_header hdrs[32];
                    size_t num_hdrs = 32;

                    /* picohttpparser: parser HTTP/1.x de terceros (ver
                     * utils/http/picohttpparser.c), no tocado por esta
                     * documentacion al ser codigo vendorizado con su
                     * propia licencia. */
                    int pret = phr_parse_request(rd->buf, (size_t)cqe->res,
                        &method_ptr, &method_len, &path_ptr, &path_len,
                        &minor_version, hdrs, &num_hdrs, 0);

                    if (pret > 0) {
                        /* Copiamos metodo y path a buffers de tamano fijo
                         * (truncando si hiciera falta) porque dispatch()
                         * y los handlers esperan cadenas terminadas en NUL,
                         * y rd->buf no lo garantiza mas alla del request. */
                        char m[16] = {0}, p[256] = {0};
                        size_t mlen = method_len < sizeof(m) - 1 ? method_len : sizeof(m) - 1;
                        size_t plen = path_len < sizeof(p) - 1 ? path_len : sizeof(p) - 1;
                        memcpy(m, method_ptr, mlen);
                        memcpy(p, path_ptr, plen);

                        /* HTTP/1.1 asume keep-alive salvo que diga lo contrario;
                         * HTTP/1.0 asume close salvo que lo pida explicitamente. */
                        int ka = (minor_version >= 1);
                        int has_cl = 0, has_te = 0;
                        for (size_t i = 0; i < num_hdrs; i++) {
                            if (hdrs[i].name_len == 10 && strncasecmp(hdrs[i].name, "Connection", 10) == 0) {
                                if (hdrs[i].value_len == 5 && strncasecmp(hdrs[i].value, "close", 5) == 0) ka = 0;
                                else if (hdrs[i].value_len == 10 && strncasecmp(hdrs[i].value, "keep-alive", 10) == 0) ka = 1;
                            }
                            if (hdrs[i].name_len == 14 && strncasecmp(hdrs[i].name, "Content-Length", 14) == 0) has_cl = 1;
                            if (hdrs[i].name_len == 17 && strncasecmp(hdrs[i].name, "Transfer-Encoding", 17) == 0) has_te = 1;
                        }

                        /* Content-Length y Transfer-Encoding a la vez es el vector
                         * clasico de HTTP request smuggling (desync entre nginx y
                         * este backend sobre donde termina un request y empieza el
                         * siguiente). No intentamos adivinar cual header "gana":
                         * se rechaza directo y se cierra la conexion. */
                        if (has_cl && has_te) {
                            set_keep_alive(rd->client_fd, 0);
                            send_res(&r, rd->client_fd, "400 Bad Request", "text/plain", "400");
                        } else {
                            set_keep_alive(rd->client_fd, ka);
                            dispatch(&r, rd->client_fd, m, p, rd->buf);
                        }
                    } else {
                        /* Parseo invalido o incompleto: no reintentamos, se
                         * cierra la conexion directamente. */
                        close(rd->client_fd);
                    }
                } else {
                    /* read <= 0: EOF o error de socket. */
                    close(rd->client_fd);
                }
                free(rd);
            }
            else if (type == EVENT_DB_POLL) {
                /* Continuacion de una consulta Postgres en curso (config/db.c). */
                handle_db_cqe((db_t*)data, cqe);
            }
            else if (type == EVENT_HTTP_HELPER) {
                /* Continuacion de una operacion de archivo o de escritura de
                 * respuesta (utils/http/http.h); esa funcion libera su propio
                 * contexto, aqui no hay nada mas que hacer. */
                http_helper_handle_cqe(cqe);
            }

        next:
            io_uring_cqe_seen(&r, cqe);
        }
    }
    return NULL;
}
