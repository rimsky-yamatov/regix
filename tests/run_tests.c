/*
 * Regix test runner (plain C, no scripting language needed).
 *
 *   run_tests <rgc> <cases-dir> [<rgfmt> <fmt-dir> [<multi-dir>]]
 *
 * Each .rgx file in the cases dir carries directives in `//` comments:
 *   // expect-exit: N       compile, run, and require exit code N
 *   // expect-error: TEXT   compilation must fail and print TEXT
 *   // expect-stdout: TEXT  (with expect-exit) program output must contain TEXT
 *   // args: A B            (with expect-exit) command-line arguments to run with
 *   // stdin: TEXT         (with expect-exit) text fed to standard input (\n and \r escapes)
 *   // expect-gas: TEXT     `--target linux-x64 -S` output must contain TEXT
 *   // expect-masm: TEXT    `--target windows-x64 -S` output must contain TEXT
 *
 * Each multi-dir/NAME.d is a directory of .rgx files compiled together; its
 * main.rgx carries the directives.
 * Each fmt-dir/NAME.in is formatted by rgfmt and compared to NAME.out.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define EXE_SUFFIX ".exe"
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#define EXE_SUFFIX ""
#endif

static const char *TMP = "build/tmp";
static int passed, failed;

static char *native(const char *s)
{
    char *d = xstrdup(s);
#ifdef _WIN32
    for (char *p = d; *p; p++)
        if (*p == '/')
            *p = '\\';
#endif
    return d;
}

static int run_cmd(const char *cmd)
{
    char *n = native(cmd);
    int rc = system(n);
    free(n);
#ifndef _WIN32
    rc = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
    return rc;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Names in `dir` ending with `suffix`, sorted. */
static char **list_dir(const char *dir, const char *suffix, int *count)
{
    char **names = NULL;
    int n = 0, cap = 0;
    size_t sl = strlen(suffix);
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    char *pat = xsprintf("%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const char *nm = fd.cFileName;
#else
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            const char *nm = de->d_name;
#endif
            size_t l = strlen(nm);
            if (l > sl && strcmp(nm + l - sl, suffix) == 0) {
                char *s = xstrdup(nm);
                VEC_PUSH(names, n, cap, s);
            }
#ifdef _WIN32
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
        }
        closedir(d);
    }
#endif
    if (n)
        qsort(names, (size_t)n, sizeof *names, cmp_str);
    *count = n;
    return names;
}

static void report(const char *name, int ok, const char *why)
{
    if (ok) {
        passed++;
        printf("ok   %s\n", name);
    } else {
        failed++;
        printf("FAIL %s: %s\n", name, why);
    }
}

/* Value of the first `// key: value` line, or NULL. */
static char *directive(const char *src, const char *key)
{
    char *pat = xsprintf("// %s:", key);
    const char *p = strstr(src, pat);
    free(pat);
    if (!p)
        return NULL;
    p = strchr(p, ':') + 1;
    while (*p == ' ')
        p++;
    const char *e = p;
    while (*e && *e != '\n' && *e != '\r')
        e++;
    return xstrndup(p, (size_t)(e - p));
}

static int file_contains(const char *path, const char *text)
{
    char *s = read_file(path, NULL);
    int r = s && strstr(s, text) != NULL;
    free(s);
    return r;
}

/* `path` is the argument list for rgc (one or more files); directives are read
 * from `directive_file`. */
static void run_case_impl(const char *rgc, const char *name, const char *path,
                          const char *directive_file)
{
    char *src = read_file(directive_file, NULL);
    if (!src) {
        report(name, 0, "cannot read case");
        return;
    }
    char *exit_s = directive(src, "expect-exit");
    char *err_s = directive(src, "expect-error");
    char *gas_s = directive(src, "expect-gas");
    char *masm_s = directive(src, "expect-masm");
    char *out_s = directive(src, "expect-stdout");
    char *args_s = directive(src, "args");
    char *stdin_s = directive(src, "stdin");
    char *log = xsprintf("%s/%s.log", TMP, name);

    if (exit_s) {
        char *exe = xsprintf("%s/%s%s", TMP, name, EXE_SUFFIX);
        char *cmd = xsprintf("%s %s -o %s > %s 2>&1", rgc, path, exe, log);
        if (run_cmd(cmd) != 0) {
            report(name, 0, "compilation failed (see build/tmp log)");
        } else {
            char *outf = xsprintf("%s/%s.stdout", TMP, name);
            char *runp = args_s ? xsprintf("%s %s", exe, args_s) : xstrdup(exe);
            if (stdin_s) {
                char *inf = xsprintf("%s/%s.stdin", TMP, name);
                FILE *sf = fopen(inf, "wb");
                if (sf) {
                    for (const char *q = stdin_s; *q; q++) {
                        if (q[0] == '\\' && (q[1] == 'n' || q[1] == 'r')) {
                            fputc(q[1] == 'n' ? '\n' : '\r', sf);
                            q++;
                        } else {
                            fputc(*q, sf);
                        }
                    }
                    fclose(sf);
                }
                runp = xsprintf("%s < %s", runp, inf);
            }
            char *rcmd = out_s ? xsprintf("%s > %s 2>&1", runp, outf) : runp;
            int rc = run_cmd(rcmd);
            int want = atoi(exit_s);
            if (rc == want && out_s && !file_contains(outf, out_s)) {
                report(name, 0, "stdout does not contain the expected text");
            } else if (rc == want) {
                report(name, 1, "");
            } else {
                char *why = xsprintf("exit code %d, expected %d", rc, want);
                report(name, 0, why);
            }
        }
    } else if (err_s) {
        char *cmd = xsprintf("%s %s -o %s/%s.bin > %s 2>&1", rgc, path, TMP, name, log);
        int rc = run_cmd(cmd);
        if (rc == 0)
            report(name, 0, "expected a compile error but compilation succeeded");
        else if (!file_contains(log, err_s))
            report(name, 0, "error message does not contain the expected text");
        else
            report(name, 1, "");
    } else if (gas_s || masm_s) {
        const char *want = gas_s ? gas_s : masm_s;
        const char *target = gas_s ? "linux-x64" : "windows-x64";
        char *out = xsprintf("%s/%s.out.asm", TMP, name);
        char *cmd = xsprintf("%s %s --target %s -S -o %s > %s 2>&1", rgc, path, target, out, log);
        if (run_cmd(cmd) != 0)
            report(name, 0, "assembly generation failed");
        else if (!file_contains(out, want))
            report(name, 0, "assembly does not contain the expected text");
        else
            report(name, 1, "");
    } else {
        report(name, 0, "no expect-* directive");
    }
}

static void run_case(const char *rgc, const char *dir, const char *file)
{
    char *path = xsprintf("%s/%s", dir, file);
    char *name = xstrndup(file, strlen(file) - 4);
    run_case_impl(rgc, name, path, path);
}

/* A directory NAME.d holding several .rgx files that are compiled together;
 * directives live in its main.rgx. */
static void run_multi_case(const char *rgc, const char *dir, const char *dname)
{
    char *sub = xsprintf("%s/%s", dir, dname);
    char *name = xsprintf("multi_%.*s", (int)(strlen(dname) - 2), dname);
    int n;
    char **files = list_dir(sub, ".rgx", &n);
    char *list = xstrdup("");
    for (int i = 0; i < n; i++)
        list = xsprintf("%s%s%s/%s", list, i ? " " : "", sub, files[i]);
    char *main_path = xsprintf("%s/main.rgx", sub);
    run_case_impl(rgc, name, list, main_path);
}

static void run_fmt_case(const char *rgfmt, const char *dir, const char *file)
{
    char *base = xstrndup(file, strlen(file) - 3); /* strip ".in" */
    char *in = xsprintf("%s/%s.in", dir, base);
    char *want_path = xsprintf("%s/%s.out", dir, base);
    char *got_path = xsprintf("%s/fmt_%s.txt", TMP, base);
    char *cmd = xsprintf("%s %s > %s", rgfmt, in, got_path);
    char *name = xsprintf("fmt/%s", base);
    if (run_cmd(cmd) != 0) {
        report(name, 0, "rgfmt failed");
        return;
    }
    char *want = read_file(want_path, NULL);
    char *got = read_file(got_path, NULL);
    report(name, want && got && strcmp(want, got) == 0, "formatted output differs");
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 5 && argc != 6) {
        fputs("usage: run_tests <rgc> <cases-dir> [<rgfmt> <fmt-dir> [<multi-dir>]]\n", stderr);
        return 2;
    }
#ifdef _WIN32
    _mkdir("build");
    _mkdir(TMP);
#else
    mkdir("build", 0777);
    mkdir(TMP, 0777);
#endif
    int n;
    char **cases = list_dir(argv[2], ".rgx", &n);
    for (int i = 0; i < n; i++)
        run_case(argv[1], argv[2], cases[i]);
    if (argc == 6) {
        int d;
        char **dirs = list_dir(argv[5], ".d", &d);
        for (int i = 0; i < d; i++)
            run_multi_case(argv[1], argv[5], dirs[i]);
    }
    if (argc >= 5) {
        int m;
        char **fmts = list_dir(argv[4], ".in", &m);
        for (int i = 0; i < m; i++)
            run_fmt_case(argv[3], argv[4], fmts[i]);
    }
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
