/*
 * rgpm: Regix package manager (v0.1).
 *
 *   rgpm init <name>   create <name>/rgpm.toml and <name>/src/main.rgx
 *   rgpm build         compile src/main.rgx into build/<name>
 *   rgpm run           build, then run the program
 *
 * rgpm.toml (only these keys are read so far):
 *   name = "hello"
 *   version = "0.1.0"
 *
 * Dependency resolution (local path and git dependencies) is planned.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "version.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define SEP "\\"
#define EXE ".exe"
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0777)
#define SEP "/"
#define EXE ""
#endif

static void usage(FILE *f)
{
    fputs("usage: rgpm <command>\n"
          "  init <name>   create a new package in ./<name>\n"
          "  build         compile src/main.rgx into build/<name>\n"
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

static char *build_package(void)
{
    char *name = package_name();
    if (!name)
        return NULL;
    MKDIR("build");
    char *cmd = xsprintf("rgc src" SEP "main.rgx -o build" SEP "%s" EXE, name);
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
