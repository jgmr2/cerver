/*
 * tests/unit/test_pure_functions.c - tests unitarios de las funciones
 * puras del motor (parseo/decisión, sin I/O ni estado)
 *
 * NOMBRE
 *     test_pure_functions - valida url_decode_path, path_has_hidden_segment,
 *     path_looks_like_asset (utils/http/static.h), mime_type_for_path
 *     (utils/http/mime.h) y path_is_api (utils/http/router.h)
 *
 * DESCRIPCION
 *     Estas cinco funciones son las que deciden qué se sirve como estático,
 *     qué cae en el fallback SPA y qué es API — la superficie más sensible
 *     a un refactor del router (ver el plan de middleware en TODO.md) y la
 *     más fácil de romper sin darse cuenta. No hay framework de test acá a
 *     propósito: son funciones puras (mismo input, mismo output, sin
 *     depender de io_uring/Postgres/hilos), así que un harness de 40
 *     líneas alcanza — nada que un framework externo resuelva mejor.
 *
 *     Se compila e incluye a las cabeceras reales del motor tal cual
 *     están, no una copia — si alguien cambia el comportamiento de una de
 *     estas funciones, este archivo lo nota sin tener que actualizar nada
 *     acá (el test llama a la función real, no una reimplementación).
 *
 * COMPILAR Y CORRER
 *     Requiere las mismas cabeceras/libs que el binario principal
 *     (liburing, postgresql, openssl) porque router.h las arrastra por
 *     include aunque este archivo no las use — ver tests/unit/Makefile.
 */
#include <stdio.h>
#include <string.h>

#include "../../utils/http/static.h"
#include "../../utils/http/mime.h"
#include "../../utils/http/router.h"

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(desc, cond)                     \
    do {                                       \
        tests_run++;                           \
        if (!(cond)) {                         \
            tests_failed++;                    \
            printf("FAIL: %s\n", desc);        \
        } else {                               \
            printf("ok:   %s\n", desc);        \
        }                                       \
    } while (0)

static void test_url_decode_path(void) {
    char dst[64];
    CHECK("passthrough simple",
          url_decode_path("hello", dst, sizeof dst) == 1 && strcmp(dst, "hello") == 0);
    CHECK("decodifica %2F a /",
          url_decode_path("%2Fetc", dst, sizeof dst) == 1 && strcmp(dst, "/etc") == 0);
    CHECK("rechaza NUL embebido (%00)",
          url_decode_path("foo%00bar", dst, sizeof dst) == 0);
    CHECK("rechaza overflow del buffer destino",
          url_decode_path("una-cadena-mas-larga-que-el-buffer-destino", dst, 8) == 0);
    CHECK("secuencia %XX no-hexadecimal queda literal",
          url_decode_path("%zz", dst, sizeof dst) == 1 && strcmp(dst, "%zz") == 0);
    CHECK("doble URL-encoding NO se re-decodifica (una sola pasada)",
          url_decode_path("%252e%252e", dst, sizeof dst) == 1 && strcmp(dst, "%2e%2e") == 0);
    CHECK("cadena vacia decodifica a vacia",
          url_decode_path("", dst, sizeof dst) == 1 && dst[0] == '\0');
}

static void test_path_has_hidden_segment(void) {
    CHECK("/index.html no es un segmento oculto",
          !path_has_hidden_segment("/index.html"));
    CHECK("/.git/config es un segmento oculto",
          path_has_hidden_segment("/.git/config"));
    CHECK(".hidden al inicio es un segmento oculto",
          path_has_hidden_segment(".hidden"));
    CHECK("/assets/.env es un segmento oculto",
          path_has_hidden_segment("/assets/.env"));
    CHECK("/assets/file.txt no es un segmento oculto",
          !path_has_hidden_segment("/assets/file.txt"));
}

static void test_path_looks_like_asset(void) {
    CHECK("/index.html parece un asset (tiene punto)",
          path_looks_like_asset("/index.html"));
    CHECK("/about no parece un asset (sin punto, ruta de app)",
          !path_looks_like_asset("/about"));
    CHECK("/users/42 no parece un asset",
          !path_looks_like_asset("/users/42"));
    CHECK("/assets/style.css parece un asset",
          path_looks_like_asset("/assets/style.css"));
}

static void test_mime_type_for_path(void) {
    CHECK("archivo .html",
          strcmp(mime_type_for_path("file.html"), "text/html; charset=utf-8") == 0);
    CHECK("archivo .js",
          strcmp(mime_type_for_path("file.js"), "application/javascript; charset=utf-8") == 0);
    CHECK("extension desconocida cae en octet-stream",
          strcmp(mime_type_for_path("file.unknownext"), "application/octet-stream") == 0);
    CHECK("sin extension cae en octet-stream",
          strcmp(mime_type_for_path("noextension"), "application/octet-stream") == 0);
    CHECK("case-insensitive (.PNG en mayusculas)",
          strcmp(mime_type_for_path("FILE.PNG"), "image/png") == 0);
}

static void test_path_is_api(void) {
    CHECK("/api matchea", path_is_api("/api"));
    CHECK("/api/ matchea", path_is_api("/api/"));
    CHECK("/api/foo matchea", path_is_api("/api/foo"));
    CHECK("/apix NO matchea (evita falso positivo de prefijo)", !path_is_api("/apix"));
    CHECK("/other NO matchea", !path_is_api("/other"));
}

int main(void) {
    test_url_decode_path();
    test_path_has_hidden_segment();
    test_path_looks_like_asset();
    test_mime_type_for_path();
    test_path_is_api();

    printf("\n%d/%d tests ok\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
