#include "db.h"
#include <stdio.h>
#include <stdlib.h> // Para malloc y free

PGconn *db_pool[POOL_SIZE];
unsigned int pool_index = 0;

void init_db() {
    const char *conn_info = "host=127.0.0.1 dbname=db_pruebas user=ninja password=12345";

    for (int i = 0; i < POOL_SIZE; i++) {
        db_pool[i] = PQconnectdb(conn_info);

        if (PQstatus(db_pool[i]) != CONNECTION_OK) {
            fprintf(stderr, "❌ Error en pool[%d]: %s", i, PQerrorMessage(db_pool[i]));
        } else {
            printf("✅ Conexión pool[%d] establecida con éxito.\n", i);
        }
    }
}

// --- Implementación de Utilidades Asíncronas ---

// Función estática: solo visible dentro de db.c
static void generic_db_worker(uv_work_t *req) {
    struct db_ctx *ctx = (struct db_ctx *)req;
    ctx->r = PQexec(db_pool[(pool_index++) % POOL_SIZE], ctx->query);
}

// Función estática: solo visible dentro de db.c
static void generic_db_done(uv_work_t *req, int status) {
    struct db_ctx *ctx = (struct db_ctx *)req;
    
    // Llamar al controlador
    ctx->on_success(ctx->c, ctx->r);
    
    // Limpieza
    PQclear(ctx->r);
    free(ctx);
}

// La función pública que usarán tus controladores
void db_query_async(uv_stream_t *c, const char *query, db_callback cb) {
    struct db_ctx *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        fprintf(stderr, "Error: No se pudo asignar memoria para db_ctx\n");
        return; // Protección básica contra fallos de memoria
    }
    
    ctx->c = c;
    ctx->query = query;
    ctx->on_success = cb;
    
    uv_queue_work(c->loop, (uv_work_t *)ctx, generic_db_worker, generic_db_done);
}
