#ifndef HOME_CTRL_H
#define HOME_CTRL_H

#include "../utils/http.h"
#include "../config/db.h"

static inline void home(uv_stream_t *c)     { send_html(c, "<h1>Contenido de /home</h1>"); }
static inline void status(uv_stream_t *c)   { send_text(c, "Sistema OK"); }

static inline void error404(uv_stream_t *c) { send_text(c, "404 - No encontrado"); }

static inline void api(uv_stream_t *c) {
    char j[256];
    PGresult *r = PQexec(db_pool[(pool_index++) % POOL_SIZE], "SELECT current_timestamp;");
    
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        snprintf(j, 256, "{\"data\":\"Desde el Pool\", \"status\": 200, \"db_time\": \"%s\"}", PQgetvalue(r, 0, 0));
        send_json(c, j);
    } else {
        send_json(c, "{\"error\":\"El query falló\", \"status\": 500}");
    }
    PQclear(r);
}

#endif
