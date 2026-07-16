/*
 * controllers/home.h - handler de GET /api y su callback
 *
 * NOMBRE
 *     home.h - handler de ejemplo/health-check y handler generico de 404
 *
 * DESCRIPCION
 *     A diferencia de controllers/sakila.*, este handler esta definido
 *     completo dentro del header (static/inline), por eso no existe un
 *     home.c: es un endpoint pequeno que sirve tanto de plantilla como de
 *     verificacion de que la conexion a Postgres responde.
 */
#pragma once
#include <liburing.h>
#include "../config/db.h"
#include "../models/homeModel.h"

/* Implementada en utils/http/router.h; se declara aqui porque este
 * header se incluye antes que router.h en el orden de compilacion. */
void res_json(struct io_uring *r, int f, const char *json);

/*
 * on_api - callback de la consulta QUERY_API_TIME (SELECT current_timestamp)
 *
 * Sirve como prueba de vida de la base de datos: si la consulta tuvo
 * exito, responde con la cantidad de bytes del valor devuelto y sus
 * primeros 4 bytes en hexadecimal (mas ilustrativo que util, pensado
 * como ejemplo de como leer un PGresult). Si la consulta fallo, responde
 * 503 en el body JSON.
 *
 * Parametros:
 *   r   - anillo io_uring del hilo actual
 *   f   - file descriptor del cliente
 *   res - resultado de la consulta, o NULL si fallo
 *
 * Retorna:
 *   0 siempre (no se queda con la propiedad de 'res', ver el contrato de
 *   cb en config/db.h).
 */
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

/*
 * api - handler de GET /api
 *
 * Dispara QUERY_API_TIME (definida en models/homeModel.h) de forma
 * asincrona; on_api arma y manda la respuesta cuando el resultado llega.
 *
 * Parametros:
 *   r - anillo io_uring del hilo actual
 *   f - file descriptor del cliente
 *   m - metodo HTTP, sin uso (la ruta ya filtro por GET)
 *   b - cuerpo del request, sin uso (GET no trae body relevante aqui)
 */
static inline void api(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m; (void)b;
    db_query_async(r, f, QUERY_API_TIME, on_api);
}


/*
 * error404 - handler generico invocado cuando ninguna ruta hace match
 *
 * Usado directamente por dispatch() en utils/http/router.h cuando un
 * path bajo /api no coincide con ninguna ruta registrada, y tambien
 * puede usarse como handler de fallback explicito para cualquier ruta.
 *
 * Parametros:
 *   r - anillo io_uring del hilo actual
 *   f - file descriptor del cliente
 *   m - metodo HTTP, sin uso
 *   b - cuerpo del request, sin uso
 */
inline void error404(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m; (void)b;
    extern void send_res(struct io_uring*, int, const char*, const char*, const char*);
    send_res(r, f, "404 Not Found", "text/plain", "404");
}
