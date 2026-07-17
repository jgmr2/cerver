#include "schema.h"
#include <stdlib.h>

void db_table_free(DbTable *table) {
    if (!table) return;
    free(table->columns);
    table->columns = NULL;
    table->column_count = 0;
}
