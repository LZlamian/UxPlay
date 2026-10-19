/*
 * (obs-airplay) Binary plists from the network, refused when a string or a
 * dictionary key holds a NUL byte.
 *
 * libplist's getters assert that a string's length equals its strlen()
 * (plist_get_string_val, plist_get_key_val). libplist built without NDEBUG
 * (its default) then aborts the process on a string with an embedded NUL,
 * which a sender on the network could send in any request body.  Strings and
 * keys are the same objects in a binary plist, so the raw bytes are checked
 * before parsing, independent of how libplist was built.
 */
#ifndef PLIST_CHECKED_H
#define PLIST_CHECKED_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <plist/plist.h>

static inline uint64_t
bplist_read_be(const unsigned char *bytes, unsigned int count) {
    uint64_t value = 0;
    for (unsigned int i = 0; i < count; i++) value = (value << 8) | bytes[i];
    return value;
}

/* true when any string object holds a NUL, or the structure is unreadable */
static inline bool
bplist_strings_have_nul(const char *data, uint32_t size) {
    const unsigned char *bytes = (const unsigned char *) data;
    if (!data || size < 8 + 32 || memcmp(bytes, "bplist00", 8) != 0) return true;
    const unsigned char *trailer = bytes + size - 32;
    const unsigned int offset_size = trailer[6];
    const uint64_t object_count = bplist_read_be(trailer + 8, 8);
    const uint64_t table_offset = bplist_read_be(trailer + 24, 8);
    if (offset_size < 1 || offset_size > 8 || table_offset < 8 ||
        table_offset > (uint64_t) size - 32 ||
        object_count > ((uint64_t) size - 32 - table_offset) / offset_size) {
        return true;
    }
    for (uint64_t i = 0; i < object_count; i++) {
        const uint64_t offset = bplist_read_be(bytes + table_offset + i * offset_size, offset_size);
        if (offset < 8 || offset >= table_offset) return true;
        const unsigned char marker = bytes[offset];
        const unsigned int type = marker >> 4;
        if (type != 0x5 && type != 0x6 && type != 0x7) continue;  /* ASCII, UTF-16, UTF-8 */
        uint64_t count = marker & 0x0F;
        uint64_t start = offset + 1;
        if (count == 0x0F) {
            /* the length follows as an integer object: 0x1n, 2^n bytes */
            if (start >= table_offset || (bytes[start] >> 4) != 0x1) return true;
            const unsigned int length_bytes = 1U << (bytes[start] & 0x0F);
            if (length_bytes > 8 || start + 1 + length_bytes > table_offset) return true;
            count = bplist_read_be(bytes + start + 1, length_bytes);
            start += 1 + length_bytes;
        }
        const uint64_t unit = type == 0x6 ? 2 : 1;
        if (count > (table_offset - start) / unit) return true;
        const unsigned char *text = bytes + start;
        if (unit == 1) {
            if (memchr(text, 0, (size_t) count)) return true;
        } else {
            for (uint64_t c = 0; c < count; c++) {
                if (text[2 * c] == 0 && text[2 * c + 1] == 0) return true;
            }
        }
    }
    return false;
}

/* plist_from_bin for untrusted data: *plist is NULL when the data is refused
   (or does not parse), as plist_from_bin leaves it for a parse failure */
static inline void
plist_from_bin_checked(const char *data, uint32_t size, plist_t *plist) {
    *plist = NULL;
    if (bplist_strings_have_nul(data, size)) return;
    plist_from_bin(data, size, plist);
}

#endif
