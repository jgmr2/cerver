#include "generator.h"
#include "csv_dict.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int seeded = 0;
static void ensure_seeded(void) {
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = 1;
    }
}

static int rand_range(int lo, int hi) {
    return lo + rand() % (hi - lo + 1);
}

static void gen_name_text(char *out, size_t out_len) {
    snprintf(out, out_len, "%s %s", csv_dict_random_first_name(), csv_dict_random_last_name());
}

static void gen_email_text(char *out, size_t out_len) {
    snprintf(out, out_len, "%s.%s%d@%s", csv_dict_random_first_name(), csv_dict_random_last_name(),
              rand_range(1, 9999), csv_dict_random_email_domain());
    for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
}

static void gen_phone_text(char *out, size_t out_len) {
    snprintf(out, out_len, "%03d%03d%04d", rand_range(200, 999), rand_range(200, 999), rand_range(0, 9999));
}

/* Realistic-looking values sourced from the CSV dictionary (see
   src/csv_dict.c), chosen by a keyword heuristic on the column name.
   Falls back to a generic filler pulled from the same dictionary. */
static void gen_text_for_column(const DbColumn *col, char *out, size_t out_len) {
    char lower[MAX_NAME_LEN];
    size_t i = 0;
    for (; col->name[i] && i < sizeof(lower) - 1; i++) lower[i] = (char)tolower((unsigned char)col->name[i]);
    lower[i] = '\0';

    if (strstr(lower, "email")) gen_email_text(out, out_len);
    else if (strstr(lower, "phone") || strstr(lower, "tel")) gen_phone_text(out, out_len);
    else if (strstr(lower, "company") || strstr(lower, "empresa") || strstr(lower, "organiz"))
        snprintf(out, out_len, "%s", csv_dict_random_company());
    else if (strstr(lower, "city") || strstr(lower, "ciudad"))
        snprintf(out, out_len, "%s", csv_dict_random_city());
    else if (strstr(lower, "country") || strstr(lower, "pais") || strstr(lower, "país"))
        snprintf(out, out_len, "%s", csv_dict_random_country());
    else if (strstr(lower, "job") || strstr(lower, "puesto") || strstr(lower, "cargo"))
        snprintf(out, out_len, "%s", csv_dict_random_job_title());
    else if (strstr(lower, "product") || strstr(lower, "producto"))
        snprintf(out, out_len, "%s", csv_dict_random_product_name());
    else if (strstr(lower, "category") || strstr(lower, "categoria"))
        snprintf(out, out_len, "%s", csv_dict_random_category());
    else if (strstr(lower, "color") || strstr(lower, "colour"))
        snprintf(out, out_len, "%s", csv_dict_random_color());
    else if (strstr(lower, "name") || strstr(lower, "nombre")) gen_name_text(out, out_len);
    else snprintf(out, out_len, "%s", csv_dict_random_word());

    if (col->varchar_len > 0 && (int)strlen(out) > col->varchar_len) {
        out[col->varchar_len] = '\0';
    }
}

static void gen_date_text(char *out, size_t out_len, int with_time) {
    int year = rand_range(2000, 2026);
    int month = rand_range(1, 12);
    int day = rand_range(1, 28);
    if (with_time) {
        snprintf(out, out_len, "%04d-%02d-%02d %02d:%02d:%02d", year, month, day,
                  rand_range(0, 23), rand_range(0, 59), rand_range(0, 59));
    } else {
        snprintf(out, out_len, "%04d-%02d-%02d", year, month, day);
    }
}

/* Fills `values[0..table->column_count)`; DB_VAL_TEXT entries own a
   malloc'd string that the caller must free after use. Returns 0 on
   success, -1 if a foreign key could not be resolved (parent table empty). */
static int generate_row(const DbBackend *backend, DbConn *conn, const DbTable *table,
                         DbValue *values, char *err, size_t err_len) {
    for (int i = 0; i < table->column_count; i++) {
        const DbColumn *col = &table->columns[i];
        DbValue *v = &values[i];
        memset(v, 0, sizeof(*v));

        if (col->is_autoincrement) {
            v->type = DB_VAL_NULL;
            continue;
        }
        if (col->nullable && rand_range(1, 10) == 1) {
            v->type = DB_VAL_NULL;
            continue;
        }
        if (col->has_fk) {
            if (backend->sample_column_value(conn, col->fk_table, col->fk_column, v) != 0) {
                snprintf(err, err_len, "FK %.40s.%.40s -> %.40s.%.40s sin filas para muestrear",
                          table->name, col->name, col->fk_table, col->fk_column);
                return -1;
            }
            continue;
        }
        if (col->has_enum && col->enum_count > 0) {
            const char *chosen = col->enum_values[rand_range(0, col->enum_count - 1)];
            v->type = DB_VAL_TEXT;
            v->as_text = strdup(chosen);
            continue;
        }

        char buf[512];
        switch (col->sql_type) {
            case SQL_TYPE_INT:
                v->type = DB_VAL_INT;
                v->as_int = rand_range(1, 100000);
                break;
            case SQL_TYPE_BIGINT:
                v->type = DB_VAL_INT;
                v->as_int = (long long)rand_range(1, 1000000) * rand_range(1, 1000);
                break;
            case SQL_TYPE_REAL:
                v->type = DB_VAL_REAL;
                v->as_real = rand_range(0, 1000000) / 100.0;
                break;
            case SQL_TYPE_BOOL:
                v->type = DB_VAL_INT;
                v->as_int = rand_range(0, 1);
                break;
            case SQL_TYPE_DATE:
                gen_date_text(buf, sizeof(buf), 0);
                v->type = DB_VAL_TEXT;
                v->as_text = strdup(buf);
                break;
            case SQL_TYPE_DATETIME:
                gen_date_text(buf, sizeof(buf), 1);
                v->type = DB_VAL_TEXT;
                v->as_text = strdup(buf);
                break;
            case SQL_TYPE_TEXT:
            case SQL_TYPE_VARCHAR:
            case SQL_TYPE_UNKNOWN:
                gen_text_for_column(col, buf, sizeof(buf));
                v->type = DB_VAL_TEXT;
                v->as_text = strdup(buf);
                break;
            case SQL_TYPE_BLOB:
            case SQL_TYPE_ENUM:
            default:
                v->type = DB_VAL_NULL;
                break;
        }
    }
    return 0;
}

static void free_row_values(const DbTable *table, DbValue *values) {
    for (int i = 0; i < table->column_count; i++) {
        if (values[i].type == DB_VAL_TEXT && values[i].as_text) {
            free(values[i].as_text);
            values[i].as_text = NULL;
        }
    }
}

void generate_and_insert(const DbBackend *backend, DbConn *conn, const DbTable *table,
                          int count, GenerateStats *stats) {
    ensure_seeded();
    memset(stats, 0, sizeof(*stats));
    stats->rows_requested = count;

    DbValue *values = calloc((size_t)table->column_count, sizeof(DbValue));
    if (!values) {
        snprintf(stats->last_error, sizeof(stats->last_error), "out of memory");
        return;
    }

    backend->begin_tx(conn);

    for (int r = 0; r < count; r++) {
        char err[MAX_ERROR_LEN] = {0};
        if (generate_row(backend, conn, table, values, err, sizeof(err)) != 0) {
            stats->rows_failed++;
            snprintf(stats->last_error, sizeof(stats->last_error), "%s", err);
            continue;
        }
        if (backend->insert_row(conn, table->name, table->columns, values, table->column_count) != 0) {
            stats->rows_failed++;
            snprintf(stats->last_error, sizeof(stats->last_error), "%s", backend->last_error(conn));
        } else {
            stats->rows_inserted++;
        }
        free_row_values(table, values);
    }

    backend->commit_tx(conn);
    free(values);
}
