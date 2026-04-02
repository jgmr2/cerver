#pragma once
#include <postgresql/libpq-fe.h>
#include <liburing.h> // Adiós <uv.h>

// Importamos el enumerador global que creaste
// (Ajusta la ruta si guardaste events.h en otra carpeta)
#include "../utils/events.h" 

#define POOL_SIZE 128

// 1. Actualizamos la firma del callback para usar ring y client_fd
typedef void (*db_callback)(struct io_uring*, int, PGresult*);

typedef struct {
    // ¡CRÍTICO! Este DEBE ser el primer campo de la estructura
    // para que main.c sepa qué tipo de evento es al castear (int*)
    event_type_t core_type; 
    
    PGconn *conn;
    
    // Guardamos el ring para encolar los polls de lectura/escritura
    struct io_uring *ring; 
    
    // Reemplazamos uv_stream_t con el file descriptor nativo
    int client_fd; 
    
    db_callback on_success;
    char is_busy;
    
    // Opcional pero recomendado: un tipo de estado para saber 
    // en tu handle_cqe qué estás haciendo con esta conexión
    int poll_state; 
} db_conn_t;

extern __thread db_conn_t db_pool[POOL_SIZE];
// 2. Reemplazamos uv_loop_t con la estructura principal del ring
void init_db(struct io_uring *ring);

// 3. El manejador de la máquina de estados para la base de datos
void handle_db_cqe(db_conn_t *ctx, struct io_uring_cqe *cqe);

// 4. Reemplazamos uv_stream_t en los parámetros por el nativo
void db_query_async(struct io_uring *ring, int client_fd, const char *query, db_callback cb);