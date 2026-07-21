/*
 * controllers/dbfiller_shell.h - quoting seguro para comandos de shell
 * armados con snprintf (docker run/compose, psql, ab, etc.)
 *
 * DESCRIPCION
 *     La vieja GUI GTK4 usaba g_shell_quote() (GLib) para esto -- esta
 *     GUI no linkea contra GLib (gtk4 no es una dependencia de
 *     dbfiller-web, ver Makefile), asi que se reimplementa el mismo
 *     algoritmo: envolver en comillas simples, escapando cada comilla
 *     simple literal como '\'' (cerrar comilla, comilla escapada con
 *     backslash, reabrir comilla). Necesario en cualquier lado donde un
 *     valor que llego del cliente HTTP (nombre de archivo, DATABASE_URL,
 *     path) se concatena a una linea de comando para popen()/job_start()
 *     -- sin esto, un valor con comillas o "; rm -rf /" seria
 *     interpretado por el shell en vez de tratado como un solo argumento
 *     literal.
 */
#ifndef DBFILLER_SHELL_H
#define DBFILLER_SHELL_H

#include <stddef.h>

/*
 * shell_quote_into - copia src a dst envuelto en comillas simples,
 * seguro para pegar directo en una linea de comando de shell
 *
 * Retorna 0 en exito, -1 si no entro en dst_sz (dst queda en un estado
 * parcial/indefinido en ese caso -- el llamador debe tratarlo como
 * error, no usar dst).
 */
static inline int shell_quote_into(char *dst, size_t dst_sz, const char *src) {
    size_t pos = 0;
    if (dst_sz < 3) return -1;
    dst[pos++] = '\'';
    for (; *src; src++) {
        if (*src == '\'') {
            if (pos + 4 >= dst_sz) return -1;
            dst[pos++] = '\'';
            dst[pos++] = '\\';
            dst[pos++] = '\'';
            dst[pos++] = '\'';
        } else {
            if (pos + 1 >= dst_sz) return -1;
            dst[pos++] = *src;
        }
    }
    if (pos + 2 > dst_sz) return -1;
    dst[pos++] = '\'';
    dst[pos] = '\0';
    return 0;
}

#endif
