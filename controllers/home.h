#pragma once
#include <stdio.h>
#include <liburing.h>
#include <time.h>
#include <string.h>
#include "../utils/http.h"     
#include "../config/db.h"
#include "../models/homeModel.h"

// --- ESTRUCTURA DE CACHÉ GLOBAL ---
typedef struct {
    char json[512];           // Buffer para el JSON
    long last_update_ms;      // Timestamp de la última actualización
} api_cache_t;

static api_cache_t global_api_cache = { .last_update_ms = 0 };

// Helper para obtener milisegundos actuales
static inline long get_now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000L) + (ts.tv_nsec / 1000000L);
}
// ----------------------------------

static inline void home(struct io_uring *ring, int client_fd) { 
    send_html(ring, client_fd, "<h1>Contenido de /home</h1>"); 
}

static inline void status(struct io_uring *ring, int client_fd) { 
    send_text(ring, client_fd, "Sistema OK"); 
}

static inline void error404(struct io_uring *ring, int client_fd) { 
    send_res(ring, client_fd, "404 Not Found", "text/plain", "404 - No encontrado\n"); 
}

// El callback ahora ACTUALIZA el caché antes de enviar
static inline void on_api_success(struct io_uring *ring, int client_fd, PGresult *r) {
    if (r && PQresultStatus(r) == PGRES_TUPLES_OK) {
        unsigned char *val = (unsigned char *)PQgetvalue(r, 0, 0);
        int len = PQgetlength(r, 0, 0);
        
        char j[512];
        snprintf(j, 512, "{\"data\":\"Binario desde Modelo, con CACHE 10ms\", \"bytes\": %d, \"hex\":\"%02x%02x%02x%02x\"}", 
                 len, val[0], val[1], val[2], val[3]);
        
        // Guardamos en el caché global ANTES de enviar
        strncpy(global_api_cache.json, j, 512);
        global_api_cache.last_update_ms = get_now_ms();

        send_json(ring, client_fd, j);
    } else {
        send_json(ring, client_fd, "{\"error\":\"Error en BD\", \"status\": 503}");
    }
}

static inline void api(struct io_uring *ring, int client_fd) { 
    long now = get_now_ms();

    // Si el caché tiene menos de 10ms, servimos directo de RAM
    if (now - global_api_cache.last_update_ms < 10 && global_api_cache.last_update_ms != 0) {
        send_json(ring, client_fd, global_api_cache.json);
        return; 
    }

    // Si expiró, vamos a la base de datos (una sola vez cada 10ms)
    db_query_async(ring, client_fd, QUERY_API_TIME, on_api_success); 
}