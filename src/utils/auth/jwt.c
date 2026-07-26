/*
 * utils/auth/jwt.c - implementacion de jwt_create/jwt_verify_and_store/
 * jwt_claim (ver utils/auth/jwt.h)
 */
#include "jwt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include "../json.h"

/* Estado del ultimo jwt_verify_and_store exitoso. static (no extern):
 * a diferencia de conn_keep_alive o route_params, nada fuera de este
 * archivo toca estos arreglos directamente — jwt_claim() es una funcion
 * real (no static inline), compilada una sola vez en jwt.o, asi que
 * cualquier .c que la llame pasa siempre por esta misma copia sin
 * importar en que unidad de traduccion este. No hace falta extern
 * cuando el unico acceso es a traves de funciones reales. */
typedef struct {
    char sub[JWT_SUB_MAX];
    char username[JWT_USERNAME_MAX];
} jwt_claims_t;

static __thread jwt_claims_t g_claims;
static __thread int g_claims_valid = 0;

static const char B64URL_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* base64url_encode - sin padding ('='), alfabeto URL-safe. Retorna la
 * cantidad de caracteres escritos (sin contar el NUL final), o 0 si
 * 'out' no alcanza. */
static size_t base64url_encode(const unsigned char *in, size_t in_len, char *out, size_t out_sz) {
    size_t oi = 0, i = 0;
    for (; i + 3 <= in_len; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | (uint32_t)in[i + 2];
        if (oi + 4 >= out_sz) return 0;
        out[oi++] = B64URL_CHARS[(v >> 18) & 0x3F];
        out[oi++] = B64URL_CHARS[(v >> 12) & 0x3F];
        out[oi++] = B64URL_CHARS[(v >> 6) & 0x3F];
        out[oi++] = B64URL_CHARS[v & 0x3F];
    }
    size_t rem = in_len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (oi + 2 >= out_sz) return 0;
        out[oi++] = B64URL_CHARS[(v >> 18) & 0x3F];
        out[oi++] = B64URL_CHARS[(v >> 12) & 0x3F];
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        if (oi + 3 >= out_sz) return 0;
        out[oi++] = B64URL_CHARS[(v >> 18) & 0x3F];
        out[oi++] = B64URL_CHARS[(v >> 12) & 0x3F];
        out[oi++] = B64URL_CHARS[(v >> 6) & 0x3F];
    }
    out[oi] = '\0';
    return oi;
}

static int base64url_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

/* base64url_decode - decodifica exactamente in_len caracteres de 'in'
 * (sin padding). No agrega NUL: el caller es responsable si necesita
 * tratar el resultado como string. Retorna la cantidad de bytes
 * escritos, o 0 si 'out' no alcanza o 'in' trae un caracter invalido. */
static size_t base64url_decode(const char *in, size_t in_len, unsigned char *out, size_t out_sz) {
    size_t oi = 0;
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < in_len; i++) {
        int v = base64url_val((unsigned char)in[i]);
        if (v < 0) return 0;
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (oi >= out_sz) return 0;
            out[oi++] = (unsigned char)((buf >> bits) & 0xFF);
        }
    }
    return oi;
}

/* jwt_key_eq - igual que json_key_eq (utils/http/http.h), duplicada
 * aca a proposito: este modulo no depende de utils/http, la relacion
 * de dependencia va al reves (el router depende de auth, no auth del
 * router). */
static int jwt_key_eq(const char *json_str, const jsmntok_t *tok, const char *key) {
    return tok->type == JSMN_STRING &&
           (int)strlen(key) == tok->end - tok->start &&
           strncmp(json_str + tok->start, key, (size_t)(tok->end - tok->start)) == 0;
}

static void jwt_copy_token(const char *json_str, const jsmntok_t *tok, char *out, size_t out_sz) {
    size_t len = (size_t)(tok->end - tok->start);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, json_str + tok->start, len);
    out[len] = '\0';
}

#define JWT_HEADER_JSON "{\"alg\":\"HS256\",\"typ\":\"JWT\"}"

int jwt_create(const char *secret, const char *sub, const char *username, long expires_in_seconds, char *out, size_t out_sz) {
    char header_b64[64];
    if (base64url_encode((const unsigned char *)JWT_HEADER_JSON, strlen(JWT_HEADER_JSON), header_b64, sizeof(header_b64)) == 0) {
        return 0;
    }

    time_t now = time(NULL);
    long exp = (long)now + expires_in_seconds;

    char payload_json[512];
    int pn = snprintf(payload_json, sizeof(payload_json), "{\"sub\":\"%s\",\"username\":\"%s\",\"iat\":%ld,\"exp\":%ld}", sub, username, (long)now, exp);
    if (pn < 0 || (size_t)pn >= sizeof(payload_json)) return 0;

    char payload_b64[700];
    if (base64url_encode((const unsigned char *)payload_json, (size_t)pn, payload_b64, sizeof(payload_b64)) == 0) {
        return 0;
    }

    char signing_input[800];
    int sn = snprintf(signing_input, sizeof(signing_input), "%s.%s", header_b64, payload_b64);
    if (sn < 0 || (size_t)sn >= sizeof(signing_input)) return 0;

    unsigned char sig[EVP_MAX_MD_SIZE];
    unsigned int sig_len = 0;
    if (!HMAC(EVP_sha256(), secret, (int)strlen(secret), (const unsigned char *)signing_input, (size_t)sn, sig, &sig_len)) {
        return 0;
    }

    char sig_b64[64];
    if (base64url_encode(sig, sig_len, sig_b64, sizeof(sig_b64)) == 0) return 0;

    int n = snprintf(out, out_sz, "%s.%s", signing_input, sig_b64);
    return n > 0 && (size_t)n < out_sz;
}

int jwt_verify_and_store(const char *secret, const char *token) {
    g_claims_valid = 0;
    if (!secret || !token) return 0;

    const char *dot1 = strchr(token, '.');
    if (!dot1) return 0;
    const char *dot2 = strchr(dot1 + 1, '.');
    if (!dot2) return 0;
    if (strchr(dot2 + 1, '.')) return 0; /* un token bien formado tiene exactamente 2 puntos */

    size_t payload_len = (size_t)(dot2 - dot1 - 1);
    const char *sig_b64 = dot2 + 1;
    size_t sig_b64_len = strlen(sig_b64);
    size_t signing_input_len = (size_t)(dot2 - token);
    if (dot1 == token || payload_len == 0 || sig_b64_len == 0) return 0;

    /* Recalcula la firma esperada sobre "header.payload" tal cual llego
     * (sin decodificar ni reinterpretar el header): siempre HS256, ver
     * el comentario de utils/auth/jwt.h sobre confusion de algoritmo. */
    unsigned char expected_sig[EVP_MAX_MD_SIZE];
    unsigned int expected_sig_len = 0;
    if (!HMAC(EVP_sha256(), secret, (int)strlen(secret), (const unsigned char *)token, signing_input_len, expected_sig, &expected_sig_len)) {
        return 0;
    }

    char expected_sig_b64[64];
    size_t expected_sig_b64_len = base64url_encode(expected_sig, expected_sig_len, expected_sig_b64, sizeof(expected_sig_b64));
    if (expected_sig_b64_len == 0) return 0;

    /* Comparacion en tiempo constante: la longitud esperada es publica
     * (siempre la misma para HS256), asi que compararla primero no
     * filtra nada; CRYPTO_memcmp evita filtrar en que byte difirio. */
    if (expected_sig_b64_len != sig_b64_len ||
        CRYPTO_memcmp(expected_sig_b64, sig_b64, expected_sig_b64_len) != 0) {
        return 0;
    }

    unsigned char payload_raw[512];
    size_t payload_raw_len = base64url_decode(dot1 + 1, payload_len, payload_raw, sizeof(payload_raw) - 1);
    if (payload_raw_len == 0) return 0;
    payload_raw[payload_raw_len] = '\0';

    jsmntok_t tokens[16];
    jsmn_parser p;
    jsmn_init(&p);
    int ntok = jsmn_parse(&p, (const char *)payload_raw, payload_raw_len, tokens, 16);
    if (ntok < 1 || tokens[0].type != JSMN_OBJECT) return 0;

    jwt_claims_t claims = {0};
    long exp = -1;
    for (int i = 1; i + 1 < ntok; i += 2) {
        jsmntok_t *key = &tokens[i];
        jsmntok_t *val = &tokens[i + 1];
        if (jwt_key_eq((const char *)payload_raw, key, "sub")) {
            jwt_copy_token((const char *)payload_raw, val, claims.sub, sizeof(claims.sub));
        } else if (jwt_key_eq((const char *)payload_raw, key, "username")) {
            jwt_copy_token((const char *)payload_raw, val, claims.username, sizeof(claims.username));
        } else if (jwt_key_eq((const char *)payload_raw, key, "exp") && val->type == JSMN_PRIMITIVE) {
            exp = strtol((const char *)payload_raw + val->start, NULL, 10);
        }
    }

    if (exp < 0 || (long)time(NULL) > exp) return 0; /* sin exp, o vencido */

    g_claims = claims;
    g_claims_valid = 1;
    return 1;
}

const char *jwt_claim(const char *name) {
    if (!g_claims_valid) return NULL;
    if (strcmp(name, "sub") == 0) return g_claims.sub;
    if (strcmp(name, "username") == 0) return g_claims.username;
    return NULL;
}
