#ifndef EVENTS_H
#define EVENTS_H

typedef enum {
    EVENT_ACCEPT,
    EVENT_HTTP_READ_INITIAL,
    EVENT_HTTP_HELPER,
    EVENT_DB_POLL
} event_type_t;

#endif