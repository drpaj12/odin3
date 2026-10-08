/*
 * main.c — the odin3 command-line driver. The only place exit codes and
 * process-level output policy live (spec §15.1: no exit() outside cli/).
 */
#include "odin3/odin3.h"

#include <stdio.h>
#include <string.h>

static void print_usage(FILE *out) {
    (void)fputs("usage: odin3 [--version] [--help] [--plugin FILE.so]...\n"
                "\n"
                "Phase 0 skeleton: no synthesis yet.\n"
                "  --version        print the version and ABI version\n"
                "  --plugin FILE    load a shared-object plugin (repeatable)\n",
                out);
}

static int load_plugin(const char *path) {
    odin3_status status = odin3_plugin_load(path);
    if (status != ODIN3_OK) {
        (void)fprintf(stderr, "odin3: cannot load plugin %s: %s\n", path,
                      odin3_status_string(status));
        return 1;
    }
    (void)printf("odin3: loaded plugin %s\n", path);
    return 0;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            (void)printf("odin3 %s (ABI %u)\n", odin3_version_string(), odin3_abi_version());
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(stdout);
        } else if (strcmp(argv[i], "--plugin") == 0 && i + 1 < argc) {
            i++;
            if (load_plugin(argv[i]) != 0) {
                return 1;
            }
        } else {
            (void)fprintf(stderr, "odin3: unknown or incomplete argument: %s\n", argv[i]);
            print_usage(stderr);
            return 2;
        }
    }
    if (argc == 1) {
        print_usage(stdout);
    }
    return 0;
}
