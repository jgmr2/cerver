#include "repo_patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} strbuf_t;

static void sb_init(strbuf_t *sb, size_t cap) {
    sb->buf = malloc(cap);
    sb->buf[0] = '\0';
    sb->cap = cap;
    sb->len = 0;
}

static void sb_append_str(strbuf_t *sb, const char *s) {
    size_t n = strlen(s);
    if (sb->len + n >= sb->cap) n = sb->cap - sb->len - 1;
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_append_range(strbuf_t *sb, const char *a, const char *b) {
    size_t n = (size_t)(b - a);
    if (sb->len + n >= sb->cap) n = sb->cap - sb->len - 1;
    memcpy(sb->buf + sb->len, a, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static const char *line_start(const char *content, const char *pos) {
    while (pos > content && *(pos - 1) != '\n') pos--;
    return pos;
}

static const char *line_end(const char *pos) {
    const char *e = strchr(pos, '\n');
    return e ? e + 1 : pos + strlen(pos);
}

static char *file_read_all(const char *path, char *err, size_t err_len) {
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, err_len, "no se pudo abrir '%s'", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    size_t n = fread(buf, 1, (size_t)size, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int file_write_all(const char *path, const char *content, char *err, size_t err_len) {
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(err, err_len, "no se pudo abrir '%s' para escritura", path);
        return -1;
    }
    fputs(content, f);
    fclose(f);
    return 0;
}

/*
 * upsert_block - inserta o reemplaza el bloque
 * "/ * dbfiller:<table>:<tag>:begin * /" .. "/ * dbfiller:<table>:<tag>:end * /"
 * (sin espacios en el codigo real) dentro de 'content'. Si el bloque no
 * existe todavia, se inserta justo antes de la linea que contiene
 * "/ * dbfiller:<tag>-point * /". new_body debe terminar en '\n'.
 *
 * Retorna un buffer nuevo (malloc'd, el llamador debe liberarlo), o
 * NULL si no encontro ni el bloque ni el marcador de punto de insercion
 * (err queda lleno).
 */
static char *upsert_block(const char *content, const char *table, const char *tag, const char *new_body, char *err, size_t err_len) {
    char begin_marker[192], end_marker[192], anchor[160];
    snprintf(begin_marker, sizeof(begin_marker), "/* dbfiller:%s:%s:begin */", table, tag);
    snprintf(end_marker, sizeof(end_marker), "/* dbfiller:%s:%s:end */", table, tag);
    snprintf(anchor, sizeof(anchor), "/* dbfiller:%s-point */", tag);

    strbuf_t out;
    sb_init(&out, strlen(content) + strlen(new_body) + 4096);

    const char *begin_pos = strstr(content, begin_marker);
    if (begin_pos) {
        const char *end_pos = strstr(begin_pos, end_marker);
        if (!end_pos) {
            snprintf(err, err_len, "bloque '%s' sin marcador de cierre correspondiente", begin_marker);
            free(out.buf);
            return NULL;
        }
        const char *seg1_end = line_end(begin_pos);
        sb_append_range(&out, content, seg1_end);
        sb_append_str(&out, new_body);
        const char *seg2_start = line_start(content, end_pos);
        sb_append_range(&out, seg2_start, content + strlen(content));
        return out.buf;
    }

    const char *anchor_pos = strstr(content, anchor);
    if (!anchor_pos) {
        snprintf(err, err_len, "no se encontro el marcador '%s' -- agregalo una vez a mano (ver plan de esta sesion)", anchor);
        free(out.buf);
        return NULL;
    }
    const char *anchor_line = line_start(content, anchor_pos);
    sb_append_range(&out, content, anchor_line);
    sb_append_str(&out, begin_marker);
    sb_append_str(&out, "\n");
    sb_append_str(&out, new_body);
    sb_append_str(&out, end_marker);
    sb_append_str(&out, "\n");
    sb_append_range(&out, anchor_line, content + strlen(content));
    return out.buf;
}

/* patch_file - aplica upsert_block dos veces (includes + tag de cuerpo)
   sobre un archivo, escribiendolo de vuelta si ambos aplicaron bien. */
static int patch_file(const char *path, const char *table, const char *include_line,
                       const char *body_tag, const char *body_lines, char *err, size_t err_len) {
    char *content = file_read_all(path, err, err_len);
    if (!content) return -1;

    char *step1 = upsert_block(content, table, "includes", include_line, err, err_len);
    free(content);
    if (!step1) return -1;

    char *step2 = upsert_block(step1, table, body_tag, body_lines, err, err_len);
    free(step1);
    if (!step2) return -1;

    int rc = file_write_all(path, step2, err, err_len);
    free(step2);
    return rc;
}

int repo_patch_apply(const char *repo_root, const char *table_name, int has_update, char *err, size_t err_len) {
    char path[1024], include_line[256], routes_body[1024], models_body[256];

    snprintf(include_line, sizeof(include_line), "#include \"../controllers/%s.h\"\n", table_name);

    size_t pos = 0;
    pos += (size_t)snprintf(routes_body + pos, sizeof(routes_body) - pos, "    get(\"/api/%s\", list_%s);\n", table_name, table_name);
    pos += (size_t)snprintf(routes_body + pos, sizeof(routes_body) - pos, "    get(\"/api/%s/:id\", get_%s);\n", table_name, table_name);
    pos += (size_t)snprintf(routes_body + pos, sizeof(routes_body) - pos, "    post_auth(\"/api/%s\", create_%s);\n", table_name, table_name);
    if (has_update) {
        pos += (size_t)snprintf(routes_body + pos, sizeof(routes_body) - pos, "    put_auth(\"/api/%s/:id\", update_%s);\n", table_name, table_name);
    }
    snprintf(routes_body + pos, sizeof(routes_body) - pos, "    del_auth(\"/api/%s/:id\", delete_%s);\n", table_name, table_name);

    snprintf(path, sizeof(path), "%s/routes/index.h", repo_root);
    if (patch_file(path, table_name, include_line, "routes", routes_body, err, err_len) != 0) return -1;

    snprintf(include_line, sizeof(include_line), "#include \"../models/%s.h\"\n", table_name);
    snprintf(models_body, sizeof(models_body), "    %s_register();\n", table_name);

    snprintf(path, sizeof(path), "%s/models/registry.h", repo_root);
    if (patch_file(path, table_name, include_line, "models", models_body, err, err_len) != 0) return -1;

    return 0;
}
