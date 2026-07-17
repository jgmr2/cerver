#include "docker_control.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <fcntl.h>

/* Runs argv[0] with the given NULL-terminated argument list via fork+execvp
   (no shell involved - argv entries, including container IDs, are passed
   literally, so nothing here is a command-injection vector), capturing
   stdout into `out` and stderr into `err` separately. `docker`'s own error
   messages ("Cannot connect to the Docker daemon...", "No such container...")
   go to stderr, so callers need that surfaced rather than discarded (unlike
   gui_app.c's run_and_capture_argv, which only ever needs stdout). Returns 1
   if the child could be started at all (regardless of its exit status -
   check *exit_code for that); 0 only if fork/pipe setup itself failed. */
static int docker_run_capture(char *const argv[], char *out, size_t out_size,
                               char *err, size_t err_len, int *exit_code) {
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0) return 0;
    if (pipe(err_pipe) != 0) { close(out_pipe[0]); close(out_pipe[1]); return 0; }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return 0;
    }
    if (pid == 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);
        close(err_pipe[1]);
        execvp(argv[0], argv);
        _exit(127); /* argv[0] not found (docker not installed / not in PATH) */
    }
    close(out_pipe[1]);
    close(err_pipe[1]);

    /* Drain both pipes concurrently via select() rather than reading stdout
       to EOF before even looking at stderr - sequential reads would deadlock
       if the child fills both OS pipe buffers before either is read. A
       scratch buffer soaks up (and discards) whichever stream the caller
       passed NULL for, so the child never blocks trying to write to it. */
    char scratch[4096];
    size_t out_total = 0, err_total = 0;
    int out_done = 0, err_done = 0;
    while (!out_done || !err_done) {
        fd_set readfds;
        FD_ZERO(&readfds);
        int maxfd = -1;
        if (!out_done) { FD_SET(out_pipe[0], &readfds); if (out_pipe[0] > maxfd) maxfd = out_pipe[0]; }
        if (!err_done) { FD_SET(err_pipe[0], &readfds); if (err_pipe[0] > maxfd) maxfd = err_pipe[0]; }

        if (select(maxfd + 1, &readfds, NULL, NULL, NULL) < 0) break;

        if (!out_done && FD_ISSET(out_pipe[0], &readfds)) {
            char *dst = out ? out + out_total : scratch;
            size_t cap = out ? out_size - 1 - out_total : sizeof(scratch);
            ssize_t n = (cap > 0) ? read(out_pipe[0], dst, cap) : 0;
            if (n > 0 && out) out_total += (size_t)n;
            else if (n <= 0) out_done = 1;
        }
        if (!err_done && FD_ISSET(err_pipe[0], &readfds)) {
            char *dst = err ? err + err_total : scratch;
            size_t cap = err ? err_len - 1 - err_total : sizeof(scratch);
            ssize_t n = (cap > 0) ? read(err_pipe[0], dst, cap) : 0;
            if (n > 0 && err) err_total += (size_t)n;
            else if (n <= 0) err_done = 1;
        }
    }
    if (out) out[out_total] = '\0';
    if (err) err[err_total] = '\0';
    close(out_pipe[0]);
    close(err_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    if (out) while (out_total > 0 && (out[out_total - 1] == '\n' || out[out_total - 1] == '\r')) out[--out_total] = '\0';
    if (err) while (err_total > 0 && (err[err_total - 1] == '\n' || err[err_total - 1] == '\r')) err[--err_total] = '\0';
    return 1;
}

/* Turns a docker_run_capture failure (started but exited non-zero, or
   couldn't be started at all) into a message worth showing the user. */
static void fill_run_error(char *err, size_t err_len, int started, int exit_code, const char *raw_err) {
    if (!started) {
        snprintf(err, err_len, "%s", "No se pudo ejecutar el proceso docker.");
    } else if (exit_code == 127) {
        snprintf(err, err_len, "%s", "docker no esta instalado o no esta en el PATH.");
    } else if (raw_err && raw_err[0]) {
        snprintf(err, err_len, "%s", raw_err);
    } else {
        snprintf(err, err_len, "docker salio con codigo %d.", exit_code);
    }
}

int docker_list_containers(DockerContainer *out, int max_count, char *err, size_t err_len) {
    char *argv[] = {
        (char *)"docker", (char *)"ps", (char *)"-a", (char *)"--format",
        (char *)"{{.ID}}\t{{.Names}}\t{{.Image}}\t{{.Status}}\t{{.Ports}}",
        NULL
    };
    char stdout_buf[32768];
    char stderr_buf[1024];
    int exit_code = -1;
    int started = docker_run_capture(argv, stdout_buf, sizeof(stdout_buf), stderr_buf, sizeof(stderr_buf), &exit_code);

    if (!started || exit_code != 0) {
        fill_run_error(err, err_len, started, exit_code, stderr_buf);
        return -1;
    }

    int count = 0;
    char *line = stdout_buf;
    while (line && *line && count < max_count) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (line[0]) {
            DockerContainer *c = &out[count];
            memset(c, 0, sizeof(*c));
            char *fields[5] = {NULL, NULL, NULL, NULL, NULL};
            char *tok = line;
            for (int i = 0; i < 5 && tok; i++) {
                char *tab = strchr(tok, '\t');
                if (tab) *tab = '\0';
                fields[i] = tok;
                tok = tab ? tab + 1 : NULL;
            }
            if (fields[0]) snprintf(c->id, sizeof(c->id), "%s", fields[0]);
            if (fields[1]) snprintf(c->name, sizeof(c->name), "%s", fields[1]);
            if (fields[2]) snprintf(c->image, sizeof(c->image), "%s", fields[2]);
            if (fields[3]) snprintf(c->status, sizeof(c->status), "%s", fields[3]);
            if (fields[4]) snprintf(c->ports, sizeof(c->ports), "%s", fields[4]);
            c->running = fields[3] && strncasecmp(fields[3], "Up", 2) == 0;
            count++;
        }

        line = nl ? nl + 1 : NULL;
    }
    return count;
}

static int docker_action(const char *action, const char *id, char *err, size_t err_len) {
    char *argv[] = { (char *)"docker", (char *)action, (char *)id, NULL };
    char stderr_buf[1024];
    int exit_code = -1;
    int started = docker_run_capture(argv, NULL, 0, stderr_buf, sizeof(stderr_buf), &exit_code);
    if (!started || exit_code != 0) {
        fill_run_error(err, err_len, started, exit_code, stderr_buf);
        return -1;
    }
    return 0;
}

int docker_start_container(const char *id, char *err, size_t err_len) {
    return docker_action("start", id, err, err_len);
}

int docker_stop_container(const char *id, char *err, size_t err_len) {
    return docker_action("stop", id, err, err_len);
}

int docker_restart_container(const char *id, char *err, size_t err_len) {
    return docker_action("restart", id, err, err_len);
}
