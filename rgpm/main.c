/*
 * rgpm: Regix package manager (skeleton).
 *
 * v0.1 only offers `build`, which compiles src/main.rgx with rgc.
 * Dependency resolution (rgpm.toml, local path and git dependencies)
 * is planned.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "version.h"

static void usage(FILE *f)
{
    fputs("usage: rgpm <command>\n"
          "  build      compile src/main.rgx into build/main\n"
          "  version\n"
          "  help\n",
          f);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0) {
        printf("rgpm %s\n", RGX_VERSION);
        return 0;
    }
    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(argv[1], "build") == 0) {
#ifdef _WIN32
        if (system("if not exist build mkdir build") != 0)
            return 1;
        return system("rgc src\\main.rgx -o build\\main") == 0 ? 0 : 1;
#else
        if (system("mkdir -p build") != 0)
            return 1;
        return system("rgc src/main.rgx -o build/main") == 0 ? 0 : 1;
#endif
    }
    fprintf(stderr, "rgpm: unknown command '%s'\n", argv[1]);
    usage(stderr);
    return 2;
}
