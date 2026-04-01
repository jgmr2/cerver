#include "db.h"
#include <stdio.h>
PGconn *db_pool[POOL_SIZE];
unsigned int pool_index = 0;
void init_db() {
    for (int i = 0; i < POOL_SIZE; i++) {
        db_pool[i] = PQconnectdb("host=localhost user=tu_usuario dbname=tu_bd");
        if (PQstatus(db_pool[i]) != CONNECTION_OK) {
            fprintf(stderr, "Error en pool[%d]: %s", i, PQerrorMessage(db_pool[i]));
        }
    }
}
