/*
 * A pipe a thread's select() also waits on, so it can sleep long between
 * packets and still notice at once that it must stop or that another thread
 * left it work (flush, volume, metadata).  On Windows (no pipes in select)
 * the read end is -1 and callers keep a short poll.
 */
#ifndef WAKE_PIPE_H
#define WAKE_PIPE_H

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

typedef struct {
    int fds[2];
} wake_pipe_t;

static inline void
wake_pipe_open(wake_pipe_t *pipe_fds) {
    pipe_fds->fds[0] = pipe_fds->fds[1] = -1;
#ifndef _WIN32
    if (pipe(pipe_fds->fds) != 0) {
        pipe_fds->fds[0] = pipe_fds->fds[1] = -1;
        return;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(pipe_fds->fds[i], F_SETFL, fcntl(pipe_fds->fds[i], F_GETFL) | O_NONBLOCK);
        fcntl(pipe_fds->fds[i], F_SETFD, FD_CLOEXEC);
    }
#endif
}

static inline void
wake_pipe_close(wake_pipe_t *pipe_fds) {
#ifndef _WIN32
    for (int i = 0; i < 2; i++) {
        if (pipe_fds->fds[i] != -1) close(pipe_fds->fds[i]);
        pipe_fds->fds[i] = -1;
    }
#endif
}

static inline void
wake_pipe_signal(wake_pipe_t *pipe_fds) {
#ifndef _WIN32
    if (pipe_fds->fds[1] != -1) {
        const char byte = 1;
        ssize_t ignored = write(pipe_fds->fds[1], &byte, 1);  /* full pipe: already signalled */
        (void) ignored;
    }
#endif
}

static inline void
wake_pipe_drain(wake_pipe_t *pipe_fds) {
#ifndef _WIN32
    char bytes[64];
    if (pipe_fds->fds[0] == -1) return;
    while (read(pipe_fds->fds[0], bytes, sizeof(bytes)) > 0) {
    }
#endif
}

/* select() timeout: long with a pipe, the old 5 ms poll without one */
static inline long
wake_pipe_timeout_usec(const wake_pipe_t *pipe_fds) {
    return pipe_fds->fds[0] != -1 ? 250000 : 5000;
}

#endif
