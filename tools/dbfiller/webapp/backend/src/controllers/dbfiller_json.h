/*
 * controllers/dbfiller_json.h - helper de escape JSON compartido por
 * todos los controllers de dbfiller-web
 *
 * DESCRIPCION
 *     Mismo criterio que json_escape_into() en controllers/home.h (ese
 *     helper es especifico de su unico call site, 'echo' -- este es el
 *     que usan todos los controllers nuevos: log de jobs, mensajes de
 *     error, nombres de tabla, etc., cualquier texto que no venga ya de
 *     Postgres via row_to_json). No des-escapa nada de entrada -- ver el
 *     mismo comentario en utils/http/json_body.h, aplica igual aca: el
 *     valor siempre se usa como texto de salida o como parametro real de
 *     libpq, nunca concatenado a SQL.
 */
#ifndef DBFILLER_JSON_H
#define DBFILLER_JSON_H

#include <stdio.h>

static inline void dbfiller_json_escape(char *dst, size_t dst_sz, const char *src) {
    size_t pos = 0;
    for (; src && *src && pos + 1 < dst_sz; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            if (pos + 2 >= dst_sz) break;
            dst[pos++] = '\\';
            dst[pos++] = (char)c;
        } else if (c == '\n') {
            if (pos + 2 >= dst_sz) break;
            dst[pos++] = '\\';
            dst[pos++] = 'n';
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
 * dbfiller_json_unescape - resuelve las secuencias de escape basicas
 * (\", \\, \/, \n, \r, \t) de un string ya extraido con json_body_field
 * (utils/http/json_body.h), que a proposito NO des-escapa nada (ver el
 * comentario grande en ese header: para el uso original, parametros de
 * un prepared statement, no hace falta). dbfiller-web si lo necesita
 * para un caso puntual: contenido de un .sql subido por
 * POST /api/schema/upload puede traer comillas dobles (identificadores
 * Postgres "MayusMinus") que, sin este paso, quedarian escritas en disco
 * todavia escapadas (\") en vez del caracter real. \uXXXX NO se resuelve
 * (no hace falta en SQL, y complicaria el helper para un caso que no se
 * va a dar en la practica) -- si aparece, se copia literal.
 */
static inline void dbfiller_json_unescape(char *dst, size_t dst_sz, const char *src) {
    size_t pos = 0;
    for (; src && *src && pos + 1 < dst_sz; src++) {
        if (*src == '\\' && src[1]) {
            char next = src[1];
            char out = 0;
            switch (next) {
                case '"': out = '"'; break;
                case '\\': out = '\\'; break;
                case '/': out = '/'; break;
                case 'n': out = '\n'; break;
                case 'r': out = '\r'; break;
                case 't': out = '\t'; break;
                default: out = 0; break;
            }
            if (out) {
                dst[pos++] = out;
                src++;
                continue;
            }
        }
        dst[pos++] = *src;
    }
    dst[pos < dst_sz ? pos : dst_sz - 1] = '\0';
}

#endif
