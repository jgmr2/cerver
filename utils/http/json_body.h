/*
 * utils/http/json_body.h - lectura de un campo escalar de un body JSON
 * plano (un solo nivel de profundidad)
 *
 * NOMBRE
 *     json_body.h - json_body_field(), building block para los
 *     controllers CRUD generados por tools/dbfiller
 *
 * DESCRIPCION
 *     controllers/auth.c ya tiene su propio parse_credentials() a mano
 *     para dos campos fijos (username/password), siempre strings. Los
 *     endpoints generados por dbfiller necesitan lo mismo pero para un
 *     numero variable de columnas de tipos variados (texto, numeros,
 *     booleanos, null), asi que esa logica se factoriza aca en vez de
 *     duplicarse en cada controllers/<tabla>.c generado.
 *
 *     Al igual que parse_credentials, no des-escapa el contenido de un
 *     valor STRING (\", \\, \uXXXX): no es una brecha de seguridad
 *     porque el valor siempre viaja como parametro real de un prepared
 *     statement (nunca concatenado al texto SQL), solo significa que un
 *     valor con comillas/backslash literales llega a Postgres con esos
 *     caracteres de escape tal cual en vez de resueltos.
 */
#ifndef UTILS_HTTP_JSON_BODY_H
#define UTILS_HTTP_JSON_BODY_H

#include <string.h>
#include "http.h"

/*
 * json_body_field - busca "key" en un objeto JSON de un nivel
 * (tokens[0] debe ser JSMN_OBJECT, ver http_parse_json_body) y copia su
 * valor a out como texto crudo, listo para usarse como parametro de un
 * prepared statement (Postgres lo castea segun el tipo real de la
 * columna de destino).
 *
 * Parametros:
 *   body   - puntero al inicio del JSON (out_body de http_parse_json_body)
 *   tokens - tokens ya parseados (http_parse_json_body)
 *   ntok   - cantidad de tokens validos
 *   key    - clave a buscar
 *   out    - buffer de salida
 *   out_sz - tamano de out
 *
 * Retorna:
 *    1  la clave esta presente con un valor usable (out queda NUL-terminado)
 *    0  la clave no esta presente en el body
 *    2  la clave esta presente con el literal JSON null
 *   -1  la clave esta presente pero el valor no entra en out_sz, o no es
 *       un escalar (objeto/arreglo anidado) - el llamador debe responder 400
 */
static inline int json_body_field(const char *body, jsmntok_t *tokens, int ntok, const char *key, char *out, size_t out_sz) {
    if (ntok < 1 || tokens[0].type != JSMN_OBJECT) return -1;

    for (int i = 1; i + 1 < ntok; i += 2) {
        jsmntok_t *k = &tokens[i];
        jsmntok_t *v = &tokens[i + 1];
        if (!json_key_eq(body, k, key)) continue;

        if (v->type != JSMN_STRING && v->type != JSMN_PRIMITIVE) return -1; /* objeto/arreglo anidado, no soportado */

        size_t vlen = (size_t)(v->end - v->start);
        if (v->type == JSMN_PRIMITIVE && vlen == 4 && strncmp(body + v->start, "null", 4) == 0) return 2;
        if (vlen >= out_sz) return -1;
        memcpy(out, body + v->start, vlen);
        out[vlen] = '\0';
        return 1;
    }
    return 0;
}

#endif
