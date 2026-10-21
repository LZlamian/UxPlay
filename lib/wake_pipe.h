/*
 * A pipe a thread's select() also waits on, so it can sleep long between
 * packets and still notice at once that it must stop or that another thread
 * left it work (flush, volume, metadata).  On Windows, where select() takes
 * only sockets, it is a pair of loopback UDP sockets: fds[1] is connected to
 * fds[0].  If it cannot be opened the read end is -1 and callers keep a
 * short poll.
 */
#ifndef WAKE_PIPE_H
#define WAKE_PIPE_H

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#else
#include <string.h>
#include <winsock2.h>
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
#else
    SOCKET reader = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SOCKET writer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr;
    int addr_len = sizeof(addr);
    u_long nonblocking = 1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (reader == INVALID_SOCKET || writer == INVALID_SOCKET ||
        bind(reader, (struct sockaddr *) &addr, sizeof(addr)) != 0 ||
        getsockname(reader, (struct sockaddr *) &addr, &addr_len) != 0 ||
        connect(writer, (struct sockaddr *) &addr, sizeof(addr)) != 0 ||
        ioctlsocket(reader, FIONBIO, &nonblocking) != 0 ||
        ioctlsocket(writer, FIONBIO, &nonblocking) != 0) {
        if (reader != INVALID_SOCKET) closesocket(reader);
        if (writer != INVALID_SOCKET) closesocket(writer);
        return;
    }
    /* the rest of UxPlay keeps sockets in ints too */
    pipe_fds->fds[0] = (int) reader;
    pipe_fds->fds[1] = (int) writer;
#endif
}

static inline void
wake_pipe_close(wake_pipe_t *pipe_fds) {
    for (int i = 0; i < 2; i++) {
#ifndef _WIN32
        if (pipe_fds->fds[i] != -1) close(pipe_fds->fds[i]);
#else
        if (pipe_fds->fds[i] != -1) closesocket((SOCKET) pipe_fds->fds[i]);
#endif
        pipe_fds->fds[i] = -1;
    }
}

static inline void
wake_pipe_signal(wake_pipe_t *pipe_fds) {
#ifndef _WIN32
    if (pipe_fds->fds[1] != -1) {
        const char byte = 1;
        ssize_t ignored = write(pipe_fds->fds[1], &byte, 1);  /* full pipe: already signalled */
        (void) ignored;
    }
#else
    if (pipe_fds->fds[1] != -1) {
        const char byte = 1;
        send((SOCKET) pipe_fds->fds[1], &byte, 1, 0);  /* buffer full: already signalled */
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
#else
    char bytes[64];
    if (pipe_fds->fds[0] == -1) return;
    while (recv((SOCKET) pipe_fds->fds[0], bytes, sizeof(bytes), 0) > 0) {
    }
#endif
}

/* select() timeout: long with a pipe, the old 5 ms poll without one */
static inline long
wake_pipe_timeout_usec(const wake_pipe_t *pipe_fds) {
    return pipe_fds->fds[0] != -1 ? 250000 : 5000;
}

#endif
