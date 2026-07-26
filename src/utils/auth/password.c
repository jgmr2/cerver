/*
 * utils/auth/password.c - implementacion de password_hash/password_verify
 * (ver utils/auth/password.h)
 */
#include "password.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>

#define SALT_BYTES 16
#define HASH_BYTES 32
/* g_pbkdf2_iterations (utils/auth/password.h, variable de entorno
 * PBKDF2_ITERATIONS): esto SI bloquea el hilo worker que lo ejecuta,
 * como cualquier hash de contrasenas — es CPU pura, breve, y solo corre
 * en login/registro, no en el camino caliente de las rutas de datos. */

static void bytes_to_hex(const unsigned char *in, size_t in_len, char *out) {
    static const char hexchars[] = "0123456789abcdef";
    for (size_t i = 0; i < in_len; i++) {
        out[i * 2] = hexchars[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = hexchars[in[i] & 0xF];
    }
    out[in_len * 2] = '\0';
}

/* hex_to_bytes - decodifica exactamente out_len*2 caracteres hex de
 * 'in' a 'out'. Retorna 1 en exito, 0 si 'in' no tiene la longitud
 * esperada o contiene un caracter no hexadecimal. */
static int hex_to_bytes(const char *in, unsigned char *out, size_t out_len) {
    for (size_t i = 0; i < out_len; i++) {
        int hi = in[i * 2], lo = in[i * 2 + 1];
        int v = 0;
        for (int pass = 0; pass < 2; pass++) {
            int c = pass == 0 ? hi : lo;
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return 0;
            v = (v << 4) | digit;
        }
        out[i] = (unsigned char)v;
    }
    return 1;
}

int password_hash(const char *password, char *out, size_t out_sz) {
    if (out_sz < PASSWORD_HASH_BUF_SIZE) return 0;

    unsigned char salt[SALT_BYTES];
    if (RAND_bytes(salt, sizeof(salt)) != 1) return 0;

    unsigned char hash[HASH_BYTES];
    if (PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, sizeof(salt),
                           g_pbkdf2_iterations, EVP_sha256(), sizeof(hash), hash) != 1) {
        return 0;
    }

    char salt_hex[SALT_BYTES * 2 + 1];
    char hash_hex[HASH_BYTES * 2 + 1];
    bytes_to_hex(salt, sizeof(salt), salt_hex);
    bytes_to_hex(hash, sizeof(hash), hash_hex);

    /* Las iteraciones van adentro del hash guardado (no solo en la
     * variable de entorno): asi, si el dia de mañana se sube
     * PBKDF2_ITERATIONS para hashes nuevos, los usuarios que ya se
     * registraron con el valor viejo siguen pudiendo loguearse — cada
     * hash se verifica con las iteraciones con las que se creo, no con
     * las que este configuradas ahora. */
    int n = snprintf(out, out_sz, "%d:%s:%s", g_pbkdf2_iterations, salt_hex, hash_hex);
    return n > 0 && (size_t)n < out_sz;
}

int password_verify(const char *password, const char *stored) {
    /* Formato: "iteraciones:salt_hex:hash_hex" (ver el comentario en
     * password_hash sobre por que las iteraciones viajan en el string
     * guardado y no se toman de g_pbkdf2_iterations aca). */
    char *end = NULL;
    long iterations = strtol(stored, &end, 10);
    if (end == stored || *end != ':' || iterations < 1) return 0;

    const char *salt_hex = end + 1;
    const char *sep = strchr(salt_hex, ':');
    if (!sep) return 0;

    size_t salt_hex_len = (size_t)(sep - salt_hex);
    const char *hash_hex = sep + 1;
    size_t hash_hex_len = strlen(hash_hex);
    if (salt_hex_len != SALT_BYTES * 2 || hash_hex_len != HASH_BYTES * 2) return 0;

    unsigned char salt[SALT_BYTES];
    unsigned char expected_hash[HASH_BYTES];
    if (!hex_to_bytes(salt_hex, salt, sizeof(salt))) return 0;
    if (!hex_to_bytes(hash_hex, expected_hash, sizeof(expected_hash))) return 0;

    unsigned char computed_hash[HASH_BYTES];
    if (PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, sizeof(salt),
                           (int)iterations, EVP_sha256(), sizeof(computed_hash), computed_hash) != 1) {
        return 0;
    }

    /* CRYPTO_memcmp en vez de memcmp: tiempo constante respecto de donde
     * difieren los bytes, para no filtrar informacion util para un
     * ataque de temporizacion sobre cuanto del hash coincidio. */
    return CRYPTO_memcmp(computed_hash, expected_hash, sizeof(computed_hash)) == 0;
}
