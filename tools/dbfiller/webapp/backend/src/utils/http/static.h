/*
 * utils/http/static.h - montaje y despacho de archivos estaticos
 *
 * NOMBRE
 *     static.h - mount_static() / try_serve_static(), con fallback SPA
 *     opcional y proteccion contra path traversal
 *
 * DESCRIPCION
 *     Registra directorios servibles bajo un prefijo (por ejemplo
 *     mount_static("/", "./public", 1) en routes/index.h) y resuelve
 *     cada GET que matchea ese prefijo contra un file descriptor de
 *     directorio (O_PATH) fijado al arrancar el hilo. La resolucion real
 *     del archivo (apertura, tamano, lectura y escritura por chunks) es
 *     asincrona y vive en serve_file_async() (utils/http/http.h); este
 *     archivo se encarga de decidir *que* archivo pedir y con que
 *     politica (decodificar el path, rechazar dotfiles, elegir si
 *     corresponde el fallback SPA).
 */
#ifndef UTILS_HTTP_STATIC_H
#define UTILS_HTTP_STATIC_H

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <liburing.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "mime.h"

/*
 * static_mount_t - un directorio registrado como servible
 *
 * Campos:
 *   prefix     - prefijo de URL que activa este mount (p.ej. "/")
 *   prefix_len - strlen(prefix), cacheado
 *   root_fd    - file descriptor O_PATH del directorio raiz, abierto una
 *                sola vez en mount_static() y vivo para siempre
 *   spa        - si es distinto de 0, un archivo no encontrado cae a
 *                index.html en vez de 404 (fallback de router
 *                client-side)
 */
typedef struct {
    char prefix[64];
    size_t prefix_len;
    int root_fd;
    int spa;
} static_mount_t;

#define MAX_STATIC_MOUNTS 8
static __thread static_mount_t static_mounts[MAX_STATIC_MOUNTS];
static __thread int static_mount_count = 0;

/*
 * mount_static - registra un directorio como servible bajo un prefijo
 *
 * El open() aqui es bloqueante pero corre una sola vez al arrancar el
 * hilo (igual que init_db conectando a Postgres): el hot path por
 * request no toca este costo. El fd de directorio (O_PATH) queda vivo
 * para siempre y es lo que ancla RESOLVE_BENEATH en serve_file_async: el
 * kernel garantiza que ninguna resolucion async escapa de el.
 *
 * Parametros:
 *   prefix       - prefijo de URL (p.ej. "/", "/assets")
 *   root_dir     - directorio raiz en disco a servir
 *   spa_fallback - distinto de 0 para que un archivo inexistente (sin
 *                  "pinta" de asset, ver path_looks_like_asset) sirva
 *                  index.html en su lugar
 */
static inline void mount_static(const char *prefix, const char *root_dir, int spa_fallback) {
    if (static_mount_count >= MAX_STATIC_MOUNTS) return;

    int fd = open(root_dir, O_PATH | O_DIRECTORY);
    if (fd < 0) {
        fprintf(stderr, "WARN: static mount root '%s' no existe, se omite\n", root_dir);
        return;
    }

    static_mount_t *m = &static_mounts[static_mount_count++];
    snprintf(m->prefix, sizeof(m->prefix), "%s", prefix);
    m->prefix_len = strlen(m->prefix);
    m->root_fd = fd;
    m->spa = spa_fallback;
}

/*
 * url_decode_path - decodifica secuencias %XX de una URL
 *
 * Rechaza bytes NUL embebidos (%00) y overflow del buffer destino.
 *
 * Parametros:
 *   src - path URL-encoded de entrada
 *   dst - buffer de salida decodificado
 *   cap - capacidad de dst
 *
 * Retorna:
 *   1 si se decodifico completo dentro de cap y sin NUL embebidos, 0 en
 *   caso contrario (el caller debe responder 400, no usar dst).
 */
static inline int url_decode_path(const char *src, char *dst, size_t cap) {
    size_t di = 0;
    for (size_t si = 0; src[si]; si++) {
        int byte;
        if (src[si] == '%' && isxdigit((unsigned char)src[si + 1]) && isxdigit((unsigned char)src[si + 2])) {
            char h[3] = {src[si + 1], src[si + 2], 0};
            byte = (int)strtol(h, NULL, 16);
            si += 2;
        } else {
            byte = (unsigned char)src[si];
        }
        if (byte == 0) return 0;
        if (di + 1 >= cap) return 0;
        dst[di++] = (char)byte;
    }
    dst[di] = '\0';
    return 1;
}

/*
 * path_has_hidden_segment - detecta segmentos de path que empiezan con '.'
 *
 * RESOLVE_BENEATH (ver serve_file_async en http.h) ya protege de escapar
 * del root_fd; esto es una politica aparte para no exponer dotfiles
 * (.env, .git, etc.) que pudieran quedar dentro del directorio servido.
 *
 * Parametros:
 *   decoded - path ya decodificado (ver url_decode_path)
 *
 * Retorna:
 *   distinto de 0 si algun segmento del path empieza con '.'
 */
static inline int path_has_hidden_segment(const char *decoded) {
    if (decoded[0] == '.') return 1;
    for (const char *s = decoded; *s; s++) {
        if (s[0] == '/' && s[1] == '.') return 1;
    }
    return 0;
}

/*
 * path_looks_like_asset - distingue un pedido de archivo real de una ruta de app
 *
 * Si el ultimo segmento del path tiene un '.', se asume pedido de un
 * archivo real (asset): si no existe, corresponde 404 real. Sin
 * extension se asume ruta de la app (client-side) y ahi aplica el
 * fallback SPA a index.html.
 *
 * Parametros:
 *   decoded - path ya decodificado
 *
 * Retorna:
 *   distinto de 0 si el ultimo segmento del path contiene un '.'
 */
static inline int path_looks_like_asset(const char *decoded) {
    const char *slash = strrchr(decoded, '/');
    const char *last_segment = slash ? slash + 1 : decoded;
    return strrchr(last_segment, '.') != NULL;
}

/*
 * try_serve_static - intenta resolver un request GET contra los mounts registrados
 *
 * Recorre static_mounts[] buscando uno cuyo prefijo matchee el path;
 * dentro de ese mount decodifica el path, rechaza dotfiles y dispara
 * serve_file_async() (utils/http/http.h) con el fallback SPA que
 * corresponda.
 *
 * Parametros:
 *   r      - anillo io_uring del hilo actual
 *   fd     - file descriptor del cliente
 *   method - metodo HTTP del request (solo GET se sirve como estatico)
 *   path   - path del request
 *
 * Retorna:
 *   1 si algun mount aplico y ya se disparo una respuesta (exitosa,
 *   400, o 404), 0 si ningun mount aplica, para que el caller
 *   (dispatch(), utils/http/router.h) siga con su propio fallback 404.
 */
static inline int try_serve_static(struct io_uring *r, int fd, const char *method, const char *path) {
    if (strcmp(method, "GET") != 0) return 0;

    for (int i = 0; i < static_mount_count; i++) {
        static_mount_t *m = &static_mounts[i];
        if (strncmp(path, m->prefix, m->prefix_len) != 0) continue;
        /* Prefijo raiz ("/"): no hay limite que validar, el path entero aplica. */
        if (m->prefix_len > 1 && path[m->prefix_len] != '/' && path[m->prefix_len] != '\0') continue;

        const char *rel = (m->prefix_len > 1) ? path + m->prefix_len : path;
        char rel_buf[PATH_MAX];
        if (rel[0] == '\0' || (rel[0] == '/' && rel[1] == '\0')) {
            snprintf(rel_buf, sizeof(rel_buf), "/index.html");
        } else {
            snprintf(rel_buf, sizeof(rel_buf), "%s", rel);
        }

        char decoded[PATH_MAX];
        if (!url_decode_path(rel_buf, decoded, sizeof(decoded))) {
            send_res(r, fd, "400 Bad Request", "text/plain", "400");
            return 1;
        }

        if (path_has_hidden_segment(decoded)) {
            send_404(r, fd);
            return 1;
        }

        /* decoded siempre arranca con '/'; se la quitamos para que
         * openat2 lo resuelva relativo a root_fd (un path absoluto
         * ignoraria el dirfd). */
        int use_fallback = m->spa && !path_looks_like_asset(decoded);
        serve_file_async(r, fd, m->root_fd, decoded + 1, mime_type_for_path(decoded), use_fallback);
        return 1;
    }

    return 0;
}

#endif
