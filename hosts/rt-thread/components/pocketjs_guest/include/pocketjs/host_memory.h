#pragma once
#include <stddef.h>
#include <string.h>
#include <stdint.h>
/* The consuming port supplies this allocation seam; D13x uses PSRAM_SW only. */
void *pjs_host_alloc(uint32_t size, uint32_t align);
void pjs_host_free(void *pointer, uint32_t align);
static inline void *pjs_malloc(size_t n) { return n && n <= UINT32_MAX ? pjs_host_alloc((uint32_t)n, 16) : NULL; }
static inline void *pjs_calloc(size_t c, size_t n) { size_t z = c && n > SIZE_MAX / c ? 0 : c * n; void *p = z ? pjs_malloc(z) : NULL; if (p) memset(p, 0, z); return p; }
static inline void pjs_free(void *p) { if (p) pjs_host_free(p, 16); }
static inline char *pjs_strdup(const char *s) { size_t n = s ? strlen(s) + 1 : 0; char *p = n ? pjs_malloc(n) : NULL; if (p) memcpy(p, s, n); return p; }
