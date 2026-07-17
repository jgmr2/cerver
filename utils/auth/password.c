/*
 * utils/auth/password.c - implementacion de password_hash/password_verify
 * (ver utils/auth/password.h)
 */
#include "password.h"

#include <stdio.h>
#include <string.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>

#define SALT_BYTES 16
#define HASH_BYTES 32
/* Iteraciones de PBKDF2: valor conservador para no introducir latencia
 * notable en login bajo el modelo sin-bloqueo (esto SI bloquea el hilo
 * worker que lo ejecuta, como cualquier hash de contrasenas: es CPU
 * pura, breve, y solo corre en login/registro, no en el camino
 * caliente de las rutas de datos). Subir este numero si el hardware de
 * destino lo permite comodamente. */
#define PBKDF2_ITERATIONS 100000

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
                           PBKDF2_ITERATIONS, EVP_sha256(), sizeof(hash), hash) != 1) {
        return 0;
    }

    char salt_hex[SALT_BYTES * 2 + 1];
    char hash_hex[HASH_BYTES * 2 + 1];
    bytes_to_hex(salt, sizeof(salt), salt_hex);
    bytes_to_hex(hash, sizeof(hash), hash_hex);

    int n = snprintf(out, out_sz, "%s:%s", salt_hex, hash_hex);
    return n > 0 && (size_t)n < out_sz;
}

int password_verify(const char *password, const char *stored) {
    const char *sep = strchr(stored, ':');
    if (!sep) return 0;

    size_t salt_hex_len = (size_t)(sep - stored);
    size_t hash_hex_len = strlen(sep + 1);
    if (salt_hex_len != SALT_BYTES * 2 || hash_hex_len != HASH_BYTES * 2) return 0;

    unsigned char salt[SALT_BYTES];
    unsigned char expected_hash[HASH_BYTES];
    if (!hex_to_bytes(stored, salt, sizeof(salt))) return 0;
    if (!hex_to_bytes(sep + 1, expected_hash, sizeof(expected_hash))) return 0;

    unsigned char computed_hash[HASH_BYTES];
    if (PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, sizeof(salt),
                           PBKDF2_ITERATIONS, EVP_sha256(), sizeof(computed_hash), computed_hash) != 1) {
        return 0;
    }

    /* CRYPTO_memcmp en vez de memcmp: tiempo constante respecto de donde
     * difieren los bytes, para no filtrar informacion util para un
     * ataque de temporizacion sobre cuanto del hash coincidio. */
    return CRYPTO_memcmp(computed_hash, expected_hash, sizeof(computed_hash)) == 0;
}
