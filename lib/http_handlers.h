/**
 * Copyright (c) 2024 fduncanh
 * All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 */

/* this file is part of raop.c and should not be included in any other file */

#include "airplay_video.h"
#include "fcup_request.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_FCUP_MEDIA_BYTES (128U * 1024U * 1024U)
#define MAX_FCUP_ACTION_BYTES (MAX_FCUP_MEDIA_BYTES + 1024U * 1024U)
#define MAX_SPOOL_DIRECTORY_BYTES 768U
#define SPOOL_PATH_BYTES 1024U
#define SPOOL_WRITE_CHUNK_BYTES (1024U * 1024U)

static void
*hls_get_current_video(raop_t *raop) {
    if (raop->current_video < 0 || raop->current_video >= MAX_AIRPLAY_VIDEO ||
        !raop->airplay_video[raop->current_video]) {
        logger_log(raop->logger, LOGGER_ERR, "hls_get_current_video: failed to identify current_playlist");
        return NULL;
    }
    return (void *) raop->airplay_video[raop->current_video];
}

static int
get_playlist_by_uuid(raop_t *raop, const char *uuid) {
    if (!uuid) return -1;
    for (int i = 0 ;i < MAX_AIRPLAY_VIDEO; i++) {
        const char *stored_uuid = raop->airplay_video[i]
            ? get_playback_uuid(raop->airplay_video[i]) : NULL;
        if (stored_uuid && !strcmp(uuid, stored_uuid)) {
            return i;
        }
    }
    return -1;
}

static bool
is_iso_bmff_media(const char *data, size_t size) {
    return data && size >= 12 && !memcmp(data + 4, "ftyp", 4);
}

static void
log_spool_failure(raop_t *raop, const char *stage, int error_number) {
    if (!error_number) error_number = EIO;
    logger_log(raop->logger, LOGGER_ERR,
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

static char *
spool_reverse_media(raop_t *raop, const char *data, size_t size) {
    if (!data || !size || size > MAX_FCUP_MEDIA_BYTES) {
        log_spool_failure(raop, "validate", EINVAL);
        return NULL;
    }

    char user_directory[MAX_SPOOL_DIRECTORY_BYTES + 1U] = {0};
    const char *candidates[3] = {NULL, NULL, "/private/tmp"};
    size_t candidate_count = 0;
#ifdef _CS_DARWIN_USER_TEMP_DIR
    const size_t needed = confstr(_CS_DARWIN_USER_TEMP_DIR, NULL, 0);
    if (needed > 1 && needed <= sizeof(user_directory) &&
        confstr(_CS_DARWIN_USER_TEMP_DIR, user_directory,
                sizeof(user_directory)) == needed &&
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

    for (size_t candidate = 0; candidate < candidate_count; ++candidate) {
        const char *directory = candidates[candidate];
        if (!safe_spool_directory(directory)) continue;

        char path[SPOOL_PATH_BYTES];
        const int path_length = snprintf(path, sizeof(path), "%s%sobs-airplay-fcup-XXXXXX",
                                         directory,
                                         directory[strlen(directory) - 1] == '/' ? "" : "/");
        if (path_length <= 0 || (size_t) path_length >= sizeof(path)) {
            log_spool_failure(raop, "path", ENAMETOOLONG);
            continue;
        }

        errno = 0;
        int fd = mkstemp(path);
        if (fd < 0) {
            log_spool_failure(raop, "mkstemp", errno);
            continue;
        }
        logger_log(raop->logger, LOGGER_INFO,
                   "[MEDIA] blob spool using validated temp directory %s",
                   directory);

        bool failed = false;
        if (fchmod(fd, S_IRUSR | S_IWUSR) < 0) {
            log_spool_failure(raop, "fchmod", errno);
            failed = true;
        }
        if (!failed) {
            const int descriptor_flags = fcntl(fd, F_GETFD);
            if (descriptor_flags < 0 ||
                fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
                log_spool_failure(raop, "fcntl", errno);
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
                log_spool_failure(raop, "write", result < 0 ? errno : EIO);
                failed = true;
                break;
            }
            written += (size_t) result;
        }

        if (close(fd) < 0) {
            log_spool_failure(raop, "close", errno);
            failed = true;
        }
        if (failed) {
            unlink(path);
            continue;
        }

        errno = 0;
        char *copy = strdup(path);
        if (!copy) {
            log_spool_failure(raop, "strdup", errno ? errno : ENOMEM);
            unlink(path);
            continue;
        }
        return copy;
    }

    return NULL;
}

static void
http_handler_server_info(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                         char **response_data, int *response_datalen)  {

    raop_t *raop = conn->raop;
    assert(raop->dnssd);
    int hw_addr_raw_len = 0;
    const char *hw_addr_raw = dnssd_get_hw_addr(raop->dnssd, &hw_addr_raw_len);

    char *hw_addr = calloc(1, 3 * hw_addr_raw_len);
    utils_hwaddr_airplay(hw_addr, 3 * hw_addr_raw_len, hw_addr_raw, hw_addr_raw_len);

    plist_t r_node = plist_new_dict();

    /* first 12 AirPlay features bits (R to L): 0x27F = 0010 0111 1111
     * Only bits 0-6 and bit 9  are set:
     * 0. video supported
     * 1. photo supported
     * 2. video protected wirh FairPlay DRM
     * 3. volume control supported for video
     * 4. HLS supported
     * 5. slideshow supported
     * 6. (unknown)
     * 9. audio supported.
     */
    plist_t features_node = plist_new_uint(0x27F); 
    plist_dict_set_item(r_node, "features", features_node);

    plist_t mac_address_node = plist_new_string(hw_addr);
    plist_dict_set_item(r_node, "macAddress", mac_address_node);

    plist_t model_node = plist_new_string(GLOBAL_MODEL);
    plist_dict_set_item(r_node, "model", model_node);

    plist_t os_build_node = plist_new_string("12B435");
    plist_dict_set_item(r_node, "osBuildVersion", os_build_node);

    plist_t protovers_node = plist_new_string("1.0");
    plist_dict_set_item(r_node, "protovers", protovers_node);

    plist_t source_version_node = plist_new_string(GLOBAL_VERSION);
    plist_dict_set_item(r_node, "srcvers", source_version_node);

    plist_t vv_node = plist_new_uint(strtol(AIRPLAY_VV, NULL, 10));
    plist_dict_set_item(r_node, "vv", vv_node);

    plist_t device_id_node = plist_new_string(hw_addr);
    plist_dict_set_item(r_node, "deviceid", device_id_node);

    plist_to_xml(r_node, response_data, (uint32_t *) response_datalen);

    //assert(*response_datalen == strlen(*response_data));

    /* last character (at *response_data[response_datalen - 1]) is  0x0a = '\n'
     * (*response_data[response_datalen] is '\0').
     * apsdk removes the last "\n" by overwriting it with '\0', and reducing response_datalen by 1. 
     * TODO: check if this is necessary  */
    
    plist_free(r_node);
    http_response_add_header(response, "Content-Type", "text/x-apple-plist+xml");
    free(hw_addr);
}    

static void
http_handler_scrub(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                   char **response_data, int *response_datalen) {
    raop_t *raop = conn->raop;
    const char *url = http_request_get_url(request);
    const char *data = strstr(url, "?");
    float scrub_position = 0.0f;
    if (data) {
        data++;
        const char *equals = strstr(data, "=");
        if (equals) {
            const char *position = equals + 1;
            char *end = NULL;
            double value = strtod(position, &end);
            if (end && end != position) {
                scrub_position = (float) value;
                logger_log(raop->logger, LOGGER_DEBUG, "http_handler_scrub: got position = %.6f",
                           scrub_position);
            }
        }
    }
    logger_log(raop->logger, LOGGER_DEBUG, "**********************SCRUB %f ***********************",scrub_position);
    raop->callbacks.on_video_scrub(raop->callbacks.cls, scrub_position);
}

static void
http_handler_rate(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                  char **response_data, int *response_datalen) {

    raop_t *raop = conn->raop;
    const char *url = http_request_get_url(request);
    const char *data = strstr(url, "?");
    float rate_value = 0.0f;
    if (data) {
        data++;
        const char *equals = strstr(data, "=");
        if (equals) {
            const char *rate = equals + 1;
            char *end = NULL;
            float value = strtof(rate, &end);
            if (end && end != rate) {
                rate_value =  value;
                logger_log(raop->logger, LOGGER_DEBUG, "http_handler_rate: got rate = %.6f", rate_value);
            }
        }
    }
    raop->callbacks.on_video_rate(raop->callbacks.cls, rate_value);
}

static void
http_handler_stop(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                  char **response_data, int *response_datalen) {

    raop_t *raop = conn->raop;
    logger_log(raop->logger, LOGGER_INFO, "client HTTP request POST stop");

    raop->callbacks.on_video_stop(raop->callbacks.cls);
}

/* handles PUT /setProperty http requests from Client to Server */

static void
http_handler_set_property(raop_conn_t *conn,
                          http_request_t *request, http_response_t *response,
                          char **response_data, int *response_datalen) {

    raop_t *raop = conn->raop;
    const char *url = http_request_get_url(request);
    const char *property = url + strlen("/setProperty?");
    logger_log(raop->logger, LOGGER_DEBUG, "http_handler_set_property: %s", property);


    /*  actionAtItemEnd:  values:  
                  0: advance (advance to next item, if there is one)
                  1: pause   (pause playing)
                  2: none    (do nothing)             

        reverseEndTime   (only used when rate < 0) time at which reverse playback ends
        forwardEndTime   (only used when rate > 0) time at which reverse playback ends
        selectedMediaArray contains plist with language choice:
    */

    airplay_video_t *airplay_video = (airplay_video_t *) hls_get_current_video(raop);
    if (!strcmp(property, "selectedMediaArray")) {
        if (!airplay_video) {
            logger_log(raop->logger, LOGGER_WARNING,
                       "ignoring selectedMediaArray before a valid POST /play");
            goto post_error;
        }
        /* verify that this request contains a binary plist*/
        char *header_str = NULL;
        int request_datalen = 0;
        http_request_get_header_string(request, &header_str);
        bool is_plist = header_str && strstr(header_str,"apple-binary-plist");
        free(header_str);
        if (!is_plist) {
            logger_log(raop->logger, LOGGER_DEBUG, "POST /setProperty?selectedMediaArray"
                       "does not provide an apple-binary-plist");
            goto post_error;
        }

        const char *request_data = http_request_get_data(request, &request_datalen);
        plist_t req_root_node = NULL;
        if (!request_data || request_datalen <= 0) {
            goto post_error;
        }
        plist_from_bin(request_data, request_datalen, &req_root_node);
        if (!req_root_node || !PLIST_IS_DICT(req_root_node)) {
            if (req_root_node) plist_free(req_root_node);
            goto post_error;
        }
        plist_t req_value_node = plist_dict_get_item(req_root_node, "value");

        if (!req_value_node || !PLIST_IS_ARRAY(req_value_node)) {	  
            logger_log(raop->logger, LOGGER_INFO, "POST /setProperty?selectedMediaArray"
                   " did not provide expected plist from client");
            plist_free(req_root_node);
            goto post_error;
        }

        int count = plist_array_get_size(req_value_node);
        char *name = NULL;
        char *code = NULL;
        char *language_name = NULL;
        char *language_code = NULL;
        for (int i = 0; i < count; i++) {
            plist_t req_value_array_node = plist_array_get_item(req_value_node,i);
            if (!language_name) {
                plist_t req_value_options_name_node =  plist_dict_get_item(req_value_array_node,"MediaSelectionOptionsName");
                if (PLIST_IS_STRING(req_value_options_name_node)) {
                    plist_get_string_val(req_value_options_name_node, &name);
                    if (name) {
                        language_name = (char *) calloc(strlen(name) + 1, sizeof(char));
                        if (!language_name) {
                            printf("Memory allocation failed\n");
                            exit(1);
                        }
                        memcpy(language_name, name, strlen(name));
                        plist_mem_free(name);
                    }
                }
            }
            if (!language_code) {
                plist_t req_value_options_code_node =  plist_dict_get_item(req_value_array_node,"MediaSelectionOptionsUnicodeLanguageIdentifier");
                if (PLIST_IS_STRING(req_value_options_code_node)) {
                    plist_get_string_val(req_value_options_code_node, &code);
                    if (code) {
                        language_code = (char *) calloc(strlen(code) + 1, sizeof(char));
                        if (!language_code) {
                            printf("Memory allocation failed\n");
                            exit(1);
                        }
                        memcpy(language_code, code, strlen(code));
                        plist_mem_free(code);
                    }
                }
            }
            if (language_code && language_name) {
                break;
            } else {
                continue;
            }
        }
        plist_free (req_root_node);
        if (language_code && language_name) {
            set_language_code(airplay_video, language_code, strlen(language_code));
            set_language_name(airplay_video, language_name, strlen(language_name));
            logger_log(raop->logger, LOGGER_INFO, "stored language from MediaSelectionOptions: %s \"%s\"",
                       get_language_code(airplay_video), get_language_name(airplay_video));
        }
        plist_mem_free(language_name);
        plist_mem_free(language_code);
    } else if (!strcmp(property, "reverseEndTime") ||
        !strcmp(property, "forwardEndTime") ||
        !strcmp(property, "actionAtItemEnd")) {
        logger_log(raop->logger, LOGGER_DEBUG, "property %s is known but unhandled", property);

        plist_t errResponse = plist_new_dict();
        plist_t errCode = plist_new_uint(0);
        plist_dict_set_item(errResponse, "errorCode", errCode);
        plist_to_xml(errResponse, response_data, (uint32_t *) response_datalen);
        plist_free(errResponse);
        http_response_add_header(response, "Content-Type", "text/x-apple-plist+xml");
    } else {
        logger_log(raop->logger, LOGGER_DEBUG, "property %s is unknown, unhandled", property);      
        goto post_error;
    }
    return;
 post_error:
    http_response_add_header(response, "Content-Length", "0");
}

/* handles GET /getProperty http requests from Client to Server.  (not implemented) */

static void
http_handler_get_property(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                          char **response_data, int *response_datalen) {
    raop_t *raop = conn->raop;
    const char *url = http_request_get_url(request);
    const char *property = url + strlen("getProperty?");
    logger_log(raop->logger, LOGGER_DEBUG, "http_handler_get_property: %s (unhandled)", property);
}

/* this request (for a variant FairPlay decryption)  cannot be handled  by UxPlay */
static void
http_handler_fpsetup2(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                      char **response_data, int *response_datalen) {
    raop_t *raop = conn->raop;
    logger_log(raop->logger, LOGGER_WARNING, "client HTTP request POST fp-setup2 is unhandled");
    http_response_add_header(response, "Content-Type", "application/x-apple-binary-plist");
    int req_datalen = 0;
    const unsigned char *req_data = (unsigned char *) http_request_get_data(request, &req_datalen);
    logger_log(raop->logger, LOGGER_ERR, "only FairPlay version 0x03 is implemented, version is 0x%2.2x",
               req_data[4]);
    http_response_init(response, "HTTP/1.1", 421, "Misdirected Request");
}

// called by http_handler_playback_info while preparing response to a GET /playback_info request from the client.

typedef struct time_range_s {
    double start;
    double duration;
} time_range_t;

void time_range_to_plist(void *time_ranges, const int n_time_ranges,
		         plist_t time_ranges_node) {
    time_range_t *tr = (time_range_t *) time_ranges;
    for (int i = 0 ; i < n_time_ranges; i++) {
        plist_t time_range_node = plist_new_dict();
        plist_t duration_node = plist_new_real(tr[i].duration);
        plist_dict_set_item(time_range_node, "duration", duration_node);
        plist_t start_node = plist_new_real(tr[i].start);
        plist_dict_set_item(time_range_node, "start", start_node);
        plist_array_append_item(time_ranges_node, time_range_node);
    }
}

// called by http_handler_playback_info while preparing response to a GET /playback_info request from the client.

int create_playback_info_plist_xml(playback_info_t *playback_info, char **plist_xml) {

    plist_t res_root_node = plist_new_dict();

    plist_t duration_node = plist_new_real(playback_info->duration);
    plist_dict_set_item(res_root_node, "duration", duration_node);

    plist_t position_node = plist_new_real(playback_info->position);
    plist_dict_set_item(res_root_node, "position", position_node);

    plist_t rate_node = plist_new_real(playback_info->rate);
    plist_dict_set_item(res_root_node, "rate", rate_node);

    /* should these be int or bool? */
    plist_t ready_to_play_node = plist_new_uint(playback_info->ready_to_play);
    plist_dict_set_item(res_root_node, "readyToPlay", ready_to_play_node);

    plist_t playback_buffer_empty_node = plist_new_uint(playback_info->playback_buffer_empty);
    plist_dict_set_item(res_root_node, "playbackBufferEmpty", playback_buffer_empty_node);

    plist_t playback_buffer_full_node = plist_new_uint(playback_info->playback_buffer_full);
    plist_dict_set_item(res_root_node, "playbackBufferFull", playback_buffer_full_node);

    plist_t playback_likely_to_keep_up_node = plist_new_uint(playback_info->playback_likely_to_keep_up);
    plist_dict_set_item(res_root_node, "playbackLikelyToKeepUp", playback_likely_to_keep_up_node);

    plist_t loaded_time_ranges_node = plist_new_array();
    time_range_to_plist(playback_info->loadedTimeRanges, playback_info->num_loaded_time_ranges,
                        loaded_time_ranges_node);
    plist_dict_set_item(res_root_node, "loadedTimeRanges", loaded_time_ranges_node);

    plist_t seekable_time_ranges_node = plist_new_array();
    time_range_to_plist(playback_info->seekableTimeRanges, playback_info->num_seekable_time_ranges,
                        seekable_time_ranges_node);
    plist_dict_set_item(res_root_node, "seekableTimeRanges", seekable_time_ranges_node);

    int len = 0;
    plist_to_xml(res_root_node, plist_xml, (uint32_t *) &len);
    /* plist_xml is null-terminated, last character is '/n' */

    plist_free(res_root_node);

    return len;
}

/* this handles requests from the Client  for "Playback information" while the Media is playing on the 
   Media Player.  (The Server gets this information by monitoring the Media Player). The Client could use 
   the information to e.g. update  the slider it shows with progress to the player (0%-100%). 
   It does not affect playing of the Media*/

static void
http_handler_playback_info(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                           char **response_data, int *response_datalen)
{
    raop_t *raop = conn->raop;
    //const char *session_id = http_request_get_header(request, "X-Apple-Session-ID");
    playback_info_t playback_info;

    playback_info.stallcount = 0;
    //playback_info.playback_buffer_empty = false;   // maybe  need to get this from playbin 
    //playback_info.playback_buffer_full = true;
    //ayback_info.ready_to_play = true; // ???;
    //ayback_info.playback_likely_to_keep_up = true;

    raop->callbacks.on_video_acquire_playback_info(raop->callbacks.cls, &playback_info);
    if (playback_info.duration == -1.0) {
        /* video has finished, reset */
        logger_log(raop->logger, LOGGER_DEBUG, "playback_info not available (finishing)");
        //httpd_remove_known_connections(raop->httpd);
        http_response_set_disconnect(response,1);
        raop->callbacks.video_reset(raop->callbacks.cls, RESET_TYPE_HLS_SHUTDOWN);
        return;
    } else if (playback_info.position == -1.0) {
        logger_log(raop->logger, LOGGER_DEBUG, "playback_info not available");
        return;
    }      

    playback_info.num_loaded_time_ranges =
        playback_info.duration > playback_info.position ? 1 : 0;
    time_range_t time_ranges_loaded[1];
    time_ranges_loaded[0].start = playback_info.position;
    time_ranges_loaded[0].duration = playback_info.num_loaded_time_ranges
        ? playback_info.duration - playback_info.position : 0.0;
    playback_info.loadedTimeRanges = playback_info.num_loaded_time_ranges
        ? (void *) &time_ranges_loaded : NULL;

    playback_info.num_seekable_time_ranges = playback_info.seek_duration > 0.0 ? 1 : 0;
    time_range_t time_ranges_seekable[1];
    time_ranges_seekable[0].start = playback_info.seek_start;
    time_ranges_seekable[0].duration = playback_info.seek_duration;
    playback_info.seekableTimeRanges = playback_info.num_seekable_time_ranges
        ? (void *) &time_ranges_seekable : NULL;

    *response_datalen =  create_playback_info_plist_xml(&playback_info, response_data);
    http_response_add_header(response, "Content-Type", "text/x-apple-plist+xml");
}

/* this handles the POST /reverse request from Client to Server on a AirPlay http channel to "Upgrade" 
   to "PTTH/1.0" Reverse HTTP protocol proposed in 2009 Internet-Draft 

          https://datatracker.ietf.org/doc/id/draft-lentczner-rhttp-00.txt .  

   After the Upgrade the channel becomes a reverse http "AirPlay (reversed)" channel for
   http requests from Server to Client.
  */

static void
http_handler_reverse(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                     char **response_data, int *response_datalen) {

    raop_t *raop = conn->raop;
    /* get http socket for send */
    int socket_fd = httpd_get_connection_socket (raop->httpd, (void *) conn);
    if (socket_fd < 0) {
        logger_log(raop->logger, LOGGER_ERR, "fcup_request failed to retrieve socket_fd from httpd");
        /* shut down connection? */
    }
    
    const char *purpose = http_request_get_header(request, "X-Apple-Purpose");
    const char *connection = http_request_get_header(request, "Connection");
    const char *upgrade = http_request_get_header(request, "Upgrade");
    logger_log(raop->logger, LOGGER_INFO,
               "[MEDIA] POST /reverse: connection=%s upgrade=%s purpose=%s",
               connection ? connection : "(missing)",
               upgrade ? upgrade : "(missing)", purpose ? purpose : "(missing)");

    httpd_set_connection_type(raop->httpd, (void *) conn, CONNECTION_TYPE_PTTH);
    int type_PTTH = httpd_count_connection_type(raop->httpd, CONNECTION_TYPE_PTTH);

    if (type_PTTH == 1) {
        logger_log(raop->logger, LOGGER_DEBUG, "will use socket %d for %s connections", socket_fd, purpose);
        http_response_init(response, "HTTP/1.1", 101, "Switching Protocols");
        http_response_add_header(response, "Connection", "Upgrade");
        http_response_add_header(response, "Upgrade", "PTTH/1.0");
    } else {
        logger_log(raop->logger, LOGGER_ERR, "multiple TPPH connections (%d) are forbidden", type_PTTH );
    }    
}

/* POST /action returns the payload requested by a preceding FCUP request on
   the reverse channel. Historically this was limited to HLS playlists. Safari
   can also advertise a page-local blob URL, for which we experimentally ask
   the sender to return the bounded ISO-BMFF media object itself. */

static void
http_handler_action(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                    char **response_data, int *response_datalen) {

    raop_t *raop = conn->raop;
    airplay_video_t *airplay_video = (airplay_video_t *) hls_get_current_video(raop);
    assert(airplay_video);
    bool data_is_plist = false;
    plist_t req_root_node = NULL;
    uint64_t uint_val = 0;
    int request_id = 0;
    int fcup_response_statuscode = 0;
    bool fcup_response_status_present = false;
    bool logger_debug = (logger_get_level(raop->logger) >= LOGGER_DEBUG);
    /* Every error path reaches post_action_error, including paths above plist
       parsing. Keep cleanup-owned locals initialized before the first goto. */
    char *type = NULL;

    const char* session_id = http_request_get_header(request, "X-Apple-Session-ID");
    if (!session_id) {
        logger_log(raop->logger, LOGGER_ERR, "Play request had no X-Apple-Session-ID");
        goto post_action_error;
    }    
    if (!airplay_video) {
        logger_log(raop->logger, LOGGER_ERR,
                   "[MEDIA] POST /action arrived before a valid POST /play");
        goto post_action_error;
    }
    const char *apple_session_id = get_apple_session_id(airplay_video);
    if (!apple_session_id) {
        goto post_action_error;
    }
    if (strcmp(session_id, apple_session_id)){
        logger_log(raop->logger, LOGGER_ERR, "X-Apple-Session-ID has changed:\n  was:\"%s\"\n  now:\"%s\"",
                   apple_session_id, session_id);
        goto post_action_error;
    }

    /* verify that this request contains a binary plist*/
    char *header_str = NULL;
    http_request_get_header_string(request, &header_str);
    if (header_str) {
        logger_log(raop->logger, LOGGER_DEBUG, "request header: %s", header_str);
        data_is_plist = (strstr(header_str,"apple-binary-plist") != NULL);
    }
    free(header_str);
    if (!data_is_plist) {
        logger_log(raop->logger, LOGGER_INFO, "POST /action: did not receive expected plist from client");	
        goto post_action_error;
    }

    /* extract the root_node  plist */
    int request_datalen = 0;
    const char *request_data = http_request_get_data(request, &request_datalen);
    if (request_datalen <= 0 ||
        (uint64_t) request_datalen > (uint64_t) MAX_FCUP_ACTION_BYTES) {
        if (request_datalen > 0) {
            logger_log(raop->logger, LOGGER_WARNING,
                       "[MEDIA] POST /action body exceeds the %u MiB limit",
                       MAX_FCUP_ACTION_BYTES / (1024U * 1024U));
        }
        logger_log(raop->logger, LOGGER_INFO, "POST /action: did not receive expected plist from client");	
        goto post_action_error;
    }
    plist_from_bin(request_data, request_datalen, &req_root_node);
    if (!req_root_node || !PLIST_IS_DICT(req_root_node)) {
        goto post_action_error;
    }

    /* determine type of data */
    plist_t req_type_node = plist_dict_get_item(req_root_node, "type");
    if (!PLIST_IS_STRING(req_type_node)) {
        goto post_action_error;
    }
    
    /* three possible types are known: 
       playlistRemove
       playlistAdd
       unhandledURLRespone
*/
    plist_get_string_val(req_type_node, &type);
    if (!type) {
        goto post_action_error;
    }
    logger_log(raop->logger, LOGGER_DEBUG, "action type is %s", type);
    /* check that plist structure is as expected*/
    plist_t req_params_node = NULL;
    if (PLIST_IS_DICT (req_root_node)) {
        req_params_node = plist_dict_get_item(req_root_node, "params");
    }
    if (!PLIST_IS_DICT (req_params_node)) {
        goto post_action_error;
    }
    if (!strcmp(type,"playlistRemove")) {
        plist_t req_params_item_node = plist_dict_get_item(req_params_node, "item");
        if (!req_params_item_node || !PLIST_IS_DICT (req_params_item_node)) {
            goto post_action_error;
        }
        plist_t req_params_item_uuid_node = plist_dict_get_item(req_params_item_node, "uuid");
        char* remove_uuid = NULL;
        if (PLIST_IS_STRING(req_params_item_uuid_node)) {
            plist_get_string_val(req_params_item_uuid_node, &remove_uuid);
        }
        if (!remove_uuid) {
            goto post_action_error;
        }
        int id  =  get_playlist_by_uuid(raop, remove_uuid);
        if (id == raop->current_video) {
            raop->current_video = -1;
            float position = raop->callbacks.on_video_playlist_remove(raop->callbacks.cls);
            /* keep the playlist (until space is needed for another one) in case its playback_uuid is re-requested
               the video will then be restarted at its previous position */
            set_resume_position_seconds(airplay_video, position);
        } else {
            logger_log(raop->logger, LOGGER_WARNING, "playlistRemove uuid %s does not match current_video\n", remove_uuid);
        }
        plist_mem_free (remove_uuid);

    } else if (!strcmp(type, "playlistInsert")) {
        logger_log(raop->logger, LOGGER_INFO, "action type playlistInsert (start playback)");
        plist_t req_params_item_node = plist_dict_get_item(req_params_node, "item");
        if (!req_params_item_node || !PLIST_IS_DICT (req_params_item_node)) {
            goto post_action_error;
        }
        plist_t req_params_item_uuid_node = plist_dict_get_item(req_params_item_node, "uuid");
        char* remove_uuid = NULL;
        if (PLIST_IS_STRING(req_params_item_uuid_node)) {
            plist_get_string_val(req_params_item_uuid_node, &remove_uuid);
        }
        if (remove_uuid) {
            int id  =  get_playlist_by_uuid(raop, remove_uuid);
            if (id >= 0) {
                logger_log(raop->logger, LOGGER_INFO, "playlistInsert uuid %s is stored at airplay_video[%d]", remove_uuid, id);
            } else {
                logger_log(raop->logger, LOGGER_INFO, "playlistInsert uuid %s is not a stored playlist", remove_uuid);
            }
            plist_mem_free(remove_uuid);
            char *plist_xml = NULL;
            uint32_t plist_len = 0;
            plist_to_xml(req_params_item_node, &plist_xml, &plist_len);
            printf("playlistInsert parameter item list is:\n%s", plist_xml);
            plist_mem_free(plist_xml);
        }
        logger_log(raop->logger, LOGGER_ERR, "FIXME: playlistInsert is not yet implemented");

    } else if (!strcmp(type, "unhandledURLResponse")) {   
        /* handling type "unhandledURLResponse" */
        uint_val = 0;
        int fcup_response_datalen = 0;

        plist_t req_params_fcup_response_statuscode_node = plist_dict_get_item(req_params_node,
                                                                  "FCUP_Response_StatusCode");
        if (req_params_fcup_response_statuscode_node) {
            plist_get_uint_val(req_params_fcup_response_statuscode_node, &uint_val);
            fcup_response_statuscode = (int) uint_val;
            fcup_response_status_present = true;
            uint_val = 0;
        }
        logger_log(raop->logger, LOGGER_INFO,
                   "[MEDIA] reverse fetch response status_present=%d status=%d",
                   fcup_response_status_present, fcup_response_statuscode);

        if  (logger_debug) {
            plist_t req_params_fcup_response_requestid_node = plist_dict_get_item(req_params_node,
                                                                     "FCUP_Response_RequestID");
            if (req_params_fcup_response_requestid_node) {
                plist_get_uint_val(req_params_fcup_response_requestid_node, &uint_val);
                request_id = (int) uint_val;
                uint_val = 0;
                logger_log(raop->logger, LOGGER_DEBUG, "FCUP_Response_RequestID =  %d", request_id);
            }
        }

        plist_t req_params_fcup_response_url_node = plist_dict_get_item(req_params_node, "FCUP_Response_URL");
        if (!PLIST_IS_STRING(req_params_fcup_response_url_node)) {
            goto post_action_error;
        }
        char *fcup_response_url = NULL;
        plist_get_string_val(req_params_fcup_response_url_node, &fcup_response_url);
        if (!fcup_response_url) {
            goto post_action_error;
        }

        plist_t req_params_fcup_response_data_node = plist_dict_get_item(req_params_node, "FCUP_Response_Data");
        if (!PLIST_IS_DATA(req_params_fcup_response_data_node)){
            plist_mem_free(fcup_response_url);
            goto post_action_error;
        }

        uint_val = 0;
#ifdef PLIST_210
        const char *fcup_response_data = NULL;
        fcup_response_data = plist_get_data_ptr(req_params_fcup_response_data_node, &uint_val);
#else
        char *fcup_response_data = NULL;       
        fcup_response_data = plist_get_data_val(req_params_fcup_response_data_node, &fcup_response_data, &uint_val);
#endif
        fcup_response_datalen = (int) uint_val;
        logger_log(raop->logger, LOGGER_INFO, "[MEDIA] received reverse-fetched payload (%d bytes)",
                   fcup_response_datalen);
        if (!fcup_response_data) {
            plist_mem_free(fcup_response_url);
            goto post_action_error;
        }

        if (get_reverse_media_pending(airplay_video)) {
            const char *requested_url = get_playback_location(airplay_video);
            const bool url_matches = requested_url &&
                !strcmp(requested_url, fcup_response_url);
            /* Safari uses 0 when no HTTP status applies to a page-local blob. */
            const bool status_ok = !fcup_response_status_present ||
                fcup_response_statuscode == 0 ||
                (fcup_response_statuscode >= 200 && fcup_response_statuscode < 300);
            const bool size_ok = fcup_response_datalen > 0 &&
                (uint64_t) fcup_response_datalen <= (uint64_t) MAX_FCUP_MEDIA_BYTES;
            const bool format_ok = size_ok &&
                is_iso_bmff_media(fcup_response_data, (size_t) fcup_response_datalen);

            char *spooled_path = NULL;
            const bool spool_attempted = url_matches && status_ok && format_ok;
            if (spool_attempted) {
                spooled_path = spool_reverse_media(raop, fcup_response_data,
                                                   (size_t) fcup_response_datalen);
            }
#ifndef PLIST_210
            plist_mem_free(fcup_response_data);
#endif

            if (!url_matches || !status_ok || !size_ok || !format_ok || !spooled_path) {
                logger_log(raop->logger, LOGGER_ERR,
                           "[MEDIA] reverse blob fetch unusable "
                           "(url_match=%d status_present=%d status=%d status_ok=%d "
                           "bytes=%d iso_bmff=%d spool_attempted=%d spool=%d)",
                           url_matches, fcup_response_status_present,
                           fcup_response_statuscode, status_ok, fcup_response_datalen,
                           format_ok, spool_attempted, spooled_path != NULL);
                free(spooled_path);
                plist_mem_free(fcup_response_url);
                raop->callbacks.on_video_stop(raop->callbacks.cls);
                const int failed_id = raop->current_video;
                if (failed_id >= 0) raop_destroy_airplay_video(raop, failed_id);
                goto post_action_error;
            }

            if (!set_spooled_media_path(airplay_video, spooled_path,
                                        strlen(spooled_path))) {
                logger_log(raop->logger, LOGGER_ERR,
                           "[MEDIA] could not retain reverse-fetched media path");
                unlink(spooled_path);
                free(spooled_path);
                plist_mem_free(fcup_response_url);
                raop->callbacks.on_video_stop(raop->callbacks.cls);
                const int failed_id = raop->current_video;
                if (failed_id >= 0) raop_destroy_airplay_video(raop, failed_id);
                goto post_action_error;
            }
            set_playback_location(airplay_video, spooled_path, strlen(spooled_path));
            set_reverse_media_pending(airplay_video, false);
            logger_log(raop->logger, LOGGER_INFO,
                       "[MEDIA] reverse-fetched blob spooled securely (%d bytes); starting playback",
                       fcup_response_datalen);
            raop->callbacks.on_video_play(raop->callbacks.cls,
                                          get_playback_location(airplay_video),
                                          get_start_position_seconds(airplay_video));
            free(spooled_path);
            plist_mem_free(fcup_response_url);
            goto post_action_success;
        }

        char *playlist = (char *) malloc(fcup_response_datalen + 1);
        if (!playlist) {
#ifndef PLIST_210
            plist_mem_free(fcup_response_data);
#endif
            plist_mem_free(fcup_response_url);
            goto post_action_error;
        }
        playlist[fcup_response_datalen] = '\0';
        memcpy(playlist, fcup_response_data, fcup_response_datalen);
#ifndef PLIST_210
        plist_mem_free(fcup_response_data);
#endif
        int playlist_len = strlen(playlist);
    
        if (logger_debug) {
            logger_log(raop->logger, LOGGER_DEBUG, "begin FCUP Response data:\n%s\nend FCUP Response data", playlist);
        }

        char *ptr = strstr(fcup_response_url, "/master.m3u8");
        if (ptr) {
            /* this is a master playlist */
            const char *uri_prefix = get_uri_prefix(airplay_video);
            char ** uri_list = NULL;
            int num_uri = 0;
            char *uri_local_prefix = get_uri_local_prefix(airplay_video);
            playlist = select_master_playlist_language(airplay_video, playlist);
            playlist_len = strlen(playlist);
            create_media_uri_table(uri_prefix, playlist, playlist_len, &uri_list, &num_uri);	
            char *new_master = adjust_master_playlist (playlist, playlist_len,  uri_prefix, uri_local_prefix);
            free(playlist);
            store_master_playlist(airplay_video, new_master);
            create_media_data_store(airplay_video, uri_list, num_uri);
            free (uri_list);
            num_uri =  get_num_media_uri(airplay_video);
            set_next_media_uri_id(airplay_video, 0);
        } else {
            /* this is a media playlist */
            float duration = 0.0f;
            bool endlist = false;
            int count = analyze_media_playlist(playlist, &duration, &endlist);
            int uri_num = get_next_media_uri_id(airplay_video);
            --uri_num;    // (next num is current num + 1)
            int ret = store_media_playlist(airplay_video, playlist, &count, &duration, &endlist, uri_num);
            if (ret == 1) {
                logger_log(raop->logger, LOGGER_DEBUG,"media_playlist is a duplicate: do not store");
            } else if (count) {
                logger_log(raop->logger, LOGGER_DEBUG,
                           "\n%s:\nreceived media playlist has %5d chunks, total duration %9.3f secs\n",
                            fcup_response_url, count, duration);
            }
        }

        plist_mem_free(fcup_response_url);

        int num_uri = get_num_media_uri(airplay_video);
        int uri_num = get_next_media_uri_id(airplay_video);
        if (uri_num <  num_uri) {
            fcup_request((void *) conn, get_media_uri_by_num(airplay_video, uri_num),
                                                             apple_session_id,
                                                             get_next_FCUP_RequestID(airplay_video));
            set_next_media_uri_id(airplay_video, ++uri_num);
        } else {
            raop->callbacks.on_video_play(raop->callbacks.cls,
                                                get_playback_location(airplay_video),
                                                get_start_position_seconds(airplay_video));
        }


    } else {
        logger_log(raop->logger, LOGGER_INFO, "unknown action type (unhandled)"); 
    }
 post_action_success:;
    if (type) plist_mem_free(type);
    plist_free(req_root_node);
    return;

 post_action_error:;
    http_response_init(response, "HTTP/1.1", 400, "Bad Request");
    if (type) plist_mem_free(type);
    if (req_root_node)  {
        plist_free(req_root_node);
    }

}

/* The POST /play request from the Client to Server on the AirPlay http channel contains (among other information)
   the "Content Location" that specifies the HLS Playlists for the video to be streamed, as well as the video 
   "start position in seconds".   Once this request is received by the Sever, the Server sends a POST /event
   "FCUP Request" request to the Client on the reverse http channel, to request the HLS Master Playlist */

static bool
media_location_is_http(const char *location) {
    return location && (!strncasecmp(location, "http://", 7) ||
                        !strncasecmp(location, "https://", 8));
}

static bool
media_location_is_blob(const char *location) {
    return location && !strncasecmp(location, "blob:", 5);
}

static void
media_location_scheme(const char *location, char *scheme, size_t scheme_size) {
    if (!scheme || scheme_size == 0) return;
    const char *fallback = location && location[0] ? "unknown" : "missing";
    snprintf(scheme, scheme_size, "%s", fallback);
    if (!location) return;
    const char *colon = strchr(location, ':');
    if (!colon || colon == location) return;
    const size_t length = (size_t) (colon - location);
    if (length >= scheme_size) return;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char) location[i];
        if (!(isalnum(c) || c == '+' || c == '-' || c == '.')) return;
        scheme[i] = (char) tolower(c);
    }
    scheme[length] = '\0';
}

static bool
header_value_contains(const char *value, const char *needle) {
    if (!value || !needle || !needle[0]) return false;
    const size_t needle_len = strlen(needle);
    for (const char *cursor = value; *cursor; ++cursor) {
        if (!strncasecmp(cursor, needle, needle_len)) return true;
    }
    return false;
}

static char *
play_text_parameter(const char *data, int datalen, const char *name) {
    if (!data || datalen <= 0 || !name) {
        return NULL;
    }
    const size_t name_len = strlen(name);
    const char *cursor = data;
    const char *limit = data + datalen;
    while (cursor < limit) {
        const char *line_end = memchr(cursor, '\n', (size_t) (limit - cursor));
        if (!line_end) line_end = limit;
        const char *colon = memchr(cursor, ':', (size_t) (line_end - cursor));
        if (colon) {
            const char *key_end = colon;
            while (key_end > cursor && (key_end[-1] == ' ' || key_end[-1] == '\t')) --key_end;
            if ((size_t) (key_end - cursor) == name_len &&
                !strncasecmp(cursor, name, name_len)) {
                const char *value = colon + 1;
                while (value < line_end && (*value == ' ' || *value == '\t')) ++value;
                const char *value_end = line_end;
                while (value_end > value &&
                       (value_end[-1] == '\r' || value_end[-1] == ' ' || value_end[-1] == '\t')) {
                    --value_end;
                }
                const size_t value_len = (size_t) (value_end - value);
                char *copy = calloc(value_len + 1, 1);
                if (copy) memcpy(copy, value, value_len);
                return copy;
            }
        }
        cursor = line_end < limit ? line_end + 1 : limit;
    }
    return NULL;
}

static void
http_handler_play(raop_conn_t *conn, http_request_t *request, http_response_t *response,
                      char **response_data, int *response_datalen) {
    raop_t *raop = conn->raop;
    char* playback_location = NULL;
    char* client_proc_name = NULL;
    plist_t req_root_node = NULL;
    float start_position_seconds = 0.0f;
    bool data_is_binary_plist = false;
    airplay_video_t *airplay_video = NULL;
    int id = -1;
    bool created_airplay_video = false;
    bool playback_stopped = false;
    
    logger_log(raop->logger, LOGGER_DEBUG, "http_handler_play");

    int request_datalen = -1;
    const char *request_data = http_request_get_data(request, &request_datalen);
    const char *content_type = http_request_get_header(request, "Content-Type");
    const char *apple_session_id = http_request_get_header(request, "X-Apple-Session-ID");
    const bool data_is_text_parameters =
        header_value_contains(content_type, "text/parameters");

    logger_log(raop->logger, LOGGER_INFO,
               "[MEDIA] POST /play received (session=%s, body=%d bytes)",
               apple_session_id ? apple_session_id : "(missing)", request_datalen);

    if (data_is_text_parameters) {
        playback_location = play_text_parameter(request_data, request_datalen,
                                                "Content-Location");
        if (!playback_location) {
            const char *header_location = http_request_get_header(request, "Content-Location");
            if (header_location) playback_location = strdup(header_location);
        }
        char *start_value = play_text_parameter(request_data, request_datalen,
                                                "Start-Position-Seconds");
        if (!start_value) {
            start_value = play_text_parameter(request_data, request_datalen,
                                              "Start-Position");
        }
        if (!start_value) {
            const char *header_start = http_request_get_header(request, "Start-Position");
            if (header_start) start_value = strdup(header_start);
        }
        if (start_value) {
            char *end = NULL;
            const float parsed = strtof(start_value, &end);
            if (end && end != start_value) start_position_seconds = parsed;
            free(start_value);
        }
        if (!media_location_is_http(playback_location)) {
            char scheme[16];
            media_location_scheme(playback_location, scheme, sizeof(scheme));
            logger_log(raop->logger, LOGGER_ERR,
                       "[MEDIA] classic POST /play rejected Content-Location "
                       "scheme=%s length=%zu (only HTTP(S) is supported)",
                       scheme, playback_location ? strlen(playback_location) : 0);
            goto play_error;
        }
        logger_log(raop->logger, LOGGER_INFO,
                   "[MEDIA] parsed classic text/parameters play request (start=%.3fs)",
                   start_position_seconds);
        raop->callbacks.on_video_play(raop->callbacks.cls, playback_location,
                                      start_position_seconds);
        free(playback_location);
        return;
    }

    if (!apple_session_id) {
        logger_log(raop->logger, LOGGER_ERR, "Play request had no X-Apple-Session-ID");
        goto play_error;
    }

    if (request_datalen > 0) {
        data_is_binary_plist =
            header_value_contains(content_type, "x-apple-binary-plist");
    }

    if (!data_is_binary_plist) {
         logger_log(raop->logger, LOGGER_ERR, "Play request Content is not binary_plist (unsupported)");
         goto play_error;
    }

    plist_from_bin(request_data, request_datalen, &req_root_node);
    if (!req_root_node || !PLIST_IS_DICT(req_root_node)) {
        goto play_error;
    }

    plist_t req_uuid_node = plist_dict_get_item(req_root_node, "uuid");
    if (!req_uuid_node) {
        plist_t location_node = plist_dict_get_item(req_root_node, "Content-Location");
        if (location_node) plist_get_string_val(location_node, &playback_location);
        plist_t start_node = plist_dict_get_item(req_root_node, "Start-Position-Seconds");
        if (start_node) {
            double start = 0.0;
            plist_get_real_val(start_node, &start);
            start_position_seconds = (float) start;
        }
        if (!media_location_is_http(playback_location)) {
            char scheme[16];
            media_location_scheme(playback_location, scheme, sizeof(scheme));
            logger_log(raop->logger, LOGGER_ERR,
                       "[MEDIA] stateless POST /play rejected Content-Location "
                       "scheme=%s length=%zu (only HTTP(S) is supported)",
                       scheme, playback_location ? strlen(playback_location) : 0);
            goto play_error;
        }
        logger_log(raop->logger, LOGGER_INFO,
                   "[MEDIA] binary POST /play has no uuid; using stateless URL playback");
        raop->callbacks.on_video_play(raop->callbacks.cls, playback_location,
                                      start_position_seconds);
        plist_mem_free(playback_location);
        plist_free(req_root_node);
        return;
    }
    char* playback_uuid = NULL;
    plist_get_string_val(req_uuid_node, &playback_uuid);
    if (!playback_uuid) {
        logger_log(raop->logger, LOGGER_ERR,
                   "[MEDIA] binary POST /play supplied an invalid uuid");
        goto play_error;
    }

#if 0
    for (int i = 0; i < MAX_AIRPLAY_VIDEO; i++) {
        printf("old: airplay_video[%d] %p %s %f\n", i, raop->airplay_video[i],
	       get_playback_uuid(raop->airplay_video[i]),
	       get_duration(raop->airplay_video[i]));
    }
    printf("\n");
    printf("new playback_uuid %s\n\n", playback_uuid);
#endif

    id = get_playlist_by_uuid(raop, playback_uuid);

    /* check if playlist is already downloaded and stored (may have been interrupted by advertisements ) */
    if (id >= 0) {
      //printf("====use EXISTING  airplay_video[%d] %p %s %s\n", id, raop->airplay_video[id], playback_uuid, get_playback_uuid(raop->airplay_video[id]));
        raop->current_video = id;
        airplay_video = hls_get_current_video(raop);
        assert(airplay_video);
        set_apple_session_id(airplay_video, apple_session_id, strlen(apple_session_id));
        const char *stored_location = get_playback_location(airplay_video);
        const char *spooled_location = get_spooled_media_path(airplay_video);
        const bool stored_is_spooled = stored_location && spooled_location &&
            !strcmp(stored_location, spooled_location);
        if (!media_location_is_http(stored_location) && !stored_is_spooled) {
            logger_log(raop->logger, LOGGER_ERR,
                       "[MEDIA] discarding incomplete stored playlist before replay");
            raop_destroy_airplay_video(raop, id);
            id = -1;
        } else {
            plist_mem_free(playback_uuid);
            plist_free(req_root_node);
            float resume_pos = get_resume_position_seconds(airplay_video);
            float start_pos = get_start_position_seconds(airplay_video);
            raop->callbacks.on_video_play(raop->callbacks.cls,
                                          stored_location,
                                          resume_pos > start_pos ? resume_pos : start_pos);
            return;
        }
    }
    
    /* initialize a new playlist (airplay_video structure) */
    /* first delete any short stored playlists (probably advertisements */
    int count = 0;
    for (int i = 0; i < MAX_AIRPLAY_VIDEO; i++) {
        if (raop->airplay_video[i]) {
            float duration = get_duration(raop->airplay_video[i]); 
            if (duration < (float) MIN_STORED_AIRPLAY_VIDEO_DURATION_SECONDS ) { //likely to be an advertisement
                logger_log(raop->logger, LOGGER_INFO,
                          "deleting playlist playback_uuid %s duration (seconds) %f",
                           get_playback_uuid(raop->airplay_video[i]), duration);
                raop_destroy_airplay_video(raop, i);
            } else {
                count++;
            }
        }
    }

    assert (count < MAX_AIRPLAY_VIDEO);
    assert(id == -1);
    /* initialize new airplay_video structure to hold playlist */
    for (int i = 0; i < MAX_AIRPLAY_VIDEO; i++) {
        if (raop->airplay_video[i]) {
            continue;
        }
        id = i;
        break;
    }
    assert(id >= 0);

    raop->current_video = id;
    raop->airplay_video[id] = airplay_video_init(raop, raop->port, raop->lang);
    airplay_video = hls_get_current_video(raop);
    assert(airplay_video);
    created_airplay_video = true;
    set_apple_session_id(airplay_video, apple_session_id, strlen(apple_session_id));
    set_playback_uuid(airplay_video, playback_uuid, strlen(playback_uuid));
    plist_mem_free (playback_uuid);
    count++;

    /* ensure that space will always be available for adding future playlists */

    if (count == MAX_AIRPLAY_VIDEO) {
        int next = (id + 1) % (int) MAX_AIRPLAY_VIDEO;
        logger_log(raop->logger, LOGGER_INFO,
                   "deleting playlist playback_uuid %s duration (seconds) %f",
                   get_playback_uuid(raop->airplay_video[next]),
                   get_duration(raop->airplay_video[next]));
        airplay_video_destroy(raop->airplay_video[next]);
        raop->airplay_video[next] = NULL;
    }
#if 0    
    for (int i = 0; i < MAX_AIRPLAY_VIDEO; i++) {
        printf("new: airplay_video[%d] %p %s %f\n", i, raop->airplay_video[i],
	       get_playback_uuid(raop->airplay_video[i]),
	       get_duration(raop->airplay_video[i]));
    }
#endif
	   
    plist_t req_content_location_node = plist_dict_get_item(req_root_node, "Content-Location");
    if (!req_content_location_node) {
        goto play_error;
    } else {
        plist_get_string_val(req_content_location_node, &playback_location);
    }
    if (!playback_location || !*playback_location) {
        logger_log(raop->logger, LOGGER_ERR,
                   "[MEDIA] POST /play Content-Location was empty or invalid");
        goto play_error;
    }
    if (!media_location_is_http(playback_location) &&
        media_location_is_blob(playback_location)) {
        plist_t start_node = plist_dict_get_item(req_root_node,
                                                  "Start-Position-Seconds");
        if (start_node) {
            double start = 0.0;
            plist_get_real_val(start_node, &start);
            start_position_seconds = (float) start;
        }
        set_start_position_seconds(airplay_video, start_position_seconds);
        set_playback_location(airplay_video, playback_location,
                              strlen(playback_location));
        set_reverse_media_pending(airplay_video, true);

        /* A page-local blob cannot be fetched by FFmpeg. Ask Safari to fetch
           it in the originating process and return the response bytes over
           the already-established PTTH reverse channel. */
        raop->callbacks.on_video_stop(raop->callbacks.cls);
        playback_stopped = true;
        const int reverse_result = fcup_request((void *) conn, playback_location,
                                                apple_session_id,
                                                get_next_FCUP_RequestID(airplay_video));
        if (reverse_result < 0) {
            logger_log(raop->logger, LOGGER_ERR,
                       "[MEDIA] Safari blob reverse-fetch request could not be sent");
            goto play_error;
        }
        logger_log(raop->logger, LOGGER_INFO,
                   "[MEDIA] requested page-local blob over reverse channel "
                   "(length=%zu, limit=%u MiB)",
                   strlen(playback_location), MAX_FCUP_MEDIA_BYTES / (1024U * 1024U));
        plist_mem_free(playback_location);
        plist_free(req_root_node);
        return;
    }
    if (!media_location_is_http(playback_location)) {
        char scheme[16];
        media_location_scheme(playback_location, scheme, sizeof(scheme));
        logger_log(raop->logger, LOGGER_ERR,
                   "[MEDIA] POST /play rejected Content-Location scheme=%s "
                   "length=%zu (only HTTP(S) is supported)",
                   scheme, strlen(playback_location));
        goto play_error;
    }

    plist_t req_client_proc_name_node = plist_dict_get_item(req_root_node, "clientProcName");
    if (req_client_proc_name_node) {
        plist_get_string_val(req_client_proc_name_node, &client_proc_name);
        logger_log(raop->logger, LOGGER_INFO, "[MEDIA] sender process: %s",
                   client_proc_name ? client_proc_name : "(unspecified)");
        plist_mem_free(client_proc_name);
    } else {
        logger_log(raop->logger, LOGGER_INFO, "[MEDIA] sender process not specified");
    }

    plist_t req_start_position_seconds_node = plist_dict_get_item(req_root_node, "Start-Position-Seconds");
    if (!req_start_position_seconds_node) {
        logger_log(raop->logger, LOGGER_INFO, "No Start-Position-Seconds in Play request");	    
    } else {
         double start_position = 0.0;
         plist_get_real_val(req_start_position_seconds_node, &start_position);
         start_position_seconds = (float) start_position;
    }
    set_start_position_seconds(airplay_video, (float) start_position_seconds);

    logger_log(raop->logger, LOGGER_INFO,
               "[MEDIA] play request parsed (start=%.3fs, mode=%s)",
               start_position_seconds,
               strstr(playback_location, ".m3u8") ? "HLS" : "direct URL");

    /* UxPlay's reverse-channel playlist proxy is useful for protected HLS
       playlists and is retained for its historical /master.m3u8 form. Safari
       also sends public MP4 URLs and HLS manifests with other names; those can
       be opened directly by the receiver's media player. */
    const char *uri_suffix = strstr(playback_location, "/master.m3u8");
    if (!uri_suffix) {
        set_playback_location(airplay_video, playback_location, strlen(playback_location));
        logger_log(raop->logger, LOGGER_INFO,
                   "[MEDIA] using receiver-side URL fetch (no playlist proxy required)");
        raop->callbacks.on_video_play(raop->callbacks.cls,
                                      get_playback_location(airplay_video),
                                      get_start_position_seconds(airplay_video));
    } else {
        size_t len = strlen(get_uri_local_prefix(airplay_video)) + strlen(uri_suffix);
        char *location = (char *) calloc(len + 1, sizeof(char));
        if (!location) {
            printf("Memory allocation failed (location)\n");
            exit(1);
        }
        strcat(location, get_uri_local_prefix(airplay_video));
        strcat(location, uri_suffix);
        set_playback_location(airplay_video, location, strlen(location));
        free(location);
        char *uri_prefix = (char *) calloc(strlen(playback_location) + 1, sizeof(char));
        if (!uri_prefix) {
            printf("Memory allocation failed (uri_prefix)\n");
            exit(1);
        }
        strcat(uri_prefix, playback_location);
        char *end = strstr(uri_prefix, "/master.m3u8");
        *end = '\0';						  
        set_uri_prefix(airplay_video, uri_prefix, strlen(uri_prefix));
        free (uri_prefix);
    }
    if (uri_suffix) {
        set_next_media_uri_id(airplay_video, 0);
        int reverse_result = fcup_request((void *) conn, playback_location,
                                          apple_session_id,
                                          get_next_FCUP_RequestID(airplay_video));
        if (reverse_result < 0) {
            /* Public playlists remain playable even if the sender omitted or
               closed the PTTH reverse channel. */
            logger_log(raop->logger, LOGGER_WARNING,
                       "[MEDIA] reverse playlist fetch unavailable; trying direct URL fetch");
            set_playback_location(airplay_video, playback_location, strlen(playback_location));
            raop->callbacks.on_video_play(raop->callbacks.cls,
                                          get_playback_location(airplay_video),
                                          get_start_position_seconds(airplay_video));
        } else {
            logger_log(raop->logger, LOGGER_INFO,
                       "[MEDIA] requested HLS master playlist over reverse channel");
        }
    }

    plist_mem_free(playback_location);

    if (req_root_node) {
        plist_free(req_root_node);
    }
    return;

 play_error:;
    plist_mem_free(playback_location);
    if (!playback_stopped) {
        raop->callbacks.on_video_stop(raop->callbacks.cls);
    }
    if (created_airplay_video && id >= 0) {
        raop_destroy_airplay_video(raop, id);
    }
    if (req_root_node) {
        plist_free(req_root_node);
    }
    logger_log(raop->logger, LOGGER_ERR, "Could not find valid Plist Data for POST/play request, Unhandled");
    http_response_init(response, "HTTP/1.1", 400, "Bad Request");
    http_response_set_disconnect(response, 1);
}

/* the HLS handler handles http requests GET /[uri] on the HLS channel from the media player to the Server, asking for
   (adjusted) copies of Playlists: first the Master Playlist  (adjusted to change the uri prefix to
   "http://localhost:[port]/.......m3u8"), then the Media Playlists that the media player wishes to use.  
   If the client supplied Media playlists with the "YT-EXT-CONDENSED-URI" header, these must be adjusted into
   the standard uncondensed form before sending with the response.    The uri in the request is  the uri for the
   Media Playlist, taken from the Master Playlist, with the uri prefix removed.  
*/ 

static void
http_handler_hls(raop_conn_t *conn,  http_request_t *request, http_response_t *response,
                 char **response_data, int *response_datalen) {
    raop_t *raop = conn->raop;
    if (raop->current_video == -1) {
        logger_log(raop->logger, LOGGER_ERR,"airplay_video playlist  not found");
        http_response_init(response, "HTTP/1.1", 404, "Not Found");
        return;
    }
    const char *method = http_request_get_method(request);
    assert (!strcmp(method, "GET"));
    const char *url = http_request_get_url(request);    
    const char* upgrade = http_request_get_header(request, "Upgrade");
    if (upgrade) {
        //don't accept Upgrade: h2c request ?
        char *header_str = NULL;
        http_request_get_header_string(request, &header_str);
        logger_log(raop->logger, LOGGER_INFO,
                   "%s\nhls upgrade request declined", header_str); 
        free (header_str);
        return;
    }
    airplay_video_t *airplay_video = (airplay_video_t *) hls_get_current_video(raop);
    assert(airplay_video);
    if (!strcmp(url, "/master.m3u8")){
        char * master_playlist  = get_master_playlist(airplay_video);
        if (master_playlist) {
            size_t len = strlen(master_playlist);
            char * data = (char *) malloc(len + 1);
            if (!data) {
                printf("Memory allocation failed (data)\n");
                exit(1);
            }
            memcpy(data, master_playlist, len);
            data[len] = '\0';
            *response_data = data;
            *response_datalen = (int ) len;
        } else {
            logger_log(raop->logger, LOGGER_ERR,"requested master playlist %s not found", url); 
        }

    } else {
        int chunks = 0;
        float duration = 0.0f;
        char *media_playlist = get_media_playlist(airplay_video, &chunks, &duration, url);
        if (media_playlist) {
            char *data  = adjust_yt_condensed_playlist(media_playlist);
            *response_data = data;
            *response_datalen = strlen(data);
            logger_log(raop->logger, LOGGER_INFO,
                       "Requested media_playlist %s has %5d chunks, total duration %9.3f secs", url, chunks, duration); 
        } else {
            logger_log(raop->logger, LOGGER_ERR,"requested media playlist %s not found", url); 
        }
	    
    }

    http_response_add_header(response, "Access-Control-Allow-Headers", "Content-type");
    http_response_add_header(response, "Access-Control-Allow-Origin", "*");
    const char *date = NULL;
    date = gmt_time_string();
    http_response_add_header(response, "Date", date);
    if (*response_datalen > 0) {
        http_response_add_header(response, "Content-Type", "application/x-mpegURL; charset=utf-8");
    } else if (*response_datalen == 0) {
        http_response_init(response, "HTTP/1.1", 404, "Not Found");
    }
}
