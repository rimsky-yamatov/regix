#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abi.h"
#include "check.h"
#include "codegen.h"
#include "parser.h"
#include "util.h"
#include "version.h"

static void usage(FILE *f)
{
    fputs("usage: rgc [options] <file.rgx>\n"
          "  -o <path>          output path (executable, or assembly with -S)\n"
          "  -S                 stop after writing assembly\n"
          "  --target <t>       linux-x64 | windows-x64 (default: host)\n"
          "  --keep-asm         keep the intermediate assembly file\n"
          "  --version, --help\n",
          f);
}

static int run(const char *cmd)
{
    int rc = system(cmd);
    if (rc != 0)
        fprintf(stderr, "rgc: command failed: %s\n", cmd);
    return rc == 0;
}

static char *stem_of(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);
    return xstrndup(base, n);
}

static int ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL;
    const Abi *abi = abi_host();
    int asm_only = 0, keep_asm = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (strcmp(a, "-S") == 0) {
            asm_only = 1;
        } else if (strcmp(a, "--keep-asm") == 0) {
            keep_asm = 1;
        } else if (strcmp(a, "--target") == 0 && i + 1 < argc) {
            abi = abi_by_name(argv[++i]);
            if (!abi) {
                fprintf(stderr, "rgc: unknown target '%s'\n", argv[i]);
                return 2;
            }
        } else if (strcmp(a, "--version") == 0) {
            printf("rgc %s\n", RGX_VERSION);
            return 0;
        } else if (strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (a[0] == '-') {
            fprintf(stderr, "rgc: unknown option '%s'\n", a);
            usage(stderr);
            return 2;
        } else if (!input) {
            input = a;
        } else {
            fprintf(stderr, "rgc: more than one input file\n");
            return 2;
        }
    }
    if (!input) {
        usage(stderr);
        return 2;
    }
    if (!asm_only && abi != abi_host()) {
        fprintf(stderr, "rgc: cross-target builds need -S (host is %s)\n", abi_host()->name);
        return 2;
    }

    char *src = read_file(input, NULL);
    if (!src) {
        fprintf(stderr, "rgc: cannot read '%s'\n", input);
        return 1;
    }

    Program *prog = parse_program(input, src);
    if (check_program(prog, abi, 1) > 0)
        return 1;

    const char *asm_ext = abi->windows ? ".asm" : ".s";
    char *exe = NULL, *asm_path;
    if (asm_only) {
        asm_path = output ? xstrdup(output) : xsprintf("%s%s", stem_of(input), asm_ext);
    } else {
        exe = output ? xstrdup(output) : stem_of(input);
        if (abi->windows && !ends_with(exe, ".exe"))
            exe = xsprintf("%s.exe", exe);
        asm_path = xsprintf("%s%s", exe, asm_ext);
    }

    FILE *f = fopen(asm_path, "wb");
    if (!f) {
        fprintf(stderr, "rgc: cannot write '%s'\n", asm_path);
        return 1;
    }
    codegen_program(prog, abi, f);
    fclose(f);
    if (asm_only)
        return 0;

    char *obj = xsprintf("%s.%s", exe, abi->windows ? "obj" : "o");
    int ok;
    if (abi->windows) {
        char *c1 = xsprintf("ml64 /nologo /c /Fo%s %s", obj, asm_path);
        char *c2 = xsprintf("link /nologo /subsystem:console /entry:mainCRTStartup "
                            "/nodefaultlib /out:%s %s kernel32.lib", exe, obj);
        ok = run(c1) && run(c2);
    } else {
        char *c1 = xsprintf("as --64 %s -o %s", asm_path, obj);
        char *c2 = xsprintf("ld %s -o %s", obj, exe);
        ok = run(c1) && run(c2);
    }
    remove(obj);
    if (!keep_asm)
        remove(asm_path);
    return ok ? 0 : 1;
}
