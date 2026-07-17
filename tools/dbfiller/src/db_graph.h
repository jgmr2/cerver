#ifndef DBFILLER_DB_GRAPH_H
#define DBFILLER_DB_GRAPH_H

#include "schema.h"
#include "db/db_backend.h"

typedef struct {
    DbTable *tables; /* owned; length == count, in fill order */
    int count;
} OrderedTables;

/* Loads the schema for every table in table_names and orders them so a
   table referenced by another's foreign key comes first (Kahn's algorithm).
   Tables involved in a cycle or self-reference are appended at the end,
   best-effort — the generator will just leave those FK columns NULL (if
   nullable) or report a failure for that row otherwise. */
int db_graph_load_ordered(const DbBackend *backend, DbConn *conn,
                           char table_names[][MAX_NAME_LEN], int table_count,
                           OrderedTables *out, char *err, size_t err_len);

void db_graph_free(OrderedTables *ordered);

#endif
