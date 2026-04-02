#pragma once
#include <stdio.h>
#include <liburing.h>
#include "../utils/http.h"     
#include "../config/db.h"
#include "../models/homeModel.h"

static inline void home(struct io_uring *ring, int client_fd) { 
    send_html(ring, client_fd, "<h1>Contenido de /home</h1>"); 
}

static inline void status(struct io_uring *ring, int client_fd) { 
    send_text(ring, client_fd, "Sistema OK"); 
}

static inline void error404(struct io_uring *ring, int client_fd) { 
    // Ahora enviamos un VERDADERO código HTTP 404 Not Found
    send_res(ring, client_fd, "404 Not Found", "text/plain", "404 - No encontrado\n"); 
}

// ... (Tu código de on_api_success y api se queda igual) ...

// Actualizamos el callback para recibir el ring y el socket
static inline void on_api_success(struct io_uring *ring, int client_fd, PGresult *r) {
    char j[256];
    if (r && PQresultStatus(r) == PGRES_TUPLES_OK) {
        unsigned char *val = (unsigned char *)PQgetvalue(r, 0, 0);
        int len = PQgetlength(r, 0, 0);
        
        snprintf(j, 256, "{\"data\":\"Binario desde Modelo, pequeño cambio para testear deploy automático, otro cambio\", \"bytes\": %d, \"hex\":\"%02x%02x%02x%02x\"}", 
                 len, val[0], val[1], val[2], val[3]);
        send_json(ring, client_fd, j);
    } else {
        send_json(ring, client_fd, "{\"error\":\"Error en BD\", \"status\": 503}");
    }
}

static inline void api(struct io_uring *ring, int client_fd) { 
    // Pasamos el ring y el file descriptor hacia la función de la base de datos
    db_query_async(ring, client_fd, QUERY_API_TIME, on_api_success); 
}