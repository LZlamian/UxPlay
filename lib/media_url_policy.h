/*
 * Policy for media URLs a sender asks the receiver to fetch (POST /play).
 *
 * Any LAN peer can send POST /play without pairing, so the receiver must not
 * become a proxy into the host itself or onto unusual services: loopback,
 * unspecified, multicast and reserved targets are refused, link-local ones
 * are allowed only when they are the sender's own address, and hosts other
 * than the sender may only be reached on the configured web ports (or not at
 * all with raop_set_media_policy's sender_only).
 *
 * This is a textual check of the URL.  Hostnames are resolved, redirects
 * followed and nested HLS resources fetched by the host application, which
 * applies the same rules to the resolved addresses.
 *
 * this file is part of raop.c via http_handlers.h and should not be included
 * in any other file
 */

#ifndef MEDIA_URL_POLICY_H
#define MEDIA_URL_POLICY_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <strings.h>

#include "media_url.h"

static bool
media_port_allowed(raop_t *raop, long port) {
    for (int i = 0; i < raop->media_port_count; ++i) {
        if (raop->media_ports[i] == port) return true;
    }
    return false;
}

/* NULL when the URL may be fetched for this connection's sender, otherwise
   a short reason for the log. */
static const char *
media_url_policy_check(raop_conn_t *conn, const char *url) {
    raop_t *raop = conn->raop;
    char host[MEDIA_URL_HOST_BYTES];
    long port = 0;
    if (!url || !media_url_split(url, host, sizeof(host), &port)) {
        return "malformed URL";
    }
    const size_t host_length = strlen(host);
    if (!strcmp(host, "localhost") ||
        (host_length > 10 && !strcmp(host + host_length - 10, ".localhost"))) {
        return "loopback host";
    }

    bool is_peer = false;
    int family = 0;
    unsigned char address[16] = {0};
    if (media_parse_literal(host, &family, address)) {
        uint32_t v4 = 0;
        memcpy(&v4, address, sizeof(v4));
        media_address_class_t address_class = family == AF_INET
            ? media_classify_ipv4(ntohl(v4))
            : media_classify_ipv6(address);
        if (conn->remotelen == (family == AF_INET ? 4 : 16)) {
            is_peer = !memcmp(conn->remote, address, (size_t) conn->remotelen);
        }
        switch (address_class) {
        case MEDIA_ADDRESS_LOOPBACK:    return "loopback address";
        case MEDIA_ADDRESS_UNSPECIFIED: return "unspecified address";
        case MEDIA_ADDRESS_MULTICAST:   return "multicast address";
        case MEDIA_ADDRESS_RESERVED:    return "reserved address";
        case MEDIA_ADDRESS_LINK_LOCAL:
            if (!is_peer) return "link-local address that is not the sender";
            break;
        case MEDIA_ADDRESS_ROUTABLE:
            break;
        }
    }
    if (!is_peer && atomic_load(&raop->media_sender_only)) {
        return "host is not the sending device (only the sending device is allowed)";
    }
    if (!is_peer && !media_port_allowed(raop, port)) {
        return "port not allowed for a host other than the sender";
    }
    return NULL;
}

#endif
