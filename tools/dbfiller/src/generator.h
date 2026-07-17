#ifndef DBFILLER_GENERATOR_H
#define DBFILLER_GENERATOR_H

#include "schema.h"
#include "db/db_backend.h"

typedef struct {
    int rows_requested;
    int rows_inserted;
    int rows_failed;
    char last_error[MAX_ERROR_LEN];
} GenerateStats;

/* Generates `count` rows respecting each column's type/constraints and
   inserts them into `table` through `backend`/`conn`, wrapped in a single
   transaction. Rows that can't be generated or fail to insert (e.g. a
   UNIQUE violation) are counted as failures but do not abort the batch. */
void generate_and_insert(const DbBackend *backend, DbConn *conn, const DbTable *table,
                          int count, GenerateStats *stats);

#endif
