/*
 * Reverse-fetched media spool (obs-airplay).
 *
 * Media a sender returns over the reverse channel (a page-local blob or file
 * Safari plays) is written to a private temporary file by a worker thread,
 * so the receiver's single HTTP thread never waits on the disk; the
 * finished file is applied (playback starts) back on the HTTP thread
 * (spool_take_done from its thread_tick).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef SPOOL_H
#define SPOOL_H

#include <stdbool.h>
#include <stddef.h>
#include <plist/plist.h>

#include "logger.h"

#include "media_url.h"

/* the prefix of every spool file name, in the user's temporary folder */
#define SPOOL_FILE_PREFIX MEDIA_SPOOL_FILE_PREFIX
/* one reverse-fetched reply */
#define MAX_FCUP_MEDIA_BYTES (128U * 1024U * 1024U)
/* all spooled items together, on disk */
#define MAX_SPOOL_TOTAL_BYTES (512ULL * 1024ULL * 1024ULL)

typedef struct spool_s spool_t;

/* A reverse-fetched media reply waiting to be written, being written, or
   written and waiting to be applied on the httpd thread. */
typedef struct spool_job_s {
    struct spool_job_s *next;
    const void *video;       /* the item, compared, never dereferenced, off the httpd thread */
    int video_id;
    char *url;               /* the item's reverse-fetch URL */
    plist_t owner;           /* keeps `data` alive (libplist >= 2.1) */
    char *owned;             /* or a copy of it (older libplist) */
    const char *data;
    size_t size;
    char *path;              /* the spool file, once written; NULL if writing failed */
    double write_ms;
} spool_job_t;

/* `wake` (any thread) asks the HTTP thread to call spool_take_done soon. */
spool_t *spool_create(logger_t *logger, void (*wake)(void *opaque), void *wake_opaque);
/* Stops the worker (waiting for a file being written), removes the files
   of jobs not applied, frees the spool. */
void spool_destroy(spool_t *spool);

/* HTTP thread: hands a reply to the worker, starting it on first use.
   Takes the job; false (job freed) when it cannot be queued. */
bool spool_enqueue(spool_t *spool, spool_job_t *job);
/* HTTP thread: a reply for this item is queued or being written. */
bool spool_job_pending_for(spool_t *spool, const void *video);
/* HTTP thread: the written jobs, in order (the caller applies and frees
   them, calling spool_job_applied for each). */
spool_job_t *spool_take_done(spool_t *spool);
void spool_job_applied(spool_t *spool, const spool_job_t *job);
/* Bytes queued or being written, for the spool budget. */
unsigned long long spool_pending_bytes(spool_t *spool);
void spool_job_free(spool_job_t *job);

/* Called once at start-up: removes this user's spool files more than a day
   old (younger ones may belong to another running receiver). */
void spool_sweep_stale_files(logger_t *logger);

/* Tests: makes every write this slow (milliseconds). */
void spool_set_delay_for_testing(spool_t *spool, int milliseconds);

#endif
