/*
 * AirPlay video protocol trace for the OBS AirPlay plugin.
 *
 * Logs every request and response on the AirPlay HTTP channels (/play,
 * /reverse and the PTTH upgrade, /playback-info, /scrub, /rate, /stop,
 * /action FCUP replies, reverse-HTTP responses) with the relevant headers and
 * a compact, truncated dump of binary-plist, XML-plist or text bodies.
 *
 * Every line is emitted at raop->media_trace_level (LOGGER_DEBUG unless the
 * host raises it), so the trace costs nothing when it is not enabled.
 * Long strings (signed URLs, blob identifiers) and data blobs are truncated.
 */

#ifndef MEDIA_TRACE_H
#define MEDIA_TRACE_H

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <plist/plist.h>

#define MEDIA_TRACE_MAX_STRING 160
#define MEDIA_TRACE_MAX_DATA_HEX 24
#define MEDIA_TRACE_MAX_DEPTH 6
#define MEDIA_TRACE_LINE_CHUNK 3500
#define MEDIA_TRACE_MAX_OUTPUT 24000

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool truncated;
} media_trace_buf_t;

static bool
media_trace_enabled(raop_t *raop) {
    return raop && logger_get_level(raop->logger) >= raop->media_trace_level;
}

static void
media_trace_append(media_trace_buf_t *b, const char *fmt, ...) {
    if (b->truncated) return;
    va_list ap;
    va_start(ap, fmt);
    char tmp[512];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t add = (size_t) n < sizeof(tmp) ? (size_t) n : sizeof(tmp) - 1;
    if (b->len + add + 1 > MEDIA_TRACE_MAX_OUTPUT) {
        b->truncated = true;
        return;
    }
    if (b->len + add + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        while (cap < b->len + add + 1) cap *= 2;
        char *grown = realloc(b->buf, cap);
        if (!grown) {
            b->truncated = true;
            return;
        }
        b->buf = grown;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, tmp, add);
    b->len += add;
    b->buf[b->len] = '\0';
}

static void
media_trace_append_string(media_trace_buf_t *b, const char *s, size_t len) {
    const size_t shown = len > MEDIA_TRACE_MAX_STRING ? MEDIA_TRACE_MAX_STRING : len;
    media_trace_append(b, "\"");
    for (size_t i = 0; i < shown; i++) {
        unsigned char c = (unsigned char) s[i];
        if (c == '"' || c == '\\') media_trace_append(b, "\\%c", c);
        else if (c >= 0x20 && c < 0x7f) media_trace_append(b, "%c", c);
        else if (c == '\n') media_trace_append(b, "\\n");
        else if (c == '\r') media_trace_append(b, "\\r");
        else if (c >= 0x80) media_trace_append(b, "%c", c); /* keep UTF-8 */
        else media_trace_append(b, "\\x%02x", c);
    }
    media_trace_append(b, "\"");
    if (shown < len) media_trace_append(b, "...(%zu chars)", len);
}

static void media_trace_append_plist(media_trace_buf_t *b, plist_t node, int depth);

static void
media_trace_append_data(media_trace_buf_t *b, const char *data, uint64_t len, int depth) {
    if (data && len > 8 && !memcmp(data, "bplist00", 8) && depth < MEDIA_TRACE_MAX_DEPTH) {
        plist_t nested = NULL;
        plist_from_bin(data, (uint32_t) len, &nested);
        if (nested) {
            media_trace_append(b, "<bplist %llu bytes>", (unsigned long long) len);
            media_trace_append_plist(b, nested, depth + 1);
            plist_free(nested);
            return;
        }
    }
    media_trace_append(b, "<data %llu bytes", (unsigned long long) len);
    if (data && len) {
        media_trace_append(b, ":");
        uint64_t shown = len < MEDIA_TRACE_MAX_DATA_HEX ? len : MEDIA_TRACE_MAX_DATA_HEX;
        for (uint64_t i = 0; i < shown; i++) {
            media_trace_append(b, "%02x", (unsigned char) data[i]);
        }
        if (shown < len) media_trace_append(b, "..");
    }
    media_trace_append(b, ">");
}

static void
media_trace_append_plist(media_trace_buf_t *b, plist_t node, int depth) {
    if (!node) {
        media_trace_append(b, "null");
        return;
    }
    if (depth > MEDIA_TRACE_MAX_DEPTH) {
        media_trace_append(b, "...");
        return;
    }
    switch (plist_get_node_type(node)) {
    case PLIST_DICT: {
        media_trace_append(b, "{");
        plist_dict_iter it = NULL;
        plist_dict_new_iter(node, &it);
        bool first = true;
        while (it) {
            char *key = NULL;
            plist_t value = NULL;
            plist_dict_next_item(node, it, &key, &value);
            if (!key) break;
            media_trace_append(b, "%s%s=", first ? "" : ", ", key);
            media_trace_append_plist(b, value, depth + 1);
            plist_mem_free(key);
            first = false;
        }
        if (it) free(it);
        media_trace_append(b, "}");
        break;
    }
    case PLIST_ARRAY: {
        media_trace_append(b, "[");
        uint32_t n = plist_array_get_size(node);
        for (uint32_t i = 0; i < n; i++) {
            media_trace_append(b, "%s", i ? ", " : "");
            media_trace_append_plist(b, plist_array_get_item(node, i), depth + 1);
        }
        media_trace_append(b, "]");
        break;
    }
    case PLIST_STRING: {
        char *s = NULL;
        plist_get_string_val(node, &s);
        media_trace_append_string(b, s ? s : "", s ? strlen(s) : 0);
        plist_mem_free(s);
        break;
    }
    case PLIST_KEY: {
        char *s = NULL;
        plist_get_key_val(node, &s);
        media_trace_append_string(b, s ? s : "", s ? strlen(s) : 0);
        plist_mem_free(s);
        break;
    }
    case PLIST_BOOLEAN: {
        uint8_t v = 0;
        plist_get_bool_val(node, &v);
        media_trace_append(b, "%s", v ? "true" : "false");
        break;
    }
    case PLIST_UINT: {
        uint64_t v = 0;
        plist_get_uint_val(node, &v);
        media_trace_append(b, "%llu", (unsigned long long) v);
        break;
    }
    case PLIST_REAL: {
        double v = 0.0;
        plist_get_real_val(node, &v);
        media_trace_append(b, "%.3f", v);
        break;
    }
    case PLIST_DATE: {
        int64_t sec = 0;
        plist_get_unix_date_val(node, &sec);
        media_trace_append(b, "<date %lld>", (long long) sec);
        break;
    }
    case PLIST_DATA: {
        uint64_t len = 0;
#ifdef PLIST_210
        /* only a few bytes are printed: do not copy a media-sized blob */
        const char *data = plist_get_data_ptr(node, &len);
        media_trace_append_data(b, data, len, depth);
#else
        char *data = NULL;
        plist_get_data_val(node, &data, &len);
        media_trace_append_data(b, data, len, depth);
        plist_mem_free(data);
#endif
        break;
    }
    case PLIST_UID: {
        uint64_t v = 0;
        plist_get_uid_val(node, &v);
        media_trace_append(b, "<uid %llu>", (unsigned long long) v);
        break;
    }
    default:
        media_trace_append(b, "<type %d>", (int) plist_get_node_type(node));
        break;
    }
}

/* Emit a (possibly long) trace message in chunks the logger can carry. */
static void
media_trace_emit(raop_t *raop, const char *prefix, media_trace_buf_t *b) {
    const char *text = b->buf ? b->buf : "";
    size_t len = b->len;
    size_t offset = 0;
    int part = 0;
    do {
        size_t n = len - offset > MEDIA_TRACE_LINE_CHUNK ? MEDIA_TRACE_LINE_CHUNK : len - offset;
        logger_log(raop->logger, raop->media_trace_level, "[MEDIA-TRACE] %s%s%.*s%s",
                   prefix, part ? " (cont.) " : " ", (int) n, text + offset,
                   (offset + n == len && b->truncated) ? " ...(trace truncated)" : "");
        offset += n;
        part++;
    } while (offset < len);
}

/* Append a decoded body: binary plist, XML plist, text, or a hex preview. */
static void
media_trace_append_body(media_trace_buf_t *b, const char *content_type,
                        const char *data, int datalen) {
    if (!data || datalen <= 0) {
        media_trace_append(b, " body=(none)");
        return;
    }
    const bool is_bplist = datalen > 8 && !memcmp(data, "bplist00", 8);
    const bool is_xml_plist = content_type && strstr(content_type, "plist+xml");
    if (is_bplist || is_xml_plist) {
        plist_t root = NULL;
        if (is_bplist) plist_from_bin(data, (uint32_t) datalen, &root);
        else plist_from_xml(data, (uint32_t) datalen, &root);
        if (root) {
            media_trace_append(b, " body=%s ", is_bplist ? "bplist" : "xml-plist");
            media_trace_append_plist(b, root, 0);
            plist_free(root);
            return;
        }
        media_trace_append(b, " body=(%s did not parse)", is_bplist ? "bplist" : "xml-plist");
    }
    bool printable = true;
    for (int i = 0; i < datalen && i < 2048; i++) {
        unsigned char c = (unsigned char) data[i];
        if (c < 0x09 || (c > 0x0d && c < 0x20)) {
            printable = false;
            break;
        }
    }
    if (printable) {
        media_trace_append(b, " body=text ");
        media_trace_append_string(b, data, (size_t) datalen);
    } else {
        media_trace_append(b, " body=");
        media_trace_append_data(b, data, (uint64_t) datalen, MEDIA_TRACE_MAX_DEPTH);
    }
}

static void
media_trace_append_header(media_trace_buf_t *b, http_request_t *request, const char *name) {
    const char *value = http_request_get_header(request, name);
    if (value) {
        media_trace_append(b, " %s=", name);
        media_trace_append_string(b, value, strlen(value));
    }
}

static bool
media_trace_quiet_poll(const char *url) {
    return url && !strcmp(url, "/playback-info");
}

/* Called for each HTTP/1.1 (AirPlay video) request before it is handled. */
static void
media_trace_request(raop_conn_t *conn, http_request_t *request, const char *method,
                    const char *url, const char *protocol) {
    raop_t *raop = conn->raop;
    if (!media_trace_enabled(raop)) return;
    media_trace_buf_t b = {0};
    int datalen = 0;
    const char *data = http_request_get_data(request, &datalen);
    media_trace_append(&b, "conn=%p type=%d %s %s %s", (void *) conn,
                       (int) conn->connection_type, method, url, protocol);
    media_trace_append_header(&b, request, "Content-Type");
    media_trace_append_header(&b, request, "Content-Length");
    media_trace_append_header(&b, request, "X-Apple-Session-ID");
    media_trace_append_header(&b, request, "X-Apple-Purpose");
    media_trace_append_header(&b, request, "X-Apple-Device-ID");
    media_trace_append_header(&b, request, "X-Apple-ProtocolVersion");
    media_trace_append_header(&b, request, "Connection");
    media_trace_append_header(&b, request, "Upgrade");
    media_trace_append_header(&b, request, "User-Agent");
    media_trace_append_header(&b, request, "Range");
    media_trace_append(&b, " datalen=%d", datalen);
    if (!media_trace_quiet_poll(url)) {
        media_trace_append_body(&b, http_request_get_header(request, "Content-Type"),
                                data, datalen);
    }
    media_trace_emit(raop, "->", &b);
    free(b.buf);

    if (!media_trace_quiet_poll(url)) {
        /* Also emit the complete raw header block once, so unexpected
           headers are never lost. */
        char *header_str = NULL;
        http_request_get_header_string(request, &header_str);
        if (header_str) {
            media_trace_buf_t h = {0};
            media_trace_append_string(&h, header_str, strlen(header_str));
            media_trace_emit(raop, "-> headers", &h);
            free(h.buf);
            free(header_str);
        }
    }
}

/* Called after the handler has produced its response. */
static void
media_trace_response(raop_conn_t *conn, http_response_t *response, const char *method,
                     const char *url, const char *response_data, int response_datalen) {
    raop_t *raop = conn->raop;
    if (!media_trace_enabled(raop)) return;
    int len = 0;
    const char *raw = http_response_get_data(response, &len);
    const char *eol = raw ? memchr(raw, '\r', (size_t) len) : NULL;
    media_trace_buf_t b = {0};
    media_trace_append(&b, "%s %s -> %.*s disconnect=%d", method, url,
                       raw ? (int) (eol ? eol - raw : (len > 80 ? 80 : len)) : 0, raw ? raw : "",
                       http_response_get_disconnect(response));
    /* the response buffer is not NUL-terminated: search a bounded copy of
       its header part */
    const char *ct = NULL;
    char head[512];
    const size_t head_len = raw && len > 0
        ? ((size_t) len < sizeof(head) - 1 ? (size_t) len : sizeof(head) - 1) : 0;
    if (head_len) memcpy(head, raw, head_len);
    head[head_len] = '\0';
    if (strstr(head, "apple-binary-plist")) ct = "application/x-apple-binary-plist";
    else if (strstr(head, "plist+xml")) ct = "text/x-apple-plist+xml";
    media_trace_append_body(&b, ct, response_data, response_datalen);
    media_trace_emit(raop, "<-", &b);
    free(b.buf);
}

#endif
