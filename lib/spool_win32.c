/*
 * Reverse-fetched media spool files on Windows (obs-airplay): the POSIX
 * version in spool.c relies on mkstemp, file modes and owner IDs.
 *
 * Files are created in the user's own temporary folder (GetTempPathW, which
 * other users cannot list), named obs-airplay-fcup-<16 random hex digits>,
 * with CREATE_NEW (never an existing file or link) and a protected DACL that
 * grants access to this user and SYSTEM only. The host's media policy
 * accepts a local item only if it is such a file owned by this user.
 */
#ifdef _WIN32

#include "spool_file.h"
#include "logger.h"
#include "media_url.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#define SPOOL_PREFIX_W L"" MEDIA_SPOOL_FILE_PREFIX  /* media_url.h */
#define SPOOL_CHUNK_BYTES (1024U * 1024U)
#define SPOOL_STALE_100NS (24ULL * 60ULL * 60ULL * 10000000ULL)

static void
spool_log_failure(logger_t *logger, const char *stage, DWORD error) {
    logger_log(logger, LOGGER_ERR, "[MEDIA] blob spool failed stage=%s win32_error=%lu",
               stage, (unsigned long) error);
}

/* The temporary folder with a trailing backslash, or false. */
static bool
spool_directory(wchar_t directory[MAX_PATH + 1]) {
    const DWORD length = GetTempPathW(MAX_PATH + 1, directory);
    return length > 0 && length <= MAX_PATH && directory[length - 1] == L'\\';
}

/* "D:P(A;;FA;;;<user SID>)(A;;FA;;;SY)" as a security descriptor (LocalFree). */
static PSECURITY_DESCRIPTOR
spool_private_descriptor(void) {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return NULL;
    BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    DWORD returned = 0;
    LPWSTR sid = NULL;
    const BOOL have_user = GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &returned) &&
        ConvertSidToStringSidW(((TOKEN_USER *) (void *) buffer)->User.Sid, &sid);
    CloseHandle(token);
    if (!have_user) return NULL;
    wchar_t sddl[256];
    PSECURITY_DESCRIPTOR descriptor = NULL;
    if (_snwprintf(sddl, sizeof(sddl) / sizeof(sddl[0]), L"D:P(A;;FA;;;%ls)(A;;FA;;;SY)", sid) > 0) {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor,
                                                             NULL);
    }
    LocalFree(sid);
    return descriptor;
}

static char *
spool_utf8(const wchar_t *wide) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (count <= 0) return NULL;
    char *utf8 = (char *) malloc((size_t) count);
    if (utf8) WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, count, NULL, NULL);
    return utf8;
}

char *
spool_win32_write(logger_t *logger, const char *data, size_t size) {
    wchar_t directory[MAX_PATH + 1];
    if (!spool_directory(directory)) {
        spool_log_failure(logger, "temp directory", GetLastError());
        return NULL;
    }
    unsigned char random[8];
    if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, random, sizeof(random),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        spool_log_failure(logger, "random name", ERROR_GEN_FAILURE);
        return NULL;
    }
    wchar_t path[MAX_PATH + 64];
    const int path_length = _snwprintf(path, sizeof(path) / sizeof(path[0]),
        L"%ls" SPOOL_PREFIX_W L"%02x%02x%02x%02x%02x%02x%02x%02x", directory, random[0],
        random[1], random[2], random[3], random[4], random[5], random[6], random[7]);
    if (path_length <= 0) {
        spool_log_failure(logger, "path", ERROR_BUFFER_OVERFLOW);
        return NULL;
    }

    PSECURITY_DESCRIPTOR descriptor = spool_private_descriptor();
    if (!descriptor) {
        spool_log_failure(logger, "security descriptor", GetLastError());
        return NULL;
    }
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, NULL);
    const DWORD create_error = GetLastError();
    LocalFree(descriptor);
    if (file == INVALID_HANDLE_VALUE) {
        spool_log_failure(logger, "create", create_error);
        return NULL;
    }
    logger_log(logger, LOGGER_INFO, "[MEDIA] blob spool using the user temp directory");

    bool failed = false;
    for (size_t written = 0; written < size;) {
        const size_t remaining = size - written;
        const DWORD chunk = (DWORD) (remaining < SPOOL_CHUNK_BYTES ? remaining : SPOOL_CHUNK_BYTES);
        DWORD wrote = 0;
        if (!WriteFile(file, data + written, chunk, &wrote, NULL) || wrote == 0) {
            spool_log_failure(logger, "write", GetLastError());
            failed = true;
            break;
        }
        written += wrote;
    }
    if (!CloseHandle(file) && !failed) {
        spool_log_failure(logger, "close", GetLastError());
        failed = true;
    }
    char *utf8 = failed ? NULL : spool_utf8(path);
    if (!utf8) DeleteFileW(path);
    return utf8;
}

int
spool_win32_sweep(void) {
    wchar_t directory[MAX_PATH + 1];
    if (!spool_directory(directory)) return 0;
    wchar_t pattern[MAX_PATH + 32];
    if (_snwprintf(pattern, sizeof(pattern) / sizeof(pattern[0]), L"%ls" SPOOL_PREFIX_W L"*",
                   directory) <= 0) {
        return 0;
    }
    FILETIME now_time;
    GetSystemTimeAsFileTime(&now_time);
    const unsigned long long now =
        ((unsigned long long) now_time.dwHighDateTime << 32) | now_time.dwLowDateTime;
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileExW(pattern, FindExInfoBasic, &entry, FindExSearchNameMatch, NULL, 0);
    if (find == INVALID_HANDLE_VALUE) return 0;
    int removed = 0;
    do {
        if (entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
            continue;
        }
        const unsigned long long written =
            ((unsigned long long) entry.ftLastWriteTime.dwHighDateTime << 32) |
            entry.ftLastWriteTime.dwLowDateTime;
        if (now < written || now - written < SPOOL_STALE_100NS) continue;
        wchar_t path[MAX_PATH + 300];
        if (_snwprintf(path, sizeof(path) / sizeof(path[0]), L"%ls%ls", directory,
                       entry.cFileName) > 0 && DeleteFileW(path)) {
            ++removed;
        }
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    return removed;
}

#endif /* _WIN32 */
