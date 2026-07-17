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

/* Implementadas en utils/http/router.h; se declaran aqui porque este
 * header se incluye antes que router.h en el orden de compilacion. */
void res_json(struct io_uring *r, int f, const char *json);
const char *route_param(const char *name);

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
 *   r        - anillo io_uring del hilo actual
 *   f        - file descriptor del cliente
 *   res      - resultado de la consulta, o NULL si fallo
 *   userdata - sin uso (ver cb en config/db.h)
 *
 * Retorna:
 *   0 siempre (no se queda con la propiedad de 'res', ver el contrato de
 *   cb en config/db.h).
 */
static int on_api(struct io_uring *r, int f, PGresult *res, void *userdata) {
    (void)userdata;
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
    db_query_async(r, f, QUERY_API_TIME, on_api, NULL);
}

/*
 * healthz - handler de GET /healthz
 *
 * A diferencia de /api, no toca la base de datos: responde 200 en cuanto
 * el hilo que recibio la conexion pudo parsear el request, sin esperar
 * ningun round-trip a Postgres. Pensado para que un orquestador (Docker,
 * k8s) distinga "el proceso esta vivo y atendiendo conexiones" de "la
 * base de datos responde" (eso ultimo ya lo cubre /api). Si /api falla
 * pero /healthz sigue en 200, el problema esta en la DB, no en el
 * proceso — dato util para decidir si reiniciar el contenedor o no.
 *
 * Parametros:
 *   r - anillo io_uring del hilo actual
 *   f - file descriptor del cliente
 *   m - metodo HTTP, sin uso (la ruta ya filtro por GET)
 *   b - cuerpo del request, sin uso
 */
static inline void healthz(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m; (void)b;
    /* send_res esta definida en utils/http/http.h; se declara aqui por el
     * mismo motivo que res_json arriba (orden de inclusion de headers). */
    extern void send_res(struct io_uring*, int, const char*, const char*, const char*);
    send_res(r, f, "200 OK", "text/plain", "ok");
}

/*
 * json_escape_into - copia src a dst escapando lo minimo indispensable
 * para que quede seguro como valor string dentro de un JSON armado a
 * mano (comillas, backslash, y caracteres de control)
 *
 * No es de uso general: los modelos de este proyecto (ver
 * controllers/sakila.c) delegan el armado de JSON en Postgres
 * (row_to_json/json_agg), que ya escapa correctamente. Esta funcion
 * existe solo para el handler de ejemplo 'echo' de abajo, que si arma
 * JSON a mano con texto que viene directo del cliente HTTP.
 *
 * Parametros:
 *   dst    - buffer de salida, siempre queda NUL-terminado
 *   dst_sz - tamano de 'dst'
 *   src    - texto de entrada, NUL-terminado
 */
static inline void json_escape_into(char *dst, size_t dst_sz, const char *src) {
    size_t pos = 0;
    for (; *src && pos + 1 < dst_sz; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            if (pos + 2 >= dst_sz) break;
            dst[pos++] = '\\';
            dst[pos++] = (char)c;
        } else if (c < 0x20) {
            if (pos + 6 >= dst_sz) break;
            int n = snprintf(dst + pos, dst_sz - pos, "\\u%04x", c);
            if (n > 0) pos += (size_t)n;
        } else {
            dst[pos++] = (char)c;
        }
    }
    dst[pos < dst_sz ? pos : dst_sz - 1] = '\0';
}

/*
 * echo - handler de GET /api/echo/:msg
 *
 * Ejemplo minimo de una ruta con parametro de path (ver route_param y
 * path_matches en utils/http/router.h): no toca la base de datos, solo
 * demuestra como un handler lee el segmento capturado. Sirve de
 * plantilla para cualquier endpoint que necesite un identificador en la
 * URL en vez de query string (p.ej. "/recursos/:id").
 *
 * El valor de route_param() es texto de un cliente HTTP sin validar; se
 * escapa (json_escape_into) antes de insertarlo en el JSON de
 * respuesta. Un handler que use route_param() para armar una consulta
 * SQL en vez de un JSON tiene que pasarlo como parametro real de un
 * prepared statement (PQexecParams/PQsendQueryParams) — escapar texto a
 * mano sirve para JSON, no es valido como defensa contra SQL injection.
 *
 * Parametros:
 *   r - anillo io_uring del hilo actual
 *   f - file descriptor del cliente
 *   m - metodo HTTP, sin uso
 *   b - cuerpo del request, sin uso
 */
static inline void echo(struct io_uring *r, int f, const char *m, const char *b) {
    (void)m; (void)b;
    const char *msg = route_param("msg");
    char escaped[512];
    json_escape_into(escaped, sizeof(escaped), msg ? msg : "");
    char j[600];
    snprintf(j, sizeof(j), "{\"msg\":\"%s\"}", escaped);
    res_json(r, f, j);
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
