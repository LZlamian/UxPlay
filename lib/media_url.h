/*
 * Media URL parsing and address classes (obs-airplay), shared by the
 * receiver's own URL policy (media_url_policy.h) and the host application,
 * which applies the same rules to resolved addresses: one implementation,
 * one set of test cases.
 */

#ifndef MEDIA_URL_H
#define MEDIA_URL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_URL_HOST_BYTES 256

typedef enum {
    MEDIA_ADDRESS_ROUTABLE,
    MEDIA_ADDRESS_LOOPBACK,
    MEDIA_ADDRESS_LINK_LOCAL,
    MEDIA_ADDRESS_MULTICAST,
    MEDIA_ADDRESS_UNSPECIFIED,
    MEDIA_ADDRESS_RESERVED,
} media_address_class_t;

/* An IPv4 address in host byte order. */
media_address_class_t media_classify_ipv4(uint32_t address);
/* IPv4-mapped, NAT64 and IPv4-compatible addresses are classified by the
   IPv4 address they reach. */
media_address_class_t media_classify_ipv6(const unsigned char address[16]);

/* Splits scheme://[userinfo@]host[:port]/... into a lower-case host (without
   brackets, IPv6 zone or trailing dots) and a port. False for anything that
   is not a well-formed http(s) URL. */
bool media_url_split(const char *url, char *host, size_t host_size, long *port);

/* Parses a literal address (inet_aton also takes the shorthand and
   decimal/hex forms resolvers accept). `address` gets 4 or 16 bytes. */
bool media_parse_literal(const char *host, int *family, unsigned char address[16]);

/* http:// or https:// (case-insensitive). */
bool media_url_is_http(const char *url);

#ifdef __cplusplus
}
#endif

#endif
