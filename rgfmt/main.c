/*
 * rgfmt: minimal Regix formatter.
 *
 * v0.1 re-indents by brace depth (4 spaces), trims trailing whitespace,
 * collapses runs of blank lines and ensures a final newline. It works on
 * lines, so comments are always preserved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "version.h"

static char *format(const char *src)
{
    size_t cap = strlen(src) * 2 + 64, n = 0;
    char *out = xmalloc(cap);
    int depth = 0, blank_pending = 0, any = 0;
    const char *p = src;

    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        const char *line = p;
        p += len + (eol ? 1 : 0);

        while (len && (*line == ' ' || *line == '\t')) { line++; len--; }
        while (len && (line[len - 1] == ' ' || line[len - 1] == '\t' || line[len - 1] == '\r'))
            len--;

        if (len == 0) {
            if (any)
                blank_pending = 1;
            continue;
        }

        /* count braces outside of a trailing // comment */
        int opens = 0, closes = 0;
        for (size_t i = 0; i < len; i++) {
            if (line[i] == '/' && i + 1 < len && line[i + 1] == '/')
                break;
            if (line[i] == '{') opens++;
            if (line[i] == '}') closes++;
        }
        int indent = depth - (line[0] == '}' ? 1 : 0);
        if (indent < 0)
            indent = 0;

        size_t need = n + (size_t)indent * 4 + len + 4;
        if (need >= cap) {
            while (need >= cap) cap *= 2;
            out = xrealloc(out, cap);
        }
        if (blank_pending) {
            out[n++] = '\n';
            blank_pending = 0;
        }
        for (int i = 0; i < indent * 4; i++)
            out[n++] = ' ';
        memcpy(out + n, line, len);
        n += len;
        out[n++] = '\n';
        any = 1;
        depth += opens - closes;
        if (depth < 0)
            depth = 0;
    }
    out[n] = '\0';
    return out;
}

int main(int argc, char **argv)
{
    int write_back = 0, check = 0;
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0) write_back = 1;
        else if (strcmp(argv[i], "--check") == 0) check = 1;
        else if (strcmp(argv[i], "--version") == 0) { printf("rgfmt %s\n", RGX_VERSION); return 0; }
        else if (argv[i][0] == '-') { fprintf(stderr, "rgfmt: unknown option '%s'\n", argv[i]); return 2; }
        else path = argv[i];
    }
    if (!path) {
        fputs("usage: rgfmt [-w | --check] <file.rgx>\n", stderr);
        return 2;
    }
    char *src = read_file(path, NULL);
    if (!src) {
        fprintf(stderr, "rgfmt: cannot read '%s'\n", path);
        return 1;
    }
    char *out = format(src);
    if (check)
        return strcmp(src, out) == 0 ? 0 : 1;
    if (write_back) {
        FILE *f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "rgfmt: cannot write '%s'\n", path);
            return 1;
        }
        fputs(out, f);
        fclose(f);
    } else {
        fputs(out, stdout);
    }
    return 0;
}
