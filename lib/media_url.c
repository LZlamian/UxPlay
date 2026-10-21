/*
 * Media URL parsing and address classes (obs-airplay): see media_url.h.
 */

#include "media_url.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif



media_address_class_t
media_classify_ipv4(uint32_t address) {
    const uint32_t first = address >> 24;
    if (first == 0) return MEDIA_ADDRESS_UNSPECIFIED;        /* 0.0.0.0/8 */
    if (first == 127) return MEDIA_ADDRESS_LOOPBACK;         /* 127.0.0.0/8 */
    if ((address & 0xffff0000U) == 0xa9fe0000U) return MEDIA_ADDRESS_LINK_LOCAL; /* 169.254/16 */
    if (first >= 224 && first <= 239) return MEDIA_ADDRESS_MULTICAST;
    if (first >= 240) return MEDIA_ADDRESS_RESERVED;         /* incl. broadcast */
    return MEDIA_ADDRESS_ROUTABLE;
}

media_address_class_t
media_classify_ipv6(const unsigned char a[16]) {
    static const unsigned char zero[16] = {0};
    static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    static const unsigned char nat64[12] = {0x00, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0};
    if (!memcmp(a, zero, 16)) return MEDIA_ADDRESS_UNSPECIFIED;
    if (!memcmp(a, zero, 15) && a[15] == 1) return MEDIA_ADDRESS_LOOPBACK;
    if (!memcmp(a, mapped, 12) || !memcmp(a, nat64, 12) || !memcmp(a, zero, 12)) {
        /* IPv4-mapped, NAT64 and (deprecated) IPv4-compatible addresses
           reach the embedded IPv4 address */
        const uint32_t v4 = ((uint32_t) a[12] << 24) | ((uint32_t) a[13] << 16) |
                            ((uint32_t) a[14] << 8) | (uint32_t) a[15];
        return media_classify_ipv4(v4);
    }
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) return MEDIA_ADDRESS_LINK_LOCAL;
    if (a[0] == 0xff) return MEDIA_ADDRESS_MULTICAST;
    return MEDIA_ADDRESS_ROUTABLE;
}

/* Splits scheme://[userinfo@]host[:port]/... into a lower-case host (without
   brackets or IPv6 zone) and a port.  Returns false for anything that is not
   a well-formed http(s) URL. */
bool
media_url_split(const char *url, char *host, size_t host_size, long *port) {
    long default_port;
    const char *authority;
    if (!strncasecmp(url, "http://", 7)) {
        default_port = 80;
        authority = url + 7;
    } else if (!strncasecmp(url, "https://", 8)) {
        default_port = 443;
        authority = url + 8;
    } else {
        return false;
    }
    const char *end = authority + strcspn(authority, "/?#");
    const char *at = NULL;
    for (const char *c = authority; c < end; ++c) {
        if (*c == '@') at = c;
    }
    if (at) authority = at + 1;
    if (authority >= end) return false;

    const char *host_start = authority;
    const char *host_end;
    const char *port_start = NULL;
    if (*authority == '[') {
        host_start = authority + 1;
        host_end = memchr(host_start, ']', (size_t) (end - host_start));
        if (!host_end) return false;
        if (host_end + 1 < end) {
            if (host_end[1] != ':') return false;
            port_start = host_end + 2;
        }
        const char *zone = memchr(host_start, '%', (size_t) (host_end - host_start));
        if (zone) host_end = zone;
    } else {
        host_end = memchr(authority, ':', (size_t) (end - authority));
        if (host_end) {
            port_start = host_end + 1;
        } else {
            host_end = end;
        }
    }
    size_t host_length = (size_t) (host_end - host_start);
    if (host_length == 0 || host_length >= host_size) return false;
    for (size_t i = 0; i < host_length; ++i) {
        const unsigned char c = (unsigned char) host_start[i];
        if (c <= 0x20 || c == 0x7f) return false;
        host[i] = (char) tolower(c);
    }
    host[host_length] = '\0';
    /* "localhost." names the same host as "localhost" */
    while (host_length > 1 && host[host_length - 1] == '.') host[--host_length] = '\0';

    *port = default_port;
    if (port_start && port_start < end) {
        long value = 0;
        for (const char *c = port_start; c < end; ++c) {
            if (*c < '0' || *c > '9') return false;
            value = value * 10 + (*c - '0');
            if (value > 65535) return false;
        }
        if (value == 0) return false;
        *port = value;
    }
    return true;
}

/* Parses a literal address.  inet_aton also accepts the shorthand and
   decimal/hex forms ("127.1", "2130706433", "0x7f.1") that resolvers accept. */
bool
media_parse_literal(const char *host, int *family, unsigned char address[16]) {
    struct in6_addr v6;
    struct in_addr v4;
    if (inet_pton(AF_INET6, host, &v6) == 1) {
        *family = AF_INET6;
        memcpy(address, &v6, 16);
        return true;
    }
    if (inet_aton(host, &v4) == 1) {
        *family = AF_INET;
        memcpy(address, &v4, 4);
        return true;
    }
    return false;
}

bool
media_url_is_http(const char *url) {
    return url && (!strncasecmp(url, "http://", 7) || !strncasecmp(url, "https://", 8));
}
