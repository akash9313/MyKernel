#ifndef ASTRA_KERNEL_STRING_H
#define ASTRA_KERNEL_STRING_H

#include <kernel/types.h>

static inline size_t kstrlen(const char *s) {
    if (!s) return 0;
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static inline int kstrcmp(const char *s1, const char *s2) {
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static inline int kstrncmp(const char *s1, const char *s2, size_t n) {
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static inline char *kstrcpy(char *dest, const char *src) {
    if (!dest) return NULL;
    char *d = dest;
    if (!src) { *d = '\0'; return dest; }
    while ((*d++ = *src++));
    return dest;
}

static inline char *kstrncpy(char *dest, const char *src, size_t n) {
    if (!dest) return NULL;
    char *d = dest;
    if (!src) {
        while (n--) *d++ = '\0';
        return dest;
    }
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dest;
}

static inline char *kstrcat(char *dest, const char *src) {
    if (!dest) return NULL;
    char *d = dest;
    while (*d) d++;
    if (src) {
        while ((*d++ = *src++));
    }
    return dest;
}

#endif /* ASTRA_KERNEL_STRING_H */
