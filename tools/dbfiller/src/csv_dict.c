#include "csv_dict.h"

#include "data/customers_csv.h"
#include "data/people_csv.h"
#include "data/organizations_csv.h"
#include "data/products_csv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    char **items;
    int count;
} StrPool;

static StrPool first_names, last_names, companies, cities, countries,
    domains, job_titles, product_names, categories, colors;
static int initialized = 0;

/* In-place RFC4180-ish split: handles quoted fields with embedded commas
   and "" escaping. Mutates `line`, fields point back into it. */
static int split_csv_line(char *line, char *fields[], int max_fields) {
    int n = 0;
    char *p = line;
    while (*p && n < max_fields) {
        if (*p == '"') {
            p++;
            char *start = p, *w = p;
            while (*p) {
                if (*p == '"') {
                    if (p[1] == '"') { *w++ = '"'; p += 2; continue; }
                    p++;
                    break;
                }
                *w++ = *p++;
            }
            *w = '\0';
            fields[n++] = start;
            if (*p == ',') p++;
        } else {
            char *start = p;
            while (*p && *p != ',') p++;
            if (*p == ',') { *p = '\0'; p++; }
            fields[n++] = start;
        }
    }
    return n;
}

static void pool_add(StrPool *pool, const char *value) {
    char **grown = realloc(pool->items, sizeof(char *) * (size_t)(pool->count + 1));
    if (!grown) return;
    pool->items = grown;
    pool->items[pool->count++] = strdup(value);
}

static void parse_csv_column(const char *csv_text, const char *column_name, StrPool *pool) {
    char *buf = strdup(csv_text);
    if (!buf) return;
    char *saveptr = NULL;
    char *line = strtok_r(buf, "\n", &saveptr);
    if (!line) { free(buf); return; }

    char *header_fields[32];
    int header_n = split_csv_line(line, header_fields, 32);
    int col_idx = -1;
    for (int i = 0; i < header_n; i++) {
        if (strcasecmp(header_fields[i], column_name) == 0) { col_idx = i; break; }
    }
    if (col_idx < 0) { free(buf); return; }

    while ((line = strtok_r(NULL, "\n", &saveptr)) != NULL) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';
        if (line[0] == '\0') continue;
        char *fields[32];
        int n = split_csv_line(line, fields, 32);
        if (col_idx < n && fields[col_idx][0] != '\0') pool_add(pool, fields[col_idx]);
    }
    free(buf);
}

static void extract_domains_from_emails(const char *csv_text, const char *column_name) {
    StrPool emails = {0};
    parse_csv_column(csv_text, column_name, &emails);
    for (int i = 0; i < emails.count; i++) {
        const char *at = strchr(emails.items[i], '@');
        if (at) pool_add(&domains, at + 1);
        free(emails.items[i]);
    }
    free(emails.items);
}

static void ensure_init(void) {
    if (initialized) return;
    initialized = 1;

    parse_csv_column(CUSTOMERS_CSV, "First Name", &first_names);
    parse_csv_column(CUSTOMERS_CSV, "Last Name", &last_names);
    parse_csv_column(CUSTOMERS_CSV, "Company", &companies);
    parse_csv_column(CUSTOMERS_CSV, "City", &cities);
    parse_csv_column(CUSTOMERS_CSV, "Country", &countries);
    extract_domains_from_emails(CUSTOMERS_CSV, "Email");

    parse_csv_column(PEOPLE_CSV, "First Name", &first_names);
    parse_csv_column(PEOPLE_CSV, "Last Name", &last_names);
    parse_csv_column(PEOPLE_CSV, "Job Title", &job_titles);
    extract_domains_from_emails(PEOPLE_CSV, "Email");

    parse_csv_column(ORGANIZATIONS_CSV, "Name", &companies);
    parse_csv_column(ORGANIZATIONS_CSV, "Country", &countries);

    parse_csv_column(PRODUCTS_CSV, "Name", &product_names);
    parse_csv_column(PRODUCTS_CSV, "Category", &categories);
    parse_csv_column(PRODUCTS_CSV, "Color", &colors);
}

static const char *pool_random(StrPool *pool, const char *fallback) {
    ensure_init();
    if (pool->count == 0) return fallback;
    return pool->items[rand() % pool->count];
}

const char *csv_dict_random_first_name(void) { return pool_random(&first_names, "Alex"); }
const char *csv_dict_random_last_name(void) { return pool_random(&last_names, "Doe"); }
const char *csv_dict_random_company(void) { return pool_random(&companies, "Acme Inc"); }
const char *csv_dict_random_city(void) { return pool_random(&cities, "Springfield"); }
const char *csv_dict_random_country(void) { return pool_random(&countries, "Mexico"); }
const char *csv_dict_random_email_domain(void) { return pool_random(&domains, "example.com"); }
const char *csv_dict_random_job_title(void) { return pool_random(&job_titles, "Analyst"); }
const char *csv_dict_random_product_name(void) { return pool_random(&product_names, "Generic Product"); }
const char *csv_dict_random_category(void) { return pool_random(&categories, "General"); }
const char *csv_dict_random_color(void) { return pool_random(&colors, "Black"); }

const char *csv_dict_random_word(void) {
    /* Product names double as flavorful generic filler text — more
       realistic than a lorem-ipsum pool for an untyped VARCHAR/TEXT column. */
    return pool_random(&product_names, "dato_generico");
}
