#pragma once
#include <liburing.h>
#include "../config/db.h"
#include "../models/homeModel.h"

void res_json(struct io_uring *r, int f, const char *json);

static int on_api(struct io_uring *r, int f, PGresult *res) {
    if (res && PQresultStatus(res) == PGRES_TUPLES_OK) {
        unsigned char *v = (void*)PQgetvalue(res, 0, 0);
        char j[128];
        sprintf(j, "{\"bytes\":%d,\"hex\":\"%02x%02x%02x%02x\"}", 
                PQgetlength(res, 0, 0), v[0], v[1], v[2], v[3]);
        res_json(r, f, j);
    } else {
        res_json(r, f, "{\"e\":503}");
    }
    return 0;
}

static inline void api(struct io_uring *r, int f, const char *m, const char *b) { 
    (void)m; (void)b; 
    db_query_async(r, f, QUERY_API_TIME, on_api); 
}


inline void error404(struct io_uring *r, int f, const char *m, const char *b) { 
    (void)m; (void)b;
    extern void send_res(struct io_uring*, int, const char*, const char*, const char*);
    send_res(r, f, "404 Not Found", "text/plain", "404"); 
}