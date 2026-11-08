/*
 * rgpm: Regix package manager (v0.1).
 *
 *   rgpm init <name>   create <name>/rgpm.toml and <name>/src/main.rgx
 *   rgpm build         compile every .rgx under src/ into build/<name>
 *   rgpm run           build, then run the program
 *
 * rgpm.toml (only these keys are read so far):
 *   name = "hello"
 *   version = "0.1.0"
 *
 * Layout: src/main.rgx has no package; src/a/b/x.rgx must declare
 * "package a::b;". Dependency resolution (local path and git dependencies) is planned.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "version.h"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#define SEP "\\"
#define EXE ".exe"
#else
#include <dirent.h>
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0777)
#define SEP "/"
#define EXE ""
#endif

static void usage(FILE *f)
{
    fputs("usage: rgpm <command>\n"
          "  init <name>   create a new package in ./<name>\n"
          "  build         compile all of src/ into build/<name>\n"
          "  run           build and run\n"
          "  version, help\n",
          f);
}

static int valid_name(const char *s)
{
    if (!*s || !(isalpha((unsigned char)*s) || *s == '_'))
        return 0;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '-'))
            return 0;
    return 1;
}

static int write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "rgpm: cannot write '%s'\n", path);
        return 0;
    }
    fputs(text, f);
    fclose(f);
    return 1;
}

static int cmd_init(const char *name)
{
    if (!valid_name(name)) {
        fprintf(stderr, "rgpm: invalid package name '%s'\n", name);
        return 2;
    }
    char *src_dir = xsprintf("%s" SEP "src", name);
    if (MKDIR(name) != 0) {
        fprintf(stderr, "rgpm: cannot create '%s' (already exists?)\n", name);
        return 1;
    }
    MKDIR(src_dir);
    char *toml = xsprintf("name = \"%s\"\nversion = \"0.1.0\"\n", name);
    char *toml_path = xsprintf("%s" SEP "rgpm.toml", name);
    char *main_path = xsprintf("%s" SEP "src" SEP "main.rgx", name);
    if (!write_text(toml_path, toml))
        return 1;
    if (!write_text(main_path,
                    "import std::io;\n\n"
                    "fn main() -> reg i32 {\n"
                    "    std::io::print_i32(42)\n"
                    "    return(0)\n"
                    "}\n"))
        return 1;
    printf("created package '%s'\n", name);
    return 0;
}

/* Value of `key = "value"` in rgpm.toml, or NULL. */
static char *toml_string(const char *text, const char *key)
{
    size_t kl = strlen(key);
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (strncmp(p, key, kl) == 0) {
            const char *q = p + kl;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q == '=') {
                q++;
                while (*q == ' ' || *q == '\t')
                    q++;
                if (*q == '"') {
                    q++;
                    const char *e = strchr(q, '"');
                    if (e)
                        return xstrndup(q, (size_t)(e - q));
                }
            }
        }
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
    return NULL;
}

static char *package_name(void)
{
    char *text = read_file("rgpm.toml", NULL);
    if (!text) {
        fputs("rgpm: rgpm.toml not found (run 'rgpm init <name>' first)\n", stderr);
        return NULL;
    }
    char *name = toml_string(text, "name");
    if (!name || !valid_name(name)) {
        fputs("rgpm: rgpm.toml needs a valid name = \"...\"\n", stderr);
        return NULL;
    }
    return name;
}

/* ---- source discovery ---- */

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Collect every .rgx under `dir` (recursively), as paths starting with dir. */
static void collect(const char *dir, char ***files, int *n, int *cap)
{
    char **names = NULL;
    int nn = 0, ncap = 0;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    char *pat = xsprintf("%s/*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        char *nm = xstrdup(fd.cFileName);
        VEC_PUSH(names, nn, ncap, nm);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char *nm = xstrdup(de->d_name);
        VEC_PUSH(names, nn, ncap, nm);
    }
    closedir(d);
#endif
    if (nn)
        qsort(names, (size_t)nn, sizeof *names, cmp_str);
    for (int i = 0; i < nn; i++) {
        const char *nm = names[i];
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
            continue;
        char *path = xsprintf("%s/%s", dir, nm);
        size_t l = strlen(nm);
        if (l > 4 && strcmp(nm + l - 4, ".rgx") == 0) {
            VEC_PUSH(*files, *n, *cap, path);
        } else if (!strchr(nm, '.')) {
            collect(path, files, n, cap); /* a directory (names without dots) */
        }
    }
}

/* "package a::b;" declared at the top of `text`, or NULL. */
static char *declared_package(const char *text)
{
    const char *p = text;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        break;
    }
    if (strncmp(p, "package", 7) != 0 || !(p[7] == ' ' || p[7] == '\t'))
        return NULL;
    p += 7;
    while (*p == ' ' || *p == '\t')
        p++;
    const char *e = p;
    while (*e && *e != ';' && *e != '\n' && *e != ' ' && *e != '\r')
        e++;
    return xstrndup(p, (size_t)(e - p));
}

/* Package implied by src/a/b/file.rgx is "a::b"; files directly in src/ have none. */
static char *expected_package(const char *path)
{
    const char *rel = path + strlen("src/");
    const char *slash = strrchr(rel, '/');
    if (!slash)
        return NULL;
    char *dir = xstrndup(rel, (size_t)(slash - rel));
    size_t j = 0;
    char *out = xmalloc(strlen(dir) * 2 + 1);
    for (size_t i = 0; dir[i]; i++) {
        if (dir[i] == '/') {
            out[j++] = ':';
            out[j++] = ':';
        } else {
            out[j++] = dir[i];
        }
    }
    out[j] = '\0';
    return out;
}

static int same_opt(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    return strcmp(a, b) == 0;
}

static char *build_package(void)
{
    char *name = package_name();
    if (!name)
        return NULL;
    char **files = NULL;
    int n = 0, cap = 0;
    collect("src", &files, &n, &cap);
    if (n == 0) {
        fputs("rgpm: no .rgx files under src/\n", stderr);
        return NULL;
    }
    char *cmd = xstrdup("rgc");
    int bad = 0;
    for (int i = 0; i < n; i++) {
        char *text = read_file(files[i], NULL);
        if (!text) {
            fprintf(stderr, "rgpm: cannot read '%s'\n", files[i]);
            return NULL;
        }
        char *have = declared_package(text);
        char *want = expected_package(files[i]);
        if (!same_opt(have, want)) {
            fprintf(stderr, "rgpm: %s declares package '%s' but its directory implies '%s'\n",
                    files[i], have ? have : "(none)", want ? want : "(none)");
            bad = 1;
        }
        cmd = xsprintf("%s %s", cmd, files[i]);
    }
    if (bad)
        return NULL;
    MKDIR("build");
    cmd = xsprintf("%s -o build/%s" EXE, cmd, name);
    if (system(cmd) != 0)
        return NULL;
    return xsprintf("build" SEP "%s" EXE, name);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    const char *cmd = argv[1];
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        printf("rgpm %s\n", RGX_VERSION);
        return 0;
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "init") == 0) {
        if (argc != 3) {
            usage(stderr);
            return 2;
        }
        return cmd_init(argv[2]);
    }
    if (strcmp(cmd, "build") == 0)
        return build_package() ? 0 : 1;
    if (strcmp(cmd, "run") == 0) {
        char *exe = build_package();
        if (!exe)
            return 1;
        return system(exe) == 0 ? 0 : 1;
    }
    fprintf(stderr, "rgpm: unknown command '%s'\n", cmd);
    usage(stderr);
    return 2;
}
