#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <liburing.h>

#include "server.h"
#include "../utils/events.h"
#include "../config/db.h"
#include "../routes/index.h"

typedef struct {
    event_type_t type;
    int sfd;
    struct sockaddr_in client_addr;
    socklen_t client_len;
} accept_ctx_t;

void *worker_loop(void *arg) {
    // 1. Extraer ID y liberar inmediatamente para evitar fugas y crashes
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

    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, 4);
    setsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, &opt, 4);
    
    if (bind(sfd, (struct sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); return NULL; }
    listen(sfd, 1024);

    io_uring_queue_init(2048, &r, 0); // Subimos a 2048 para mayor carga
    init_db(&r);
    init_routes();
    
    printf("🧵 Hilo #%d iniciado en puerto 8080...\n", tid);

    // Preparar el contexto de aceptación
    accept_ctx_t *actx = calloc(1, sizeof(accept_ctx_t));
    actx->type = EVENT_ACCEPT;
    actx->sfd = sfd;
    actx->client_len = sizeof(struct sockaddr_in);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&r);
    io_uring_prep_accept(sqe, sfd, (struct sockaddr*)&actx->client_addr, &actx->client_len, 0);
    io_uring_sqe_set_data(sqe, actx);

    while (1) {
        struct io_uring_cqe *cqe;
        io_uring_submit_and_wait(&r, 1);
        
        unsigned head;
        io_uring_for_each_cqe(&r, head, cqe) {
            void *data = io_uring_cqe_get_data(cqe);
            if (!data) goto next;

            // Usamos un puntero seguro para el tipo
            event_type_t type = *(event_type_t*)data;

            if (type == EVENT_ACCEPT) {
                int cfd = cqe->res;
                
                // Re-armar el accept INMEDIATAMENTE
                struct io_uring_sqe *s = io_uring_get_sqe(&r);
                io_uring_prep_accept(s, sfd, (struct sockaddr*)&actx->client_addr, &actx->client_len, 0);
                io_uring_sqe_set_data(s, actx);

                if (cfd >= 0) {
                    initial_read_ctx_t *rd = calloc(1, sizeof(initial_read_ctx_t));
                    rd->type = EVENT_HTTP_READ_INITIAL; 
                    rd->client_fd = cfd;
                    
                    s = io_uring_get_sqe(&r);
                    io_uring_prep_read(s, cfd, rd->buf, sizeof(rd->buf) - 1, 0);
                    io_uring_sqe_set_data(s, rd);
                }
            } 
            else if (type == EVENT_HTTP_READ_INITIAL) {
                initial_read_ctx_t *rd = (initial_read_ctx_t*)data;
                if (cqe->res > 0) {
                    char m[16] = {0}, p[256] = {0};
                    if (sscanf(rd->buf, "%15s %255s", m, p) == 2) {
                        dispatch(&r, rd->client_fd, m, p, rd->buf);
                    } else {
                        close(rd->client_fd);
                    }
                } else {
                    close(rd->client_fd);
                }
                free(rd);
            } 
            else if (type == EVENT_DB_POLL) {
                handle_db_cqe((db_t*)data, cqe);
            }
            else if (type == EVENT_HTTP_HELPER) {
                // Asegúrate de que http_helper_handle_cqe libere su propia data
                http_helper_handle_cqe(cqe);
            }

        next:
            io_uring_cqe_seen(&r, cqe);
        }
    }
    return NULL;
}