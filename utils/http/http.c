#include "http.h"

__thread unsigned char conn_keep_alive[MAX_TRACKED_FD];
