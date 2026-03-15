/* SPDX-License-Identifier: MIT */
/* AlJefra OS — Compiler builtin replacements
 * GCC may emit implicit calls to these even with -ffreestanding.
 *
 * Optimized with 64-bit word operations for performance.
 * No SSE/MMX — safe for freestanding kernel use.
 */

#include "string.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    /* Align destination to 8-byte boundary */
    while (n && ((uintptr_t)d & 7)) {
        *d++ = *s++;
        n--;
    }

    /* Copy 8 bytes at a time */
    uint64_t *d64 = (uint64_t *)d;
    const uint64_t *s64 = (const uint64_t *)s;
    while (n >= 8) {
        *d64++ = *s64++;
        n -= 8;
    }

    /* Copy remaining bytes */
    d = (uint8_t *)d64;
    s = (const uint8_t *)s64;
    while (n--)
        *d++ = *s++;

    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint8_t val = (uint8_t)c;

    /* Align destination to 8-byte boundary */
    while (n && ((uintptr_t)d & 7)) {
        *d++ = val;
        n--;
    }

    /* Fill 8 bytes at a time */
    if (n >= 8) {
        uint64_t wide = (uint64_t)val;
        wide |= wide << 8;
        wide |= wide << 16;
        wide |= wide << 32;

        uint64_t *d64 = (uint64_t *)d;
        while (n >= 8) {
            *d64++ = wide;
            n -= 8;
        }
        d = (uint8_t *)d64;
    }

    /* Fill remaining bytes */
    while (n--)
        *d++ = val;

    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d < s || d >= s + n) {
        /* No overlap or dst before src — forward copy */
        /* Align destination to 8-byte boundary */
        while (n && ((uintptr_t)d & 7)) {
            *d++ = *s++;
            n--;
        }
        uint64_t *d64 = (uint64_t *)d;
        const uint64_t *s64 = (const uint64_t *)s;
        while (n >= 8) {
            *d64++ = *s64++;
            n -= 8;
        }
        d = (uint8_t *)d64;
        s = (const uint8_t *)s64;
        while (n--)
            *d++ = *s++;
    } else {
        /* Overlap, dst after src — backward copy */
        d += n;
        s += n;

        /* Align destination to 8-byte boundary (from end) */
        while (n && ((uintptr_t)d & 7)) {
            *--d = *--s;
            n--;
        }
        uint64_t *d64 = (uint64_t *)d;
        const uint64_t *s64 = (const uint64_t *)s;
        while (n >= 8) {
            *--d64 = *--s64;
            n -= 8;
        }
        d = (uint8_t *)d64;
        s = (const uint8_t *)s64;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    while (n--) {
        if (*pa != *pb)
            return *pa - *pb;
        pa++;
        pb++;
    }
    return 0;
}

int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

uint32_t str_len(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

void str_copy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i + 1 < max && src[i]; i++)
        dst[i] = src[i];
    if (max > 0)
        dst[i] = '\0';
}
