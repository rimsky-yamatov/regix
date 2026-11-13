#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* readlink */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "abi.h"
#include "check.h"
#include "codegen.h"
#include "parser.h"
#include "util.h"
#include "version.h"

static void usage(FILE *f)
{
    fputs("usage: rgc [options] <file.rgx>...\n"
          "  -o <path>          output path (executable, or assembly with -S)\n"
          "  -S                 stop after writing assembly\n"
          "  --target <t>       linux-x64 | windows-x64 (default: host)\n"
          "  --keep-asm         keep the intermediate assembly file\n"
          "  --version, --help\n",
          f);
}

#ifdef _WIN32
#include <stdio.h>

/* True if `name` is found on PATH. */
static int have_tool(const char *name)
{
    char *c = xsprintf("where %s >nul 2>&1", name);
    return system(c) == 0;
}

/* "call vcvars64.bat && " for the newest Visual Studio with the C++ tools, or "". */
static char *vs_env_prefix(void)
{
    const char *pf = getenv("ProgramFiles(x86)");
    if (!pf)
        return xstrdup("");
    char *cmd = xsprintf("\"\"%s\\Microsoft Visual Studio\\Installer\\vswhere.exe\" -latest -products * "
                         "-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
                         "-property installationPath\"", pf);
    FILE *p = _popen(cmd, "r");
    if (!p)
        return xstrdup("");
    char line[1024];
    int ok = fgets(line, sizeof line, p) != NULL;
    _pclose(p);
    if (!ok)
        return xstrdup("");
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = '\0';
    if (n == 0)
        return xstrdup("");
    return xsprintf("call \"%s\\VC\\Auxiliary\\Build\\vcvars64.bat\" >nul && ", line);
}
#endif

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

/* Directory holding the Regix sources of the standard library: $RGX_STD, or
 * `std` next to (or one level above) the compiler executable. */
static char *find_std_dir(const char *argv0)
{
    const char *env = getenv("RGX_STD");
    if (env && *env)
        return xstrdup(env);
    char buf[1024];
    size_t n = 0;
#ifdef _WIN32
    n = (size_t)GetModuleFileNameA(NULL, buf, sizeof buf - 1);
#else
    ssize_t r = readlink("/proc/self/exe", buf, sizeof buf - 1);
    n = r > 0 ? (size_t)r : 0;
#endif
    if (n == 0) {
        n = strlen(argv0) < sizeof buf - 1 ? strlen(argv0) : sizeof buf - 1;
        memcpy(buf, argv0, n);
    }
    buf[n] = '\0';
    size_t cut = n;
    while (cut > 0 && buf[cut - 1] != '/' && buf[cut - 1] != '\\')
        cut--;
    char *dir = cut ? xstrndup(buf, cut - 1) : xstrdup(".");
    const char *rel[] = {"/std", "/../std"};
    for (int i = 0; i < 2; i++) {
        char *cand = xsprintf("%s%s", dir, rel[i]);
        char *probe = xsprintf("%s/math.rgx", cand);
        char *t = read_file(probe, NULL);
        if (t)
            return cand;
    }
    return NULL;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

int main(int argc, char **argv)
{
    const char *inputs[64];
    int ninputs = 0;
    const char *output = NULL;
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
        } else if (ninputs < 64) {
            inputs[ninputs++] = a;
        } else {
            fprintf(stderr, "rgc: too many input files\n");
            return 2;
        }
    }
    if (ninputs == 0) {
        usage(stderr);
        return 2;
    }
    if (!asm_only && abi != abi_host()) {
        fprintf(stderr, "rgc: cross-target builds need -S (host is %s)\n", abi_host()->name);
        return 2;
    }

    Program **files = NULL;
    int nfiles = 0, fcap = 0;
    for (int i = 0; i < ninputs; i++) {
        char *src = read_file(inputs[i], NULL);
        if (!src) {
            fprintf(stderr, "rgc: cannot read '%s'\n", inputs[i]);
            return 1;
        }
        Program *u = parse_program(inputs[i], src);
        VEC_PUSH(files, nfiles, fcap, u);
    }
    /* load the std::* modules that the program (and those modules) import */
    char *std_dir = NULL;
    for (int i = 0; i < nfiles; i++) {
        for (int k = 0; k < files[i]->nimports; k++) {
            const char *im = files[i]->imports[k];
            if (strncmp(im, "std::", 5) != 0 || is_builtin_module(im))
                continue;
            int loaded = 0;
            for (int j = 0; j < nfiles && !loaded; j++)
                loaded = files[j]->package && strcmp(files[j]->package, im) == 0;
            if (loaded)
                continue;
            if (!std_dir) {
                std_dir = find_std_dir(argv[0]);
                if (!std_dir) {
                    fprintf(stderr, "rgc: cannot find the standard library directory "
                                    "(set RGX_STD to the `std` folder)\n");
                    return 1;
                }
            }
            char *rel = xstrdup(im + 5);
            for (char *q = rel; *q; q++)
                if (*q == ':')
                    *q = '/';
            char *rel2 = xmalloc(strlen(rel) + 1);
            size_t o = 0;
            for (size_t q = 0; rel[q]; q++)
                if (!(rel[q] == '/' && rel[q + 1] == '/'))
                    rel2[o++] = rel[q];
            rel2[o] = '\0';
            char *path = xsprintf("%s/%s.rgx", std_dir, rel2);
            char *src = read_file(path, NULL);
            if (!src) {
                fprintf(stderr, "%s:%d:%d: error: standard library module '%s' not found in %s\n",
                        files[i]->file, 1, 1, im, std_dir);
                return 1;
            }
            Program *u = parse_program(path, src);
            VEC_PUSH(files, nfiles, fcap, u);
        }
    }
    const char *input = inputs[0];
    Program *prog = link_units(files, nfiles);
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
#ifdef _WIN32
        char *prefix = have_tool("ml64") ? xstrdup("") : vs_env_prefix();
        if (prefix[0]) {
            /* ml64 is not on PATH: set up Visual Studio's environment in the same shell */
            char *all = xsprintf("\"%s%s && %s\"", prefix, c1, c2);
            ok = run(all);
        } else {
            ok = run(c1) && run(c2);
        }
#else
        ok = run(c1) && run(c2);
#endif
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
