/*
 * main.c — the odin3 command-line driver. The only place exit codes and
 * process-level output policy live (spec §15.1: no exit() outside cli/).
 */
#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "util/hash.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Process exit codes: success, a failed pass or script, a command-line error. */
enum { EXIT_OK = 0, EXIT_FAIL = 1, EXIT_USAGE = 2 };

static void print_usage(FILE *out) {
    (void)fputs("usage: odin3 [options] [-p \"pass args; pass args\"]... [script.o3]...\n"
                "\n"
                "Runs pass scripts on one design, in command-line order. A script holds\n"
                "commands separated by ';' or newlines; '#' starts a comment.\n"
                "  --version        print the version and ABI version\n"
                "  --help           print this help and the registered passes\n"
                "  --plugin FILE    load a shared-object plugin (repeatable)\n"
                "  --top NAME       the top module (wins over the BLIF first model)\n"
                "  --check          check the IR before and after every pass (always in Debug)\n"
                "  -p SCRIPT        run the passes given inline\n"
                "\n"
                "passes:\n",
                out);
    for (uint32_t i = 0; i < odin3_pass_count(); i++) {
        (void)fprintf(out, "  %s\n", odin3_pass_at(i)->help);
    }
}

static int load_plugin(const char *path) {
    odin3_status status = odin3_plugin_load(path);
    if (status != ODIN3_OK) {
        (void)fprintf(stderr, "odin3: cannot load plugin %s: %s\n", path,
                      odin3_status_string(status));
        return EXIT_FAIL;
    }
    (void)printf("odin3: loaded plugin %s\n", path);
    return EXIT_OK;
}

/* What the first argument scan found. */
typedef struct cli_state {
    odin3_pass_options opts;
    bool scripts; /* a -p or a script file is given */
} cli_state;

/* An option that takes a value: true (and *value) when argv[*at] is one; advances *at. */
static bool option_value(char **argv, int argc, int *at, const char **value) {
    const char *arg = argv[*at];
    bool takes =
        strcmp(arg, "--plugin") == 0 || strcmp(arg, "--top") == 0 || strcmp(arg, "-p") == 0;
    if (!takes || *at + 1 >= argc) {
        return false;
    }
    (*at)++;
    *value = argv[*at];
    return true;
}

static int usage_error(const char *arg) {
    (void)fprintf(stderr, "odin3: unknown or incomplete argument: %s\n", arg);
    print_usage(stderr);
    return EXIT_USAGE;
}

/* Handles one option of the first scan (loads plugins, records options). */
static int scan_one(char **argv, int argc, int *at, cli_state *state) {
    const char *arg = argv[*at];
    const char *value = NULL;
    if (strcmp(arg, "--version") == 0) {
        (void)printf("odin3 %s (ABI %u)\n", odin3_version_string(), odin3_abi_version());
    } else if (strcmp(arg, "--help") == 0) {
        print_usage(stdout);
    } else if (strcmp(arg, "--check") == 0) {
        state->opts.check = true;
    } else if (option_value(argv, argc, at, &value)) {
        if (strcmp(arg, "--plugin") == 0) {
            return load_plugin(value);
        }
        state->scripts |= strcmp(arg, "-p") == 0;
        state->opts.top = strcmp(arg, "--top") == 0 ? value : state->opts.top;
    } else if (arg[0] == '-') {
        return usage_error(arg);
    } else {
        state->scripts = true;
    }
    return EXIT_OK;
}

/* Runs every -p and script file in command-line order on one design. */
static int run_scripts(char **argv, int argc) {
    odin3_design *design = odin3_design_create();
    if (design == NULL) {
        (void)fprintf(stderr, "odin3: out of memory\n");
        return EXIT_FAIL;
    }
    odin3_status st = ODIN3_OK;
    for (int i = 1; st == ODIN3_OK && i < argc; i++) {
        const char *value = NULL;
        const char *arg = argv[i];
        if (option_value(argv, argc, &i, &value)) {
            st = strcmp(arg, "-p") != 0
                     ? ODIN3_OK
                     : odin3_pass_run_script(design, odin3_bytes_cstr(value),
                                             (odin3_script_src){"-p", ODIN3_SCRIPT_BY_COMMAND});
        } else if (arg[0] != '-') {
            st = odin3_pass_run_script_file(design, arg);
        }
    }
    odin3_design_destroy(design);
    return st == ODIN3_OK ? EXIT_OK : EXIT_FAIL;
}

int main(int argc, char **argv) {
    cli_state state = {0};
    for (int i = 1; i < argc; i++) {
        int rc = scan_one(argv, argc, &i, &state);
        if (rc != EXIT_OK) {
            return rc;
        }
    }
    if (argc == 1) {
        print_usage(stdout);
    }
    if (!state.scripts) {
        return EXIT_OK;
    }
    odin3_pass_set_options(state.opts);
    return run_scripts(argv, argc);
}
