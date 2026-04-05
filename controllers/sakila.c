#include "sakila.h"

#include <arpa/inet.h>
#include <stdio.h>

#include "../config/db.h"
#include "../models/sakila.h"
#include "../utils/http/router.h"

static int parse_pg_int(const char *ptr, int len, int *out) {
    if (!ptr || !out) return 0;

    if (len == 2) {
        uint16_t v;
        memcpy(&v, ptr, sizeof(v));
        *out = (int)((int16_t)ntohs(v));
        return 1;
    }

    if (len == 4) {
        uint32_t v;
        memcpy(&v, ptr, sizeof(v));
        *out = (int)ntohl(v);
        return 1;
    }

    if (len == 8) {
        const unsigned char *u = (const unsigned char *)ptr;
        uint64_t v = ((uint64_t)u[0] << 56) |
                     ((uint64_t)u[1] << 48) |
                     ((uint64_t)u[2] << 40) |
                     ((uint64_t)u[3] << 32) |
                     ((uint64_t)u[4] << 24) |
                     ((uint64_t)u[5] << 16) |
                     ((uint64_t)u[6] << 8)  |
                     (uint64_t)u[7];
        *out = (int)((int64_t)v);
        return 1;
    }

    return 0;
}

static size_t append_json_escaped_n(char *dst, size_t cap, size_t pos, const char *src, int src_len) {
    if (!src || src_len <= 0) return pos;

    for (int i = 0; i < src_len && pos + 1 < cap; ++i) {
        unsigned char c = (unsigned char)src[i];

        if ((c == '\\' || c == '"') && pos + 2 < cap) {
            dst[pos++] = '\\';
            dst[pos++] = (char)c;
            continue;
        }

        if (c < 0x20) {
            if (pos + 6 < cap) {
                int n = snprintf(&dst[pos], cap - pos, "\\u%04x", c);
                if (n > 0) pos += (size_t)n;
            }
            continue;
        }

        dst[pos++] = (char)c;
    }

    if (pos < cap) dst[pos] = '\0';
    return pos;
}

static int on_top_films_bin_fetched(struct io_uring *r, int client_fd, PGresult *res) {
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        res_json(r, client_fd, "{\"error\":\"Sakila query failed\"}");
        return 0;
    }

    if (PQnfields(res) < 4) {
        res_json(r, client_fd, "{\"error\":\"Unexpected result shape\"}");
        return 0;
    }

    for (int c = 0; c < 4; ++c) {
        if (PQfformat(res, c) != 1) {
            res_json(r, client_fd, "{\"error\":\"Expected binary rows\"}");
            return 0;
        }
    }

    char json[16384];
    size_t pos = 0;
    int rows = PQntuples(res);

    pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "{\"data\":[");

    for (int i = 0; i < rows && pos + 96 < sizeof(json); ++i) {
        const char *title = PQgetvalue(res, i, 0);
        int title_len = PQgetlength(res, i, 0);

        const char *length_ptr = PQgetisnull(res, i, 1) ? NULL : PQgetvalue(res, i, 1);
        int length_len = PQgetisnull(res, i, 1) ? 0 : PQgetlength(res, i, 1);
        int length = 0;
        (void)parse_pg_int(length_ptr, length_len, &length);

        const char *year_ptr = PQgetisnull(res, i, 2) ? NULL : PQgetvalue(res, i, 2);
        int year_len = PQgetisnull(res, i, 2) ? 0 : PQgetlength(res, i, 2);
        int release_year = 0;
        (void)parse_pg_int(year_ptr, year_len, &release_year);

        const char *rating = PQgetisnull(res, i, 3) ? NULL : PQgetvalue(res, i, 3);
        int rating_len = PQgetisnull(res, i, 3) ? 0 : PQgetlength(res, i, 3);

        pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "{\"title\":\"");
        pos = append_json_escaped_n(json, sizeof(json), pos, title, title_len);
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "\",\"length\":%d,\"release_year\":%d,\"rating\":\"", length, release_year);
        pos = append_json_escaped_n(json, sizeof(json), pos, rating ? rating : "", rating_len);
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "\"}%s", (i < rows - 1) ? "," : "");
    }

    if (pos + 3 < sizeof(json)) {
        json[pos++] = ']';
        json[pos++] = '}';
        json[pos] = '\0';
    }

    res_json(r, client_fd, json);
    return 0;
}

static int on_top_actors_bin_fetched(struct io_uring *r, int client_fd, PGresult *res) {
    if (!res || PQresultStatus(res) != PGRES_TUPLES_OK) {
        res_json(r, client_fd, "{\"error\":\"Sakila query failed\"}");
        return 0;
    }

    if (PQnfields(res) < 3) {
        res_json(r, client_fd, "{\"error\":\"Unexpected result shape\"}");
        return 0;
    }

    for (int c = 0; c < 3; ++c) {
        if (PQfformat(res, c) != 1) {
            res_json(r, client_fd, "{\"error\":\"Expected binary rows\"}");
            return 0;
        }
    }

    char json[16384];
    size_t pos = 0;
    int rows = PQntuples(res);

    pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "{\"data\":[");

    for (int i = 0; i < rows && pos + 96 < sizeof(json); ++i) {
        const char *first = PQgetvalue(res, i, 0);
        int first_len = PQgetlength(res, i, 0);

        const char *last = PQgetvalue(res, i, 1);
        int last_len = PQgetlength(res, i, 1);

        const char *films_ptr = PQgetisnull(res, i, 2) ? NULL : PQgetvalue(res, i, 2);
        int films_len = PQgetisnull(res, i, 2) ? 0 : PQgetlength(res, i, 2);
        int films = 0;
        (void)parse_pg_int(films_ptr, films_len, &films);

        pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "{\"first_name\":\"");
        pos = append_json_escaped_n(json, sizeof(json), pos, first, first_len);
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "\",\"last_name\":\"");
        pos = append_json_escaped_n(json, sizeof(json), pos, last, last_len);
        pos += (size_t)snprintf(json + pos, sizeof(json) - pos, "\",\"films\":%d}%s", films, (i < rows - 1) ? "," : "");
    }

    if (pos + 3 < sizeof(json)) {
        json[pos++] = ']';
        json[pos++] = '}';
        json[pos] = '\0';
    }

    res_json(r, client_fd, json);
    return 0;
}

void get_sakila_top_films(struct io_uring *r, int fd, const char *req_body, const char *req_buf) {
    (void)req_body;
    (void)req_buf;
    Sakila_get_top_films_async(r, fd, on_top_films_bin_fetched);
}

void get_sakila_top_actors(struct io_uring *r, int fd, const char *req_body, const char *req_buf) {
    (void)req_body;
    (void)req_buf;
    Sakila_get_top_actors_async(r, fd, on_top_actors_bin_fetched);
}
