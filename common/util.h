#ifndef RGX_UTIL_H
#define RGX_UTIL_H
#include <stddef.h>

#if defined(__GNUC__)
#define RGX_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define RGX_NORETURN __declspec(noreturn)
#else
#define RGX_NORETURN
#endif

/* Append `item` to a growable array. */
#define VEC_PUSH(arr, len, cap, item)                                      \
    do {                                                                   \
        if ((len) >= (cap)) {                                              \
            (cap) = (cap) ? (cap) * 2 : 4;                                 \
            (arr) = xrealloc((arr), (size_t)(cap) * sizeof(*(arr)));       \
        }                                                                  \
        (arr)[(len)++] = (item);                                           \
    } while (0)

void *xmalloc(size_t n);
void *xcalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xsprintf(const char *fmt, ...);

/* Whole file as a NUL-terminated buffer, or NULL on failure. */
char *read_file(const char *path, size_t *len);

extern int rgx_nerrors;

/* Print "file:line:col: error: ..." and count it. */
void rgx_error(const char *file, int line, int col, const char *fmt, ...);
/* Same, then exit(1). */
RGX_NORETURN void rgx_die(const char *file, int line, int col, const char *fmt, ...);

#endif
