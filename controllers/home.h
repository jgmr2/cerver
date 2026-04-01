#pragma once
#include <stdio.h>
#include "../utils/http.h"
#include "../config/db.h"
#include "../models/homeModel.h" // Importamos el modelo

static inline void home(uv_stream_t *c) { send_html(c, "<h1>Contenido de /home</h1>"); }
static inline void status(uv_stream_t *c) { send_text(c, "Sistema OK"); }
static inline void error404(uv_stream_t *c) { send_text(c, "404 - No encontrado"); }

static inline void on_api_success(uv_stream_t *c, PGresult *r) {
    char j[256];
    if (r && PQresultStatus(r) == PGRES_TUPLES_OK) {
        unsigned char *val = (unsigned char *)PQgetvalue(r, 0, 0);
        int len = PQgetlength(r, 0, 0);
        
        snprintf(j, 256, "{\"data\":\"Binario desde Modelo, pequeño cambio para testear deploy automático\", \"bytes\": %d, \"hex\":\"%02x%02x%02x%02x\"}", 
                 len, val[0], val[1], val[2], val[3]);
        send_json(c, j);
    } else {
        send_json(c, "{\"error\":\"Error en BD\", \"status\": 503}");
    }
}

static inline void api(uv_stream_t *c) { 
    // Usamos las constantes definidas en el modelo
    db_query_async(c, QUERY_API_TIME, on_api_success); 
}