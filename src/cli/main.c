/*
 * main.c — the odin3 command-line driver. The only place exit codes and
 * process-level output policy live (spec §15.1: no exit() outside cli/).
 * Uses only the public ABI (odin3.h): it links libodin3, so plugins it loads
 * register into the same pass registry.
 */
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Process exit codes: success, a failed pass or script, a command-line error. */
enum { EXIT_OK = 0, EXIT_FAIL = 1, EXIT_USAGE = 2 };

/* A log message the CLI formats itself (odin3_log_write cuts longer ones the same way). */
enum { MSG_BUF = 1024 };

static void print_usage(FILE *out) {
    (void)fputs("usage: odin3 [options] [-p \"pass args; pass args\"]... [script.o3]...\n"
                "\n"
                "Runs pass scripts on one design, in command-line order, after looking up\n"
                "every pass name in all of them. A script holds commands separated by ';' or\n"
                "newlines; '#' starts a comment; \"double quotes\" keep blanks, ';' and '#'.\n"
                "  --version        print the version and ABI version\n"
                "  --help           print this help and the registered passes; run nothing\n"
                "  --plugin FILE    load a shared-object plugin (repeatable)\n"
                "  --top NAME       the top module (wins over the BLIF first model)\n"
                "  --check          check the IR before and after every pass (always in Debug)\n"
                "  -p SCRIPT        run the passes given inline\n"
                "\n"
                "passes:\n",
                out);
    for (uint32_t i = 0; i < odin3_pass_get_count(); i++) {
        const char *help = "";
        (void)odin3_pass_get_help(i, &help);
        (void)fprintf(out, "  %s\n", help);
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
    bool check;      /* --check */
    const char *top; /* --top NAME, NULL for none */
    bool scripts;    /* a -p or a script file is given */
    bool help;       /* --help: printed after the scan (plugin passes listed), nothing runs */
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
        state->help = true;
    } else if (strcmp(arg, "--check") == 0) {
        state->check = true;
    } else if (option_value(argv, argc, at, &value)) {
        if (strcmp(arg, "--plugin") == 0) {
            return load_plugin(value);
        }
        state->scripts |= strcmp(arg, "-p") == 0;
        state->top = strcmp(arg, "--top") == 0 ? value : state->top;
    } else if (arg[0] == '-') {
        return usage_error(arg);
    } else {
        state->scripts = true;
    }
    return EXIT_OK;
}

/* The -p or script file at argv[*at] (advancing past an option's value): run on design, or only
 * resolved when design is NULL. OK for every other argument. */
static odin3_status script_arg(char **argv, int argc, int *at, odin3_design *design) {
    const char *arg = argv[*at];
    const char *value = NULL;
    if (option_value(argv, argc, at, &value)) {
        if (strcmp(arg, "-p") != 0) {
            return ODIN3_OK;
        }
        odin3_script_src src = {"-p", ODIN3_SCRIPT_BY_COMMAND};
        return design == NULL ? odin3_script_resolve(value, src)
                              : odin3_design_run_script(design, value, src);
    }
    if (arg[0] == '-') {
        return ODIN3_OK;
    }
    return design == NULL ? odin3_script_resolve_file(arg)
                          : odin3_design_run_script_file(design, arg);
}

/* Every -p and script file in command-line order (see script_arg); stops at the first failure. */
static odin3_status each_script(char **argv, int argc, odin3_design *design) {
    odin3_status st = ODIN3_OK;
    for (int i = 1; st == ODIN3_OK && i < argc; i++) {
        st = script_arg(argv, argc, &i, design);
    }
    return st;
}

/* A --top that no pass applied (no read_blif or hierarchy ran) is reported, not ignored. */
static void warn_unapplied_top(const odin3_design *design, const char *top) {
    uint32_t id = 0;
    const char *name = NULL;
    if (odin3_design_get_top_module(design, &id) == ODIN3_OK && id != 0) {
        (void)odin3_module_get_name(design, id, &name);
    }
    if (top != NULL && (name == NULL || strcmp(name, top) != 0)) {
        char msg[MSG_BUF];
        (void)snprintf(msg, sizeof msg, "--top %s was not applied (no read_blif or hierarchy ran)",
                       top);
        (void)odin3_log_write(ODIN3_LOG_WARN, msg);
    }
}

/* Resolves every -p and script file (a typo anywhere costs nothing), then runs them in
 * command-line order on one design. */
static int run_scripts(char **argv, int argc, const char *top) {
    if (each_script(argv, argc, NULL) != ODIN3_OK) {
        return EXIT_FAIL;
    }
    odin3_design *design = odin3_design_create();
    if (design == NULL) {
        (void)fprintf(stderr, "odin3: out of memory\n");
        return EXIT_FAIL;
    }
    odin3_status st = each_script(argv, argc, design);
    if (st == ODIN3_OK) {
        warn_unapplied_top(design, top);
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
    if (argc == 1 || state.help) {
        print_usage(stdout);
    }
    if (!state.scripts || state.help) {
        return EXIT_OK;
    }
    odin3_pass_set_check(state.check);
    if (odin3_pass_set_top(state.top) != ODIN3_OK) {
        (void)fprintf(stderr, "odin3: out of memory\n");
        return EXIT_FAIL;
    }
    return run_scripts(argv, argc, state.top);
}
