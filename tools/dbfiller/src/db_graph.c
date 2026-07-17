#include "db_graph.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int db_graph_load_ordered(const DbBackend *backend, DbConn *conn,
                           char table_names[][MAX_NAME_LEN], int table_count,
                           OrderedTables *out, char *err, size_t err_len) {
    out->tables = NULL;
    out->count = 0;
    if (table_count <= 0) return 0;
    if (table_count > MAX_TABLES) table_count = MAX_TABLES;

    DbTable *all = calloc((size_t)table_count, sizeof(DbTable));
    out->tables = calloc((size_t)table_count, sizeof(DbTable));
    if (!all || !out->tables) {
        snprintf(err, err_len, "out of memory");
        free(all);
        free(out->tables);
        out->tables = NULL;
        return -1;
    }

    for (int i = 0; i < table_count; i++) {
        if (backend->get_table_schema(conn, table_names[i], &all[i]) != 0) {
            snprintf(err, err_len, "%s: %s", table_names[i], backend->last_error(conn));
            for (int k = 0; k < i; k++) db_table_free(&all[k]);
            free(all);
            free(out->tables);
            out->tables = NULL;
            return -1;
        }
    }

    static int depends_count[MAX_TABLES];
    static int adj_count[MAX_TABLES];
    static int dependents[MAX_TABLES][MAX_TABLES];
    static int processed[MAX_TABLES];
    memset(depends_count, 0, sizeof(int) * (size_t)table_count);
    memset(adj_count, 0, sizeof(int) * (size_t)table_count);
    memset(processed, 0, sizeof(int) * (size_t)table_count);

    for (int i = 0; i < table_count; i++) {
        int seen[MAX_TABLES] = {0};
        for (int c = 0; c < all[i].column_count; c++) {
            const DbColumn *col = &all[i].columns[c];
            if (!col->has_fk) continue;
            int j = -1;
            for (int k = 0; k < table_count; k++) {
                if (strcmp(table_names[k], col->fk_table) == 0) { j = k; break; }
            }
            if (j < 0 || j == i || seen[j]) continue;
            seen[j] = 1;
            depends_count[i]++;
            dependents[j][adj_count[j]++] = i;
        }
    }

    int remaining = table_count;
    while (remaining > 0) {
        int progressed = 0;
        for (int i = 0; i < table_count; i++) {
            if (processed[i] || depends_count[i] != 0) continue;
            processed[i] = 1;
            out->tables[out->count++] = all[i];
            remaining--;
            progressed = 1;
            for (int k = 0; k < adj_count[i]; k++) {
                int dep = dependents[i][k];
                if (!processed[dep]) depends_count[dep]--;
            }
        }
        if (!progressed) break; /* cycle among the remaining tables */
    }
    /* Cycle leftovers (or self-references that never reached 0 through
       another cycle) are appended best-effort, in their original order. */
    for (int i = 0; i < table_count; i++) {
        if (!processed[i]) out->tables[out->count++] = all[i];
    }

    free(all);
    return 0;
}

void db_graph_free(OrderedTables *ordered) {
    if (!ordered || !ordered->tables) return;
    for (int i = 0; i < ordered->count; i++) db_table_free(&ordered->tables[i]);
    free(ordered->tables);
    ordered->tables = NULL;
    ordered->count = 0;
}
