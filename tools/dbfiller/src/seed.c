#include "seed.h"
#include "codegen.h" /* is_sensitive_column */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- buffer de texto que crece con realloc. No se comparte el strbuf_t
   de codegen.c porque es static a ese archivo -- este es chico a
   proposito, mismo espiritu, sin compartir estado entre modulos que no
   lo necesitan. ---- */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} sbuf_t;

static void sbuf_init(sbuf_t *sb) {
    sb->cap = 4096;
    sb->buf = malloc(sb->cap);
    sb->buf[0] = '\0';
    sb->len = 0;
}

static void sbuf_append(sbuf_t *sb, const char *fmt, ...) {
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if ((size_t)n < sb->cap - sb->len) {
            sb->len += (size_t)n;
            return;
        }
        size_t need = sb->len + (size_t)n + 1;
        while (sb->cap < need) sb->cap *= 2;
        sb->buf = realloc(sb->buf, sb->cap);
    }
}

/* ---- generador de numeros al azar: sembrado una sola vez por proceso.
   No hace falta que sea criptograficamente fuerte -- esto es dato de
   prueba descartable, no un secreto. ---- */
static void ensure_seeded(void) {
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned)(time(NULL) ^ (long)getpid()));
        seeded = 1;
    }
}

static int rand_range(int lo, int hi) { /* [lo, hi] inclusive */
    ensure_seeded();
    return lo + (int)(rand() % (hi - lo + 1));
}

/* ---- listas fijas para que el texto generado se vea plausible sin
   necesitar una libreria de datos falsos (no existe una en C para esto,
   ver seed.h). Solo caracteres alfanumericos/espacio/@/. a proposito: el
   generador controla el 100% del texto emitido, asi que nunca hace falta
   escapar comillas/backslash para el literal SQL. ---- */
static const char *FIRST_NAMES[] = {"Ana", "Luis", "Marta", "Carlos", "Sofia", "Diego", "Elena", "Pablo", "Lucia", "Mateo"};
#define N_FIRST_NAMES (int)(sizeof(FIRST_NAMES) / sizeof(FIRST_NAMES[0]))
static const char *LAST_NAMES[] = {"Garcia", "Lopez", "Martinez", "Hernandez", "Gonzalez", "Perez", "Sanchez", "Ramirez", "Torres", "Flores"};
#define N_LAST_NAMES (int)(sizeof(LAST_NAMES) / sizeof(LAST_NAMES[0]))
static const char *CITIES[] = {"Guadalajara", "Monterrey", "Puebla", "Queretaro", "Merida", "Toluca", "Tijuana", "Leon"};
#define N_CITIES (int)(sizeof(CITIES) / sizeof(CITIES[0]))
static const char *WORDS[] = {"alpha", "beta", "gamma", "delta", "omega", "nova", "lumen", "vertex", "orbit", "pixel", "cobalto", "aurora", "fenix", "norte", "brisa"};
#define N_WORDS (int)(sizeof(WORDS) / sizeof(WORDS[0]))

static void gen_uuid_v4(char *out, size_t out_sz) {
    unsigned char b[16];
    for (int i = 0; i < 16; i++) b[i] = (unsigned char)rand_range(0, 255);
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40); /* version 4 */
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80); /* variant */
    snprintf(out, out_sz, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
              b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
              b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

/*
 * fake_value_for_column - escribe en 'out' un literal SQL (ya
 * comillado/formateado segun corresponda) para 'col', usando su
 * udt_name y, para texto, una heuristica extra por nombre de columna.
 *
 * No conoce el largo real de un varchar(N) -- schema.h no lo captura
 * (ver su comentario) -- asi que el texto generado se recorta corto (60
 * caracteres) a proposito para minimizar el riesgo de "value too long"
 * contra una columna con un limite chico.
 */
static void fake_value_for_column(const PgColumn *col, char *out, size_t out_sz) {
    const char *t = col->data_type;

    if (strcmp(t, "bool") == 0) {
        snprintf(out, out_sz, "%s", rand_range(0, 1) ? "true" : "false");
        return;
    }
    if (strcmp(t, "int2") == 0 || strcmp(t, "int4") == 0 || strcmp(t, "int8") == 0) {
        snprintf(out, out_sz, "%d", rand_range(1, 10000));
        return;
    }
    if (strcmp(t, "numeric") == 0 || strcmp(t, "float4") == 0 || strcmp(t, "float8") == 0) {
        snprintf(out, out_sz, "%d.%02d", rand_range(1, 999), rand_range(0, 99));
        return;
    }
    if (strcmp(t, "uuid") == 0) {
        char u[40];
        gen_uuid_v4(u, sizeof(u));
        snprintf(out, out_sz, "'%s'", u);
        return;
    }
    if (strcmp(t, "timestamp") == 0 || strcmp(t, "timestamptz") == 0 || strcmp(t, "date") == 0) {
        time_t when = time(NULL) - (time_t)rand_range(0, 60 * 60 * 24 * 365); /* hasta 1 anio atras */
        struct tm tmv;
        gmtime_r(&when, &tmv);
        char stamp[32];
        if (strcmp(t, "date") == 0) strftime(stamp, sizeof(stamp), "%Y-%m-%d", &tmv);
        else strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
        snprintf(out, out_sz, "'%s'", stamp);
        return;
    }
    if (strcmp(t, "json") == 0 || strcmp(t, "jsonb") == 0) {
        snprintf(out, out_sz, "'{}'");
        return;
    }

    /* texto (varchar/text/bpchar/cualquier otro udt_name no reconocido):
       heuristica por nombre de columna para que el dato se vea plausible */
    char lower[MAX_NAME_LEN];
    size_t li = 0;
    for (; col->name[li] && li + 1 < sizeof(lower); li++) lower[li] = (char)tolower((unsigned char)col->name[li]);
    lower[li] = '\0';

    char text[160];
    if (strstr(lower, "email")) {
        char first[32], last[32];
        snprintf(first, sizeof(first), "%s", FIRST_NAMES[rand_range(0, N_FIRST_NAMES - 1)]);
        snprintf(last, sizeof(last), "%s", LAST_NAMES[rand_range(0, N_LAST_NAMES - 1)]);
        for (char *p = first; *p; p++) *p = (char)tolower((unsigned char)*p);
        for (char *p = last; *p; p++) *p = (char)tolower((unsigned char)*p);
        snprintf(text, sizeof(text), "%s.%s%d@example.com", first, last, rand_range(1, 999));
    } else if (strstr(lower, "username") || strstr(lower, "user_name") || strstr(lower, "login") || strstr(lower, "slug") || strstr(lower, "handle")) {
        /* schema.h no captura restricciones UNIQUE (ver su comentario) --
           username/slug/etc son justo el tipo de columna que SUELE tener
           una, asi que acá se prioriza que el valor sea unico dentro del
           lote por encima de que se vea "bonito": nombre en minusculas
           sin espacios + sufijo numerico de alta entropia, en vez de la
           rama generica de "nombre" de abajo (que sí puede repetirse). */
        char first[32];
        snprintf(first, sizeof(first), "%s", FIRST_NAMES[rand_range(0, N_FIRST_NAMES - 1)]);
        for (char *p = first; *p; p++) *p = (char)tolower((unsigned char)*p);
        snprintf(text, sizeof(text), "%s%d", first, rand_range(10000, 999999));
    } else if (strstr(lower, "name") || strstr(lower, "nombre")) {
        snprintf(text, sizeof(text), "%s %s", FIRST_NAMES[rand_range(0, N_FIRST_NAMES - 1)], LAST_NAMES[rand_range(0, N_LAST_NAMES - 1)]);
    } else if (strstr(lower, "phone") || strstr(lower, "telefono")) {
        char digits[11];
        for (int i = 0; i < 10; i++) digits[i] = (char)('0' + rand_range(0, 9));
        digits[10] = '\0';
        snprintf(text, sizeof(text), "+52%s", digits);
    } else if (strstr(lower, "url") || strstr(lower, "link")) {
        snprintf(text, sizeof(text), "https://example.com/%s-%d", WORDS[rand_range(0, N_WORDS - 1)], rand_range(1, 999));
    } else if (strstr(lower, "city") || strstr(lower, "ciudad")) {
        snprintf(text, sizeof(text), "%s", CITIES[rand_range(0, N_CITIES - 1)]);
    } else {
        /* generico (catch-all para code/key/descripcion/lo que no
           matcheo arriba): sufijo numerico incluido por la misma razon
           que username -- podria ser una columna UNIQUE que schema.h no
           puede ver. */
        snprintf(text, sizeof(text), "%s %s %d", WORDS[rand_range(0, N_WORDS - 1)], WORDS[rand_range(0, N_WORDS - 1)], rand_range(1000, 9999));
    }

    if (strlen(text) > 60) text[60] = '\0';
    snprintf(out, out_sz, "'%s'", text);
}

char *seed_build_inserts(const PgTable *table, int row_count) {
    if (row_count < 1) row_count = 1;

    int idx[MAX_COLUMNS];
    int count = 0;
    for (int i = 0; i < table->column_count; i++) {
        const PgColumn *c = &table->columns[i];
        if (c->is_pk && c->has_default) continue; /* Postgres la genera sola (SERIAL/IDENTITY) */
        idx[count++] = i;
    }
    if (count == 0) return NULL;

    sbuf_t sb;
    sbuf_init(&sb);

    sbuf_append(&sb, "INSERT INTO %s (", table->name);
    for (int i = 0; i < count; i++) {
        sbuf_append(&sb, "%s%s", i ? ", " : "", table->columns[idx[i]].name);
    }
    sbuf_append(&sb, ") VALUES\n");

    for (int r = 0; r < row_count; r++) {
        sbuf_append(&sb, "%s(", r ? ",\n" : "");
        for (int i = 0; i < count; i++) {
            const PgColumn *c = &table->columns[idx[i]];
            char value[200];
            if (is_sensitive_column(c->name)) {
                /* Nunca al azar: SEED_PASSWORD_HASH es un hash PBKDF2
                   real (ver seed.h), para que el usuario sembrado quede
                   logueable de verdad con SEED_PASSWORD_PLAINTEXT. */
                snprintf(value, sizeof(value), "'%s'", SEED_PASSWORD_HASH);
            } else if (c->nullable && rand_range(0, 9) == 0) {
                /* 1 de cada 10 columnas nullable queda en NULL a
                   proposito, para ejercitar ese caso tambien en los
                   endpoints generados. */
                snprintf(value, sizeof(value), "NULL");
            } else {
                fake_value_for_column(c, value, sizeof(value));
            }
            sbuf_append(&sb, "%s%s", i ? ", " : "", value);
        }
        sbuf_append(&sb, ")");
    }
    sbuf_append(&sb, ";\n");

    return sb.buf;
}
