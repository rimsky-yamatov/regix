#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rgx_nerrors = 0;

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

void *xcalloc(size_t n)
{
    void *p = xmalloc(n);
    memset(p, 0, n);
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *xsprintf(const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *buf = xmalloc((size_t)n + 1);
    vsnprintf(buf, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return buf;
}

char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 4096, n = 0;
    char *buf = xmalloc(cap);
    size_t r;
    while ((r = fread(buf + n, 1, cap - n - 1, f)) > 0) {
        n += r;
        if (n + 1 >= cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
    }
    fclose(f);
    buf[n] = '\0';
    if (len)
        *len = n;
    return buf;
}

static void vreport(const char *file, int line, int col, const char *fmt, va_list ap)
{
    fprintf(stderr, "%s:%d:%d: error: ", file, line, col);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    rgx_nerrors++;
}

void rgx_error(const char *file, int line, int col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vreport(file, line, col, fmt, ap);
    va_end(ap);
}

void rgx_die(const char *file, int line, int col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vreport(file, line, col, fmt, ap);
    va_end(ap);
    exit(1);
}
