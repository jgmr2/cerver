/*
 * utils/http/router.h - tabla de rutas y despacho de requests
 *
 * NOMBRE
 *     router.h - registro de rutas (get/post/...) y dispatch() por
 *     metodo+path exacto
 *
 * DESCRIPCION
 *     La tabla routes[] es __thread: cada hilo worker la llena una vez
 *     (init_routes(), ver routes/index.h) y la consulta en cada
 *     request. El path registrado puede tener segmentos ":nombre"
 *     (p.ej. "/actores/:id") que matchean cualquier segmento no vacio
 *     del path real; el valor capturado se consulta desde el handler
 *     con route_param("nombre"). No hay wildcards ni segmentos
 *     opcionales: la cantidad de segmentos tiene que coincidir exacto.
 *     Lo que no matchea ninguna ruta cae en try_serve_static()
 *     (utils/http/static.h) o en error404 (controllers/home.h).
 *
 *     Advertencia para quien agregue un handler que use route_param():
 *     el valor es texto de un cliente HTTP, sin validar. Si se usa para
 *     armar una consulta SQL, tiene que ir como parametro real de un
 *     prepared statement (PQexecParams/PQsendQueryParams), nunca
 *     concatenado al texto de la query — igual que cualquier otro dato
 *     de entrada del cliente.
 *
 *     Una ruta registrada con get_auth()/post_auth()/etc. (en vez de
 *     get()/post()/...) exige un JWT valido en el header
 *     "Authorization: Bearer <token>" (ver utils/auth/jwt.h) antes de
 *     invocar el handler; si falta o es invalido/vencido, dispatch()
 *     responde 401 y el handler ni se llama. El handler puede leer los
 *     claims del token con jwt_claim("sub")/jwt_claim("username").
 *
 *     ADVERTENCIA APARTE, MAS IMPORTANTE, PARA CUALQUIER RUTA CON
 *     ":id" (o similar) QUE DEVUELVA UN RECURSO DE UN USUARIO (IDOR,
 *     CWE-639): get_auth()/post_auth()/etc. solo verifican "¿hay un JWT
 *     valido?", NUNCA "¿el dueño de ESTE recurso puntual es el mismo
 *     que el dueño del JWT?". Nada en el router lo hace por vos. Un
 *     handler tipo "GET /api/pedidos/:id" que solo chequea el JWT y
 *     despues busca el pedido por :id sin comparar el owner deja que
 *     cualquier usuario logueado lea el pedido de cualquier otro con
 *     solo cambiar el numero en la URL. Antes de devolver un recurso
 *     identificado por route_param(), comparar su dueño real (la
 *     columna user_id/owner_id de la fila, no algo que venga del
 *     cliente) contra jwt_claim("sub") — usar route_require_owner() de
 *     mas abajo para no reimplementar esto en cada handler.
 */
#ifndef ROUTER_H
#define ROUTER_H

#include <liburing.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "http.h"        /* respuestas HTTP (send_res, send_json, etc.) */
#include "static.h"      /* mount_static() / try_serve_static() */
#include "../auth/jwt.h" /* jwt_verify_and_store() / jwt_claim() */

/* Firma de un handler de ruta: (anillo, fd del cliente, metodo, cuerpo
 * del request). */
typedef void (*handler_t)(struct io_uring*, int, const char*, const char*);

/* Route - una entrada de la tabla: metodo + patron de path y su handler.
 * El patron puede tener segmentos ":nombre" (ver path_matches).
 * requires_auth: 1 si la ruta se registro con get_auth()/post_auth()/etc. */
typedef struct {
    const char *method;
    const char *path;
    handler_t h;
    int requires_auth;
} Route;

#define MAX_ROUTES 128
static __thread Route routes[MAX_ROUTES];
static __thread int route_count = 0;

/*
 * add_route_ex - agrega una entrada a la tabla de rutas del hilo actual
 *
 * Parametros:
 *   m             - metodo HTTP ("GET", "POST", ...)
 *   p             - patron de path a matchear, literal o con segmentos ":nombre"
 *   h             - handler a invocar en caso de match
 *   requires_auth - distinto de 0 si dispatch() debe exigir JWT valido
 *                   antes de invocar el handler
 */
static inline void add_route_ex(const char *m, const char *p, handler_t h, int requires_auth) {
    if (route_count < MAX_ROUTES) routes[route_count++] = (Route){m, p, h, requires_auth};
}

/*
 * add_route - agrega una ruta sin requisito de autenticacion (ver
 * add_route_ex)
 */
static inline void add_route(const char *m, const char *p, handler_t h) {
    add_route_ex(m, p, h, 0);
}

/* Cupo de parametros de ruta capturados por request; una URL con mas
 * segmentos ":nombre" que esto simplemente no matchea (path_matches
 * corta y devuelve 0 al llenarse). 8 alcanza de sobra para cualquier
 * ruta razonable. */
#define MAX_ROUTE_PARAMS 8

/*
 * route_param_t - un parametro de ruta capturado
 *
 * Nombre y valor se copian a buffers propios (no quedan punteros hacia
 * el patron registrado ni hacia el buffer del request) para que
 * route_param() pueda tratarlos siempre como cadenas NUL-terminadas
 * independientes, sin preocuparse de donde vino cada una.
 */
typedef struct {
    char name[32];
    char value[128];
} route_param_t;

/*
 * route_params / route_param_count - parametros capturados por el
 * ultimo path_matches() que dio positivo
 *
 * extern (no static) a proposito, mismo motivo que conn_keep_alive en
 * utils/http/http.h: path_matches() siempre corre inlineada dentro de
 * la unidad de traduccion de core/server.c (via dispatch()), pero
 * route_param() se llama desde CUALQUIER handler — y un handler
 * definido en su propio .c (p.ej. utils/auth/auth.c, o cualquier
 * controllers/<tabla>.c generado por tools/dbfiller) es una unidad de
 * traduccion distinta. Con
 * "static" cada una vería su propia copia privada: server.c escribiria
 * el parametro capturado en SU copia, y el handler leeria una copia
 * distinta, siempre vacia, exactamente el mismo bug que ya paso una vez
 * esta sesion con conn_keep_alive (ver CONCURRENCY.md, punto 1). La
 * definicion real vive en utils/http/router.c.
 */
extern __thread route_param_t route_params[MAX_ROUTE_PARAMS];
extern __thread int route_param_count;

/*
 * path_matches - compara un path real contra un patron de ruta,
 * capturando los segmentos ":nombre" que matcheen
 *
 * Recorre patron y path a la par. Un segmento del patron que arranca
 * con ':' matchea cualquier segmento no vacio del path real (hasta el
 * siguiente '/' o el final de la cadena) y lo captura en
 * route_params[]; cualquier otro caracter tiene que coincidir uno a
 * uno. No hay soporte de wildcards ni de segmentos opcionales: si el
 * patron y el path tienen distinta cantidad de segmentos, no matchea.
 *
 * Reinicia route_param_count al entrar, asi que un intento de match que
 * falla a mitad de camino no deja parametros de una ruta anterior
 * mezclados con los de la ruta que se esta probando ahora.
 *
 * Parametros:
 *   pattern - patron registrado (add_route), puede tener ":nombre"
 *   path    - path real del request
 *
 * Retorna:
 *   distinto de 0 si matchea (con route_params[]/route_param_count ya
 *   actualizados); 0 si no matchea (el contenido de route_params[] en
 *   ese caso no debe usarse).
 */
static inline int path_matches(const char *pattern, const char *path) {
    route_param_count = 0;

    while (1) {
        if (*pattern == '\0' || *path == '\0') return *pattern == *path;

        if (*pattern == ':') {
            const char *name_start = pattern + 1;
            const char *name_end = strchr(name_start, '/');
            size_t name_len = name_end ? (size_t)(name_end - name_start) : strlen(name_start);

            const char *val_start = path;
            const char *val_end = strchr(val_start, '/');
            size_t val_len = val_end ? (size_t)(val_end - val_start) : strlen(val_start);

            if (val_len == 0) return 0;
            if (route_param_count >= MAX_ROUTE_PARAMS) return 0;

            route_param_t *rp = &route_params[route_param_count++];
            size_t ncopy = name_len < sizeof(rp->name) - 1 ? name_len : sizeof(rp->name) - 1;
            memcpy(rp->name, name_start, ncopy);
            rp->name[ncopy] = '\0';
            size_t vcopy = val_len < sizeof(rp->value) - 1 ? val_len : sizeof(rp->value) - 1;
            memcpy(rp->value, val_start, vcopy);
            rp->value[vcopy] = '\0';

            pattern = name_end ? name_end : name_start + name_len;
            path = val_end ? val_end : val_start + val_len;
            continue;
        }

        if (*pattern != *path) return 0;
        pattern++;
        path++;
    }
}

/*
 * route_param - busca el valor de un parametro de ruta capturado en el
 * dispatch() que invoco al handler actual
 *
 * Solo tiene sentido llamarlo desde dentro de un handler (o de algo que
 * el handler invoque en el mismo request): route_params[] se pisa en
 * cada intento de match dentro de dispatch(), no sobrevive mas alla del
 * request que lo disparo.
 *
 * Parametros:
 *   name - nombre del parametro sin los dos puntos (para "/actores/:id"
 *          registrado con add_route, se consulta como route_param("id"))
 *
 * Retorna:
 *   el valor capturado (cadena NUL-terminada), o NULL si la ruta que
 *   matcheo no tiene un parametro con ese nombre.
 */
static inline const char *route_param(const char *name) {
    for (int i = 0; i < route_param_count; i++) {
        if (strcmp(route_params[i].name, name) == 0) return route_params[i].value;
    }
    return NULL;
}

/*
 * route_require_owner - guardrail contra IDOR (CWE-639) para rutas que
 * devuelven un recurso de un usuario puntual
 *
 * Compara el dueño REAL de un recurso (leido de la fila que ya se trajo
 * de la base, nunca de algo que mande el cliente) contra el "sub" del
 * JWT valido de este request. Pensado para llamarse DESPUES de traer la
 * fila (adentro del callback de la consulta, ver cb en config/db.h) y
 * ANTES de mandarsela al cliente — si no coincide, esta funcion ya
 * responde 403 por su cuenta; el caller solo necesita devolver sin hacer
 * nada mas.
 *
 * Solo tiene sentido en un handler registrado con get_auth()/post_auth()/
 * etc. (dispatch() ya garantizo que hay un JWT valido antes de llegar
 * aca, ver la advertencia de IDOR en el comentario de arriba de este
 * archivo); si jwt_claim("sub") fuera NULL aca seria un bug de esta
 * libreria, no del handler que la llama — de todos modos se trata como
 * "no autorizado" en vez de asumir nada.
 *
 * Ejemplo (ver controllers/home.h, me_by_id, para el caso completo):
 *   static int on_pedido_found(struct io_uring *r, int f, PGresult *res, void *ud) {
 *       if (!res || PQntuples(res) < 1) { send_404(r, f); return 0; }
 *       if (!route_require_owner(r, f, PQgetvalue(res, 0, COL_USER_ID))) return 0;
 *       res_json(r, f, ...);
 *       return 0;
 *   }
 *
 * Parametros:
 *   r        - anillo io_uring del hilo actual
 *   fd       - file descriptor del cliente
 *   owner_id - dueño real del recurso (columna de la fila ya traida de
 *              la base, NUNCA route_param() ni ningun otro dato que
 *              venga directo del cliente — eso volveria el chequeo
 *              inutil, el atacante controlaria las dos puntas de la
 *              comparacion)
 *
 * Retorna:
 *   distinto de 0 si el dueño coincide y el handler puede seguir; 0 si
 *   ya se mando 403 (el handler debe retornar de inmediato sin mandar
 *   ninguna otra respuesta).
 */
static inline int route_require_owner(struct io_uring *r, int fd, const char *owner_id) {
    const char *sub = jwt_claim("sub");
    if (!sub || !owner_id || strcmp(sub, owner_id) != 0) {
        send_res(r, fd, "403 Forbidden", "text/plain", "403");
        return 0;
    }
    return 1;
}

/* Azucar sintactica sobre add_route() para cada metodo HTTP soportado;
 * es lo que se usa en routes/index.h (get("/api", api), etc.). */
#define get(p, h)   add_route("GET", p, h)
#define post(p, h)  add_route("POST", p, h)
#define put(p, h)   add_route("PUT", p, h)
#define patch(p, h) add_route("PATCH", p, h)
#define del(p, h)   add_route("DELETE", p, h)

/* Variantes que exigen JWT valido (ver add_route_ex). */
#define get_auth(p, h)   add_route_ex("GET", p, h, 1)
#define post_auth(p, h)  add_route_ex("POST", p, h, 1)
#define put_auth(p, h)   add_route_ex("PUT", p, h, 1)
#define patch_auth(p, h) add_route_ex("PATCH", p, h, 1)
#define del_auth(p, h)   add_route_ex("DELETE", p, h, 1)

/*
 * res_json - fachada de respuesta JSON para los controladores
 *
 * Los controladores llaman a esto sin depender directamente de
 * utils/http/http.h; internamente delega en send_json().
 *
 * Parametros:
 *   r    - anillo io_uring del hilo actual
 *   f    - file descriptor del cliente
 *   json - texto JSON ya armado, listo para mandar como body
 */
static inline void res_json(struct io_uring *r, int f, const char *json) {
    send_json(r, f, json);
}

/* El prefijo /api esta reservado para rutas de backend: si no hubo match
 * exacto en la tabla, es un 404 real. Nunca debe caer en el fallback SPA
 * de static.h, o un typo en un endpoint devolveria el index.html de
 * Svelte con 200 en vez de 404. */
#define API_PREFIX "/api"

/*
 * path_is_api - indica si un path cae bajo el prefijo reservado /api
 *
 * Exige que lo que sigue a "/api" sea '/' o fin de cadena, para no
 * confundir "/apix" con el prefijo reservado.
 *
 * Parametros:
 *   p - path a evaluar
 *
 * Retorna:
 *   distinto de 0 si p empieza con "/api" seguido de '/' o NUL
 */
static inline int path_is_api(const char *p) {
    size_t n = sizeof(API_PREFIX) - 1;
    return strncmp(p, API_PREFIX, n) == 0 && (p[n] == '/' || p[n] == '\0');
}

/*
 * extract_bearer_token - ubica el valor de "Authorization: Bearer
 * <token>" dentro del buffer crudo de un request
 *
 * Busca "\r\nAuthorization:" (con el \r\n de antes) para exigir que
 * este al inicio de una linea de header y no matchear, por ejemplo, el
 * valor de otro header que contenga esa palabra; el resultado se acota
 * ademas a que aparezca antes de la separacion headers/body ("\r\n\r\n"),
 * para no confundir texto del body con un header real.
 *
 * Parametros:
 *   buf    - buffer crudo del request (headers + body)
 *   out    - buffer de salida para el token
 *   out_sz - tamano de 'out'
 *
 * Retorna:
 *   distinto de 0 si se encontro y copio un token (out queda
 *   NUL-terminado), 0 si no hay header Authorization con esquema
 *   Bearer, o el token no entra en out_sz.
 */
static inline int extract_bearer_token(const char *buf, char *out, size_t out_sz) {
    const char *body_sep = strstr(buf, "\r\n\r\n");
    const char *h = strstr(buf, "\r\nAuthorization:");
    if (!h || (body_sep && h >= body_sep)) return 0;

    h += 2 + strlen("Authorization:");
    while (*h == ' ') h++;
    if (strncasecmp(h, "Bearer ", 7) != 0) return 0;
    h += 7;

    const char *end = strstr(h, "\r\n");
    if (!end || (body_sep && end > body_sep)) return 0;

    size_t len = (size_t)(end - h);
    if (len == 0 || len >= out_sz) return 0;
    memcpy(out, h, len);
    out[len] = '\0';
    return 1;
}

/*
 * dispatch - punto de entrada del ruteo para cada request parseado
 *
 * Orden de resolucion:
 *   1. Busqueda lineal en routes[] por metodo exacto y path_matches()
 *      (patron exacto o con segmentos ":nombre", ver mas arriba).
 *   2. Si la ruta que matcheo exige auth (get_auth/post_auth/...) y no
 *      hay un JWT valido en el header Authorization: 401, sin invocar
 *      el handler.
 *   3. Si no hay match y el path cae bajo /api: 404 directo (nunca SPA).
 *   4. Si no, se intenta servir como archivo estatico
 *      (try_serve_static, utils/http/static.h).
 *   5. Si nada de lo anterior aplico: 404.
 *
 * Parametros:
 *   r   - anillo io_uring del hilo actual
 *   fd  - file descriptor del cliente
 *   m   - metodo HTTP del request
 *   p   - path del request
 *   buf - buffer crudo del request (headers + body), se pasa tal cual
 *         al handler que matchee
 */
static inline void dispatch(struct io_uring *r, int fd, const char *m, const char *p, const char *buf) {
    for (int i = 0; i < route_count; i++) {
        if (strcmp(m, routes[i].method) == 0 && path_matches(routes[i].path, p)) {
            if (routes[i].requires_auth) {
                char token[512];
                const char *secret = getenv("JWT_SECRET");
                if (!secret || !extract_bearer_token(buf, token, sizeof(token)) ||
                    !jwt_verify_and_store(secret, token)) {
                    send_res(r, fd, "401 Unauthorized", "text/plain", "401");
                    return;
                }
            }
            routes[i].h(r, fd, m, buf);
            return;
        }
    }
    extern void error404(struct io_uring*, int, const char*, const char*);
    if (path_is_api(p)) return error404(r, fd, m, buf);
    if (try_serve_static(r, fd, m, p)) return;
    error404(r, fd, m, buf);
}

#endif
