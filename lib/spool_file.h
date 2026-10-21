/*
 * Spool file housekeeping that differs by platform (obs-airplay): spool
 * paths are UTF-8, which Windows' narrow file functions do not take.
 */
#ifndef SPOOL_FILE_H
#define SPOOL_FILE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#include <stddef.h>
#include <stdlib.h>
#include <windows.h>

struct logger_s;
/* spool_win32.c: writes a private spool file and returns its UTF-8 path
   (caller frees), or NULL; removes this user's spool files older than a
   day and returns how many. */
char *spool_win32_write(struct logger_s *logger, const char *data, size_t size);
int spool_win32_sweep(void);

static inline wchar_t *
spool_wide_path(const char *path) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (count <= 0) return NULL;
    wchar_t *wide = (wchar_t *) calloc((size_t) count, sizeof(wchar_t));
    if (wide) MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, count);
    return wide;
}

static inline void
spool_file_remove(const char *path) {
    wchar_t *wide = path ? spool_wide_path(path) : NULL;
    if (wide) DeleteFileW(wide);
    free(wide);
}

/* Size of a regular file, false if it is not one. */
static inline bool
spool_file_size(const char *path, unsigned long long *size) {
    WIN32_FILE_ATTRIBUTE_DATA info;
    wchar_t *wide = path ? spool_wide_path(path) : NULL;
    const bool found = wide && GetFileAttributesExW(wide, GetFileExInfoStandard, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    free(wide);
    if (found) *size = ((unsigned long long) info.nFileSizeHigh << 32) | info.nFileSizeLow;
    return found;
}
#else
#include <sys/stat.h>
#include <unistd.h>

static inline void
spool_file_remove(const char *path) {
    if (path) unlink(path);
}

static inline bool
spool_file_size(const char *path, unsigned long long *size) {
    struct stat info;
    if (!path || stat(path, &info) != 0 || !S_ISREG(info.st_mode)) return false;
    *size = (unsigned long long) info.st_size;
    return true;
}
#endif

#endif
