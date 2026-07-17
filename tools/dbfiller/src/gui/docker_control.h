#ifndef DBFILLER_DOCKER_CONTROL_H
#define DBFILLER_DOCKER_CONTROL_H

#include <stddef.h>

#include "../schema.h"

#define MAX_CONTAINERS 64

typedef struct {
    char id[MAX_NAME_LEN];
    char name[MAX_NAME_LEN];
    char image[MAX_NAME_LEN];
    char status[128];
    char ports[192];
    int running; /* derived from status starting with "Up" */
} DockerContainer;

/* Runs `docker ps -a` and fills out[0..max_count) with every container
   (running and stopped). Returns the count on success, -1 on failure (err
   filled with a human-readable reason: docker not installed, daemon not
   reachable, etc). */
int docker_list_containers(DockerContainer *out, int max_count, char *err, size_t err_len);

int docker_start_container(const char *id, char *err, size_t err_len);
int docker_stop_container(const char *id, char *err, size_t err_len);
int docker_restart_container(const char *id, char *err, size_t err_len);

#endif
