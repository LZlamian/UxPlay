/*
 * Reverse-fetched media spool (obs-airplay): see spool.h.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "spool.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "threads.h"

/* libplist-2.3.0  API change */
#ifndef PLIST_230
static void plist_mem_free(void *ptr) {
    if (ptr) {
        free (ptr);
    }
}
#endif

#define MAX_SPOOL_DIRECTORY_BYTES 768U
#define SPOOL_PATH_BYTES 1024U
#define SPOOL_WRITE_CHUNK_BYTES (1024U * 1024U)

struct spool_s {
    logger_t *logger;
    void (*wake)(void *opaque);
    void *wake_opaque;
    mutex_handle_t mutex;
    cond_handle_t cond;
    thread_handle_t thread;
    bool thread_running;
    bool quit;
    spool_job_t *queue;            /* waiting to be written */
    spool_job_t *done;             /* written, to be applied */
    const void *writing_video;
    unsigned long long pending_bytes;
    int delay_for_testing_ms;
};

static void
log_spool_failure(logger_t *logger, const char *stage, int error_number) {
    if (!error_number) error_number = EIO;
    logger_log(logger, LOGGER_ERR,
               "[MEDIA] blob spool failed stage=%s errno=%d (%s)",
               stage, error_number, strerror(error_number));
}

static bool
safe_spool_directory(const char *directory) {
    if (!directory || directory[0] != '/') return false;
    const size_t length = strnlen(directory, MAX_SPOOL_DIRECTORY_BYTES + 1U);
    if (!length || length > MAX_SPOOL_DIRECTORY_BYTES) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char) directory[i];
        if (c < 0x20 || c == 0x7f) return false;
    }
    const char *component = directory;
    while (*component) {
        while (*component == '/') ++component;
        const char *end = component;
        while (*end && *end != '/') ++end;
        const size_t component_length = (size_t) (end - component);
        if ((component_length == 1 && component[0] == '.') ||
            (component_length == 2 && component[0] == '.' && component[1] == '.')) {
            return false;
        }
        component = end;
    }
    struct stat info;
    return stat(directory, &info) == 0 && S_ISDIR(info.st_mode) &&
        access(directory, W_OK | X_OK) == 0;
}

static bool
same_spool_directory(const char *first, const char *second) {
    if (!first || !second) return false;
    size_t first_length = strlen(first);
    size_t second_length = strlen(second);
    while (first_length > 1 && first[first_length - 1] == '/') --first_length;
    while (second_length > 1 && second[second_length - 1] == '/') --second_length;
    return first_length == second_length &&
        !strncmp(first, second, first_length);
}

/* The directories a spool file may be created in, in order of preference.
   user_directory must hold MAX_SPOOL_DIRECTORY_BYTES + 1 bytes. */
static size_t
spool_directory_candidates(char *user_directory, const char *candidates[3]) {
    size_t candidate_count = 0;
    user_directory[0] = '\0';
#ifdef _CS_DARWIN_USER_TEMP_DIR
    const size_t needed = confstr(_CS_DARWIN_USER_TEMP_DIR, NULL, 0);
    if (needed > 1 && needed <= MAX_SPOOL_DIRECTORY_BYTES + 1U &&
        confstr(_CS_DARWIN_USER_TEMP_DIR, user_directory,
                MAX_SPOOL_DIRECTORY_BYTES + 1U) == needed &&
        safe_spool_directory(user_directory)) {
        candidates[candidate_count++] = user_directory;
    }
#endif
    const char *environment_directory = getenv("TMPDIR");
    if (safe_spool_directory(environment_directory) &&
        (candidate_count == 0 ||
         !same_spool_directory(candidates[0], environment_directory))) {
        candidates[candidate_count++] = environment_directory;
    }
    if (candidate_count == 0 ||
        !same_spool_directory(candidates[candidate_count - 1], "/private/tmp")) {
        candidates[candidate_count++] = "/private/tmp";
    }
    return candidate_count;
}

/* Spool files are removed with their playlist item, but not when the host
   application crashes. Called once at start-up: removes this user's spool
   files that are more than a day old (younger ones may belong to another
   running receiver of the same user). */
void
spool_sweep_stale_files(logger_t *logger) {
    char user_directory[MAX_SPOOL_DIRECTORY_BYTES + 1U] = {0};
    const char *candidates[3] = {NULL, NULL, NULL};
    const size_t candidate_count = spool_directory_candidates(user_directory, candidates);
    const time_t now = time(NULL);
    const time_t stale_seconds = 24 * 60 * 60;
    int removed = 0;

    for (size_t candidate = 0; candidate < candidate_count; ++candidate) {
        const char *directory = candidates[candidate];
        if (!safe_spool_directory(directory)) continue;
        DIR *listing = opendir(directory);
        if (!listing) continue;
        struct dirent *entry;
        while ((entry = readdir(listing)) != NULL) {
            if (strncmp(entry->d_name, SPOOL_FILE_PREFIX, strlen(SPOOL_FILE_PREFIX))) continue;
            char path[SPOOL_PATH_BYTES];
            const int path_length = snprintf(path, sizeof(path), "%s%s%s", directory,
                                             directory[strlen(directory) - 1] == '/' ? "" : "/",
                                             entry->d_name);
            if (path_length <= 0 || (size_t) path_length >= sizeof(path)) continue;
            struct stat info;
            if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode) ||
                info.st_uid != getuid() || now - info.st_mtime < stale_seconds) {
                continue;
            }
            if (unlink(path) == 0) ++removed;
        }
        closedir(listing);
    }
    if (removed) {
        logger_log(logger, LOGGER_INFO,
                   "[MEDIA] removed %d stale media spool file(s)", removed);
    }
}

/* Writes reverse-fetched media to a new private spool file and returns its
   path (caller frees), or NULL. Touches no receiver state, so it runs on the
   spool worker thread, never on the httpd thread (obs-airplay).
   delay_ms (tests only) makes the write as slow as a slow disk. */
static char *
spool_write_media(logger_t *logger, const char *data, size_t size, int delay_ms) {
    if (delay_ms > 0) {
        sleepms(delay_ms);
    }
    if (!data || !size || size > MAX_FCUP_MEDIA_BYTES) {
        log_spool_failure(logger, "validate", EINVAL);
        return NULL;
    }
    char user_directory[MAX_SPOOL_DIRECTORY_BYTES + 1U] = {0};
    const char *candidates[3] = {NULL, NULL, NULL};
    const size_t candidate_count = spool_directory_candidates(user_directory, candidates);

    for (size_t candidate = 0; candidate < candidate_count; ++candidate) {
        const char *directory = candidates[candidate];
        if (!safe_spool_directory(directory)) continue;

        char path[SPOOL_PATH_BYTES];
        const int path_length = snprintf(path, sizeof(path), "%s%s" SPOOL_FILE_PREFIX "XXXXXX",
                                         directory,
                                         directory[strlen(directory) - 1] == '/' ? "" : "/");
        if (path_length <= 0 || (size_t) path_length >= sizeof(path)) {
            log_spool_failure(logger, "path", ENAMETOOLONG);
            continue;
        }

        errno = 0;
        int fd = mkstemp(path);
        if (fd < 0) {
            log_spool_failure(logger, "mkstemp", errno);
            continue;
        }
        logger_log(logger, LOGGER_INFO,
                   "[MEDIA] blob spool using validated temp directory %s",
                   directory);

        bool failed = false;
        if (fchmod(fd, S_IRUSR | S_IWUSR) < 0) {
            log_spool_failure(logger, "fchmod", errno);
            failed = true;
        }
        if (!failed) {
            const int descriptor_flags = fcntl(fd, F_GETFD);
            if (descriptor_flags < 0 ||
                fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
                log_spool_failure(logger, "fcntl", errno);
                failed = true;
            }
        }

        size_t written = 0;
        while (!failed && written < size) {
            const size_t remaining = size - written;
            const size_t chunk = remaining < SPOOL_WRITE_CHUNK_BYTES
                ? remaining : SPOOL_WRITE_CHUNK_BYTES;
            const ssize_t result = write(fd, data + written, chunk);
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) {
                log_spool_failure(logger, "write", result < 0 ? errno : EIO);
                failed = true;
                break;
            }
            written += (size_t) result;
        }

        if (close(fd) < 0) {
            log_spool_failure(logger, "close", errno);
            failed = true;
        }
        if (failed) {
            unlink(path);
            continue;
        }

        errno = 0;
        char *copy = strdup(path);
        if (!copy) {
            log_spool_failure(logger, "strdup", errno ? errno : ENOMEM);
            unlink(path);
            continue;
        }
        return copy;
    }

    return NULL;
}

static void
spool_job_release_data(spool_job_t *job) {
    if (job->owner) plist_free(job->owner);
    if (job->owned) plist_mem_free(job->owned);
    job->owner = NULL;
    job->owned = NULL;
    job->data = NULL;
}

void
spool_job_free(spool_job_t *job) {
    if (!job) return;
    spool_job_release_data(job);
    free(job->url);
    free(job->path);
    free(job);
}

spool_t *
spool_create(logger_t *logger, void (*wake)(void *opaque), void *wake_opaque) {
    spool_t *spool = (spool_t *) calloc(1, sizeof(spool_t));
    if (!spool) return NULL;
    spool->logger = logger;
    spool->wake = wake;
    spool->wake_opaque = wake_opaque;
    MUTEX_CREATE(spool->mutex);
    COND_CREATE(spool->cond);
    return spool;
}

static THREAD_RETVAL
spool_worker_thread(void *arg) {
    spool_t *spool = (spool_t *) arg;
    MUTEX_LOCK(spool->mutex);
    while (!spool->quit) {
        spool_job_t *job = spool->queue;
        if (!job) {
            pthread_cond_wait(&spool->cond, &spool->mutex);
            continue;
        }
        spool->queue = job->next;
        job->next = NULL;
        spool->writing_video = job->video;
        const int delay_ms = spool->delay_for_testing_ms;
        MUTEX_UNLOCK(spool->mutex);

        struct timespec started, finished;
        clock_gettime(CLOCK_MONOTONIC, &started);
        job->path = spool_write_media(spool->logger, job->data, job->size, delay_ms);
        clock_gettime(CLOCK_MONOTONIC, &finished);
        job->write_ms = (double) (finished.tv_sec - started.tv_sec) * 1000.0 +
                        (double) (finished.tv_nsec - started.tv_nsec) / 1e6;
        spool_job_release_data(job);

        MUTEX_LOCK(spool->mutex);
        spool->writing_video = NULL;
        spool_job_t **tail = &spool->done;
        while (*tail) tail = &(*tail)->next;
        *tail = job;
        MUTEX_UNLOCK(spool->mutex);
        if (spool->wake) spool->wake(spool->wake_opaque);
        MUTEX_LOCK(spool->mutex);
    }
    MUTEX_UNLOCK(spool->mutex);
    return 0;
}

bool
spool_enqueue(spool_t *spool, spool_job_t *job) {
    MUTEX_LOCK(spool->mutex);
    if (spool->quit) {
        /* spool_destroy has begun (a request still being served): a new
           worker would never be joined */
        MUTEX_UNLOCK(spool->mutex);
        spool_job_free(job);
        return false;
    }
    if (!spool->thread_running) {
        if (pthread_create(&spool->thread, NULL, spool_worker_thread, spool) != 0) {
            MUTEX_UNLOCK(spool->mutex);
            log_spool_failure(spool->logger, "thread", errno ? errno : EAGAIN);
            spool_job_free(job);
            return false;
        }
        spool->thread_running = true;
    }
    spool_job_t **tail = &spool->queue;
    while (*tail) tail = &(*tail)->next;
    *tail = job;
    spool->pending_bytes += job->size;
    pthread_cond_signal(&spool->cond);
    MUTEX_UNLOCK(spool->mutex);
    return true;
}

bool
spool_job_pending_for(spool_t *spool, const void *video) {
    bool pending = false;
    MUTEX_LOCK(spool->mutex);
    for (spool_job_t *job = spool->queue; job && !pending; job = job->next) {
        pending = job->video == video;
    }
    for (spool_job_t *job = spool->done; job && !pending; job = job->next) {
        pending = job->video == video;
    }
    if (!pending && spool->writing_video == video && video) pending = true;
    MUTEX_UNLOCK(spool->mutex);
    return pending;
}

/* Stops the worker (waiting for a file being written) and discards every
   job, removing files that were written but not applied. */
static void
spool_shutdown(spool_t *spool) {
    MUTEX_LOCK(spool->mutex);
    const bool running = spool->thread_running;
    spool->quit = true;
    pthread_cond_signal(&spool->cond);
    MUTEX_UNLOCK(spool->mutex);
    if (running) {
        THREAD_JOIN(spool->thread);
    }
    MUTEX_LOCK(spool->mutex);
    spool->thread_running = false;
    spool_job_t *lists[2] = {spool->queue, spool->done};
    spool->queue = spool->done = NULL;
    spool->pending_bytes = 0;
    MUTEX_UNLOCK(spool->mutex);
    for (int i = 0; i < 2; i++) {
        for (spool_job_t *job = lists[i]; job;) {
            spool_job_t *next = job->next;
            if (job->path) unlink(job->path);
            spool_job_free(job);
            job = next;
        }
    }
}

void
spool_destroy(spool_t *spool) {
    if (!spool) return;
    spool_shutdown(spool);
    MUTEX_DESTROY(spool->mutex);
    COND_DESTROY(spool->cond);
    free(spool);
}

spool_job_t *
spool_take_done(spool_t *spool) {
    MUTEX_LOCK(spool->mutex);
    spool_job_t *done = spool->done;
    spool->done = NULL;
    MUTEX_UNLOCK(spool->mutex);
    return done;
}

unsigned long long
spool_pending_bytes(spool_t *spool) {
    MUTEX_LOCK(spool->mutex);
    const unsigned long long pending = spool->pending_bytes;
    MUTEX_UNLOCK(spool->mutex);
    return pending;
}

void
spool_job_applied(spool_t *spool, const spool_job_t *job) {
    MUTEX_LOCK(spool->mutex);
    spool->pending_bytes -= job->size;
    MUTEX_UNLOCK(spool->mutex);
}

void
spool_set_delay_for_testing(spool_t *spool, int milliseconds) {
    MUTEX_LOCK(spool->mutex);
    spool->delay_for_testing_ms = milliseconds > 0 ? milliseconds : 0;
    MUTEX_UNLOCK(spool->mutex);
}
