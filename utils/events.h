// utils/events.h
#ifndef UTILS_EVENTS_H
#define UTILS_EVENTS_H

// Supongo que aquí tienes tu enum de eventos:
typedef enum {
    EVENT_ACCEPT,
    EVENT_HTTP_READ_INITIAL,
    EVENT_HTTP_HELPER,
    EVENT_DB_POLL
} event_type_t;

// Añade esta estructura:
typedef struct {
    event_type_t type;
    int client_fd;
    char buf[4096]; // Tu buffer inicial
} initial_read_ctx_t;

#endif