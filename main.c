#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <liburing.h>
#include <pthread.h> // Necesario para hilos

#include "utils/events.h"
#include "config/db.h"
#include "routes/index.h"

// Estructuras de contexto (se mantienen igual)
typedef struct {
    event_type_t type;
    int server_fd;
    struct sockaddr_in client_addr;
    socklen_t client_len;
} accept_ctx_t;


// --- FUNCIÓN DEL TRABAJADOR (Cada hilo corre esto) ---
void *worker_loop(void *arg) {
    int thread_id = *(int*)arg;
    free(arg);

    // 1. Cada hilo tiene su propio anillo (Ring) independiente
    struct io_uring ring;
    if (io_uring_queue_init(1024, &ring, 0) < 0) {
        perror("io_uring_init");
        return NULL;
    }

    // 2. IMPORTANTE: Cada hilo inicializa su propia copia del pool de DB
    // Asegúrate que en db.h la variable db_pool tenga el prefijo __thread
    init_db(&ring);

    // 3. Crear socket con SO_REUSEPORT
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Esta bandera permite que múltiples hilos escuchen en el mismo puerto 8080
    // El kernel de Linux balanceará las conexiones entre los hilos automáticamente.
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEPORT");
        return NULL;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return NULL;
    }
    listen(server_fd, 1024);

    printf("🧵 Hilo #%d iniciado y listo en el puerto 8080\n", thread_id);

    // 4. Preparar primer Accept para este hilo
    accept_ctx_t *accept_ctx = calloc(1, sizeof(*accept_ctx));
    accept_ctx->type = EVENT_ACCEPT;
    accept_ctx->server_fd = server_fd;
    accept_ctx->client_len = sizeof(struct sockaddr_in);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_accept(sqe, server_fd, (struct sockaddr*)&accept_ctx->client_addr, &accept_ctx->client_len, 0);
    io_uring_sqe_set_data(sqe, accept_ctx);

    // 5. El Event Loop (Se mantiene casi igual, pero es local al hilo)
    while (1) {
        io_uring_submit_and_wait(&ring, 1);
        struct io_uring_cqe *cqe;
        unsigned head;

        io_uring_for_each_cqe(&ring, head, cqe) {
            int *event_type = (int *)io_uring_cqe_get_data(cqe);
            if (!event_type) {
                io_uring_cqe_seen(&ring, cqe);
                continue;
            }

            if (*event_type == EVENT_ACCEPT) {
                accept_ctx_t *ctx = (accept_ctx_t *)event_type;
                int client_fd = cqe->res;

                struct io_uring_sqe *new_sqe = io_uring_get_sqe(&ring);
                io_uring_prep_accept(new_sqe, server_fd, (struct sockaddr*)&ctx->client_addr, &ctx->client_len, 0);
                io_uring_sqe_set_data(new_sqe, ctx);

                if (client_fd >= 0) {
                    initial_read_ctx_t *read_ctx = calloc(1, sizeof(*read_ctx));
                    read_ctx->type = EVENT_HTTP_READ_INITIAL;
                    read_ctx->client_fd = client_fd;
                    
                    new_sqe = io_uring_get_sqe(&ring);
                    io_uring_prep_read(new_sqe, client_fd, read_ctx->buf, sizeof(read_ctx->buf) - 1, 0);
                    io_uring_sqe_set_data(new_sqe, read_ctx);
                }
            } 
            else if (*event_type == EVENT_HTTP_READ_INITIAL) {
                initial_read_ctx_t *ctx = (initial_read_ctx_t *)event_type;
                if (cqe->res > 0) {
                    ctx->buf[cqe->res] = '\0';
                    char method[16], path[256];
                    if (sscanf(ctx->buf, "%15s %255s", method, path) == 2) {
                        router(&ring, ctx->client_fd, path);
                    } else {
                        close(ctx->client_fd);
                    }
                } else {
                    close(ctx->client_fd);
                }
                free(ctx);
            } 
            else if (*event_type == EVENT_HTTP_HELPER) {
                http_helper_handle_cqe(cqe);
            } 
            else if (*event_type == EVENT_DB_POLL) {
                handle_db_cqe((db_conn_t *)event_type, cqe);
            }

            io_uring_cqe_seen(&ring, cqe);
        }
    }
    return NULL;
}

int main() {
    // Detectar número de núcleos del sistema
    int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    printf("🔥 Detectados %d núcleos. Lanzando hilos de alto rendimiento...\n", num_cores);

    pthread_t threads[num_cores];

    for (int i = 0; i < num_cores; i++) {
        int *id = malloc(sizeof(int));
        *id = i;
        if (pthread_create(&threads[i], NULL, worker_loop, id) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    // Esperar a los hilos (nunca terminarán en un servidor)
    for (int i = 0; i < num_cores; i++) {
        pthread_join(threads[i], NULL);
    }

    return 0;
}