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

typedef struct {
    char prefix[64];
    size_t prefix_len;
    int root_fd;
    int spa;
} static_mount_t;

#define MAX_STATIC_MOUNTS 8
static __thread static_mount_t static_mounts[MAX_STATIC_MOUNTS];
static __thread int static_mount_count = 0;

// Registra un directorio como servible bajo un prefijo, p.ej. mount_static("/", "./public", 1).
// Con spa_fallback=1, si el archivo pedido no existe (y no "parece" un asset con
// extension) se sirve index.html en su lugar, para que el router client-side
// (Svelte, etc.) resuelva la ruta. Con 0 se comporta como un static file server
// estricto: lo que no existe es 404.
// El open() aqui es bloqueante pero corre una sola vez al arrancar el hilo (igual que
// init_db conectando a Postgres) — el hot path por request no toca este costo.
// El fd de directorio (O_PATH) queda vivo para siempre y es lo que ancla RESOLVE_BENEATH
// en serve_file_async: el kernel garantiza que ninguna resolucion async escapa de el.
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

// Decodifica %XX. Rechaza bytes NUL embebidos y overflow del buffer destino.
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

// Cualquier segmento que empiece con '.' se trata como no encontrado. RESOLVE_BENEATH
// ya nos protege de escapar del root; esto es una politica aparte para no exponer
// dotfiles (.env, .git, etc.) que pudieran quedar dentro del directorio servido.
static inline int path_has_hidden_segment(const char *decoded) {
    if (decoded[0] == '.') return 1;
    for (const char *s = decoded; *s; s++) {
        if (s[0] == '/' && s[1] == '.') return 1;
    }
    return 0;
}

// Si el ultimo segmento del path tiene un '.', se asume pedido de un archivo real
// (asset): si no existe, 404 de verdad. Sin extension se asume ruta de la app
// (client-side) y ahi aplica el fallback SPA a index.html.
static inline int path_looks_like_asset(const char *decoded) {
    const char *slash = strrchr(decoded, '/');
    const char *last_segment = slash ? slash + 1 : decoded;
    return strrchr(last_segment, '.') != NULL;
}

// Si el metodo/path calzan con algun mount, dispara el pipeline async de
// serve_file_async y devuelve 1. Devuelve 0 si ningun mount aplica, para que el
// caller siga con su propio fallback (404 de rutas normales).
static inline int try_serve_static(struct io_uring *r, int fd, const char *method, const char *path) {
    if (strcmp(method, "GET") != 0) return 0;

    for (int i = 0; i < static_mount_count; i++) {
        static_mount_t *m = &static_mounts[i];
        if (strncmp(path, m->prefix, m->prefix_len) != 0) continue;
        // Prefijo raiz ("/"): no hay limite que validar, el path entero aplica.
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

        // decoded siempre arranca con '/'; se la quitamos para que openat2 lo
        // resuelva relativo a root_fd (un path absoluto ignoraria el dirfd).
        int use_fallback = m->spa && !path_looks_like_asset(decoded);
        serve_file_async(r, fd, m->root_fd, decoded + 1, mime_type_for_path(decoded), use_fallback);
        return 1;
    }

    return 0;
}

#endif
