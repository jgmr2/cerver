/*
 * utils/net/conn_limit.c - definicion real de los contadores globales y
 * del arreglo __thread declarados en utils/net/conn_limit.h
 */
#include "conn_limit.h"

atomic_uint g_global_conn_count = 0;
atomic_uint g_ip_bucket_count[CONN_LIMIT_IP_BUCKETS];
__thread uint32_t conn_ip_bucket[CONN_LIMIT_MAX_FD];
__thread uint32_t conn_remote_ip[CONN_LIMIT_MAX_FD];
