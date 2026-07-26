/*
 * utils/auth/login_limit.c - definicion real del arreglo de buckets
 * declarado en utils/auth/login_limit.h
 */
#include "login_limit.h"

login_limit_bucket_t g_login_limit_buckets[LOGIN_LIMIT_BUCKETS];
