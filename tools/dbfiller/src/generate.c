#include "generate.h"

#include <string.h>

#include "codegen.h"
#include "repo_patch.h"

void dbfiller_generate_table(PGconn *conn, const char *table_name, const char *repo_root, int force, GenerateResult *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->table_name, sizeof(out->table_name), "%s", table_name);

    PgTable table;
    if (pg_load_table(conn, table_name, &table, out->message, sizeof(out->message)) != 0) {
        return;
    }
    if (table.pk_index < 0) {
        snprintf(out->message, sizeof(out->message), "saltada -- no tiene una PRIMARY KEY de una sola columna (no soportado)");
        return;
    }

    int has_update = 0;
    if (codegen_write_table(&table, repo_root, force, &has_update, out->message, sizeof(out->message)) != 0) {
        return;
    }
    if (repo_patch_apply(repo_root, table_name, has_update, out->message, sizeof(out->message)) != 0) {
        return;
    }

    out->ok = 1;
    out->has_update = has_update;
}
