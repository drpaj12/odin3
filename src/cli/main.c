/*
 * main.c — the odin3 command-line driver. The only place exit codes and
 * process-level output policy live (spec §15.1: no exit() outside cli/).
 * Uses only the public ABI (odin3.h): it links libodin3, so plugins it loads
 * register into the same pass registry. With ODIN3_HAVE_ABC it also links the
 * linked-ABC adapter (libodin3_abc.so, odin3_abc.h) and registers its engine at
 * start-up, before any --plugin (3D spec §4.3). With ODIN3_HAVE_SLANG it links
 * the SystemVerilog adapter (libodin3_slang.so, odin3_slang.h) and registers its
 * odin3_read_slang as the read_verilog pass's SV reader (DESIGN §4.2, ABI v6).
 */
#include "odin3/odin3.h"

#ifdef ODIN3_HAVE_ABC
#include "odin3_abc.h"
#endif

#ifdef ODIN3_HAVE_SLANG
#include "odin3_slang.h"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Process exit codes: success, a failed pass or script, a command-line error. */
enum { EXIT_OK = 0, EXIT_FAIL = 1, EXIT_USAGE = 2 };

/* A log message the CLI formats itself (odin3_log_write cuts longer ones the same way). */
enum { MSG_BUF = 1024 };

/* read_project's arguments for a --project or -f path: the path (PATH_MAX on Linux) quoted,
 * after "--format f ". */
enum { PROJECT_ARGS_BUF = 4096 + 32 };

static void print_usage(FILE *out) {
    (void)fputs("usage: odin3 [options] [-p \"pass args; pass args\"]... [script.o3]...\n"
                "       odin3 [options] --parse-only [file.v]...\n"
                "       odin3 [options] --elab-only [file.v]...\n"
                "\n"
                "Runs pass scripts on one design, in command-line order, after looking up\n"
                "every pass name in all of them. A script holds commands separated by ';' or\n"
                "newlines; '#' starts a comment; \"double quotes\" keep blanks, ';' and '#'.\n"
                "  --version        print the version and ABI version\n"
                "  --help           print this help and the registered passes; run nothing\n"
                "  --plugin FILE    load a shared-object plugin (repeatable)\n"
                "  --top NAME       the top module (wins over the BLIF first model)\n"
                "  --check          check the IR before and after every pass (always in Debug)\n"
                "  --keep-ast       keep the elaborated AST of every read (write_verilog_ast)\n"
                "  --keep-ast=all   keep the parsed AST too\n"
                "  --project FILE   read a project first (read_project FILE, before every script)\n"
                "  -f LIST          read a file list first (read_project --format f LIST)\n"
                "  --revision R     the Quartus revision every read_project reads\n"
                "  --abc-script FILE the ABC script of every abc pass (below abc --script)\n"
                "  --opt-snapshot DIR write the design before and after every changing opt step\n"
                "                   (DIR must exist: NNNN-<sub>-before.il, -after.il)\n"
                "  --dump-project   write the project record as JSON after the scripts\n"
                "  --diag-paths given|resolved\n"
                "                   name files in diagnostics as given (default) or absolute\n"
                "  -p SCRIPT        run the passes given inline\n"
                "  --parse-only     parse the files given (else the project's) and stop:\n"
                "                   read_verilog --parse-only FILE... (no script allowed)\n"
                "  --elab-only      elaborate the files given (else the project's), then stats:\n"
                "                   read_verilog FILE...; stats (no script allowed)\n"
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
    char msg[MSG_BUF];
    (void)snprintf(msg, sizeof msg, "loaded plugin %s", path);
    (void)odin3_log_write(ODIN3_LOG_INFO, msg);
    return EXIT_OK;
}

/* What the first argument scan found. */
typedef struct cli_state {
    bool check;           /* --check */
    const char *top;      /* --top NAME, NULL for none */
    const char *revision; /* --revision R, NULL for none */
    bool resolved;        /* --diag-paths resolved */
    bool scripts;         /* a script file (or, with --parse-only, a Verilog file) is given */
    bool inline_script;   /* a -p is given */
    bool script_file;     /* a positional argument ending in .o3 (a script) is given */
    bool parse_only;      /* --parse-only: the other arguments are Verilog files, not scripts */
    bool elab_only;       /* --elab-only: likewise; read_verilog elaborates, then stats runs */
    bool project;         /* a --project or -f is given */
    bool dump;            /* --dump-project */
    bool help;            /* --help: printed after the scan (plugin passes listed), nothing runs */
    const char *quoted;   /* a positional argument holding '"' (refused under --parse-only) */
    odin3_keep_ast keep;  /* --keep-ast (ELABORATED), --keep-ast=all (ALL); NONE by default */
    /* --abc-script FILE, NULL for none */
    const char *abc_script;
    bool linked_abc;          /* the linked ABC engine is registered (--version names it) */
    const char *opt_snapshot; /* --opt-snapshot DIR, NULL for none */
} cli_state;

/* The options that take a value. */
static const char *const k_value_options[] = {"--plugin",     "--top",        "-p",
                                              "--project",    "-f",           "--revision",
                                              "--diag-paths", "--abc-script", "--opt-snapshot"};

/* An option that takes a value: true (and *value) when argv[*at] is one; advances *at. */
static bool option_value(char **argv, int argc, int *at, const char **value) {
    const char *arg = argv[*at];
    bool takes = false;
    for (size_t i = 0; i < sizeof k_value_options / sizeof k_value_options[0]; i++) {
        takes |= strcmp(arg, k_value_options[i]) == 0;
    }
    if (!takes || *at + 1 >= argc) {
        return false;
    }
    (*at)++;
    *value = argv[*at];
    return true;
}

/* --project FILE or -f LIST. */
static bool is_project_option(const char *arg) {
    return strcmp(arg, "--project") == 0 || strcmp(arg, "-f") == 0;
}

static int usage_error(const char *arg) {
    (void)fprintf(stderr, "odin3: unknown or incomplete argument: %s\n", arg);
    print_usage(stderr);
    return EXIT_USAGE;
}

static int bad_value(const char *option, const char *value, const char *why) {
    (void)fprintf(stderr, "odin3: %s %s: %s\n", option, value, why);
    print_usage(stderr);
    return EXIT_USAGE;
}

/* --version: version and ABI line, commit line, then the linked ABC's commit if registered. */
static void print_version(const cli_state *state) {
    (void)printf("odin3 %s (ABI %u)\n", odin3_version_string(), odin3_abi_version());
    (void)printf("commit %s%s\n", odin3_version_commit(),
                 odin3_version_commit_dirty() != 0 ? "-dirty" : "");
#ifdef ODIN3_HAVE_SLANG
    (void)printf("SystemVerilog via slang (adapters/slang, third_party/slang)\n");
#endif
#ifdef ODIN3_HAVE_ABC
    if (state->linked_abc) {
        (void)printf("linked ABC %s (adapters/abc/NOTICE)\n", ODIN3_ABC_COMMIT_STR);
    }
#else
    (void)state;
#endif
}

/*
 * Registers the linked ABC engine (3D spec §4.3), before any --plugin can register another. A
 * failure is the warning "linked ABC unavailable: <status>", which the default log sink prints as
 * "odin3: warning: linked ABC unavailable: <status>" (§11's "odin3: " is the sink's, as for the
 * unlocated project diagnostics), and odin3 runs on without linked ABC. True when registered.
 */
static bool init_linked_abc(void) {
#ifdef ODIN3_HAVE_ABC
    odin3_status status = odin3_abc_adapter_init();
    if (status == ODIN3_OK) {
        return true;
    }
    char msg[MSG_BUF];
    (void)snprintf(msg, sizeof msg, "linked ABC unavailable: %s", odin3_status_string(status));
    (void)odin3_log_write(ODIN3_LOG_WARN, msg);
#endif
    return false;
}

/*
 * Registers the linked SystemVerilog reader (DESIGN §4.2): read_verilog hands .sv files to it.
 * A failure is the warning "SystemVerilog reader unavailable: <status>", and reads of .sv files
 * then fail with "no SystemVerilog reader registered". True when registered.
 */
static bool init_linked_slang(void) {
#ifdef ODIN3_HAVE_SLANG
    odin3_status status = odin3_sv_reader_register(odin3_read_slang);
    if (status == ODIN3_OK) {
        return true;
    }
    char msg[MSG_BUF];
    (void)snprintf(msg, sizeof msg, "SystemVerilog reader unavailable: %s",
                   odin3_status_string(status));
    (void)odin3_log_write(ODIN3_LOG_WARN, msg);
#endif
    return false;
}

/* A flag of the first scan: true when arg is one (recorded in state). */
static bool scan_flag(const char *arg, cli_state *state) {
    if (strcmp(arg, "--version") == 0) {
        print_version(state);
    } else if (strcmp(arg, "--help") == 0) {
        state->help = true;
    } else if (strcmp(arg, "--check") == 0) {
        state->check = true;
    } else if (strcmp(arg, "--dump-project") == 0) {
        state->dump = true;
    } else if (strcmp(arg, "--parse-only") == 0) {
        state->parse_only = true;
    } else if (strcmp(arg, "--elab-only") == 0) {
        state->elab_only = true;
    } else if (strcmp(arg, "--keep-ast") == 0 || strcmp(arg, "--keep-ast=all") == 0) {
        state->keep =
            arg[strlen("--keep-ast")] == '=' ? ODIN3_KEEP_AST_ALL : ODIN3_KEEP_AST_ELABORATED;
    } else {
        return false;
    }
    return true;
}

/* True when path names a regular file (symbolic links followed). */
static bool is_regular_file(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

/* True when path names a directory (symbolic links followed). */
static bool is_directory(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

/* An option with its value (loads a plugin, records the rest). */
static int scan_value(const char *arg, const char *value, cli_state *state) {
    if (strcmp(arg, "--plugin") == 0) {
        return load_plugin(value);
    }
    if (strcmp(arg, "--diag-paths") == 0) {
        if (strcmp(value, "given") != 0 && strcmp(value, "resolved") != 0) {
            return bad_value(arg, value, "expects given or resolved");
        }
        state->resolved = strcmp(value, "resolved") == 0;
    }
    if (is_project_option(arg) && strchr(value, '"') != NULL) {
        return bad_value(arg, value, "a path holding '\"' cannot be passed to read_project");
    }
    if (strcmp(arg, "--abc-script") == 0 && !is_regular_file(value)) {
        return bad_value(arg, value, "no such file");
    }
    if (strcmp(arg, "--opt-snapshot") == 0 && !is_directory(value)) {
        return bad_value(arg, value, "no such directory");
    }
    state->inline_script |= strcmp(arg, "-p") == 0;
    state->project |= is_project_option(arg);
    state->top = strcmp(arg, "--top") == 0 ? value : state->top;
    state->revision = strcmp(arg, "--revision") == 0 ? value : state->revision;
    state->abc_script = strcmp(arg, "--abc-script") == 0 ? value : state->abc_script;
    state->opt_snapshot = strcmp(arg, "--opt-snapshot") == 0 ? value : state->opt_snapshot;
    return EXIT_OK;
}

/* Handles one option of the first scan (loads plugins, records options). */
static int scan_one(char **argv, int argc, int *at, cli_state *state) {
    const char *arg = argv[*at];
    const char *value = NULL;
    if (scan_flag(arg, state)) {
        return EXIT_OK;
    }
    if (option_value(argv, argc, at, &value)) {
        return scan_value(arg, value, state);
    }
    if (arg[0] == '-') {
        return usage_error(arg);
    }
    if (strchr(arg, '"') != NULL) {
        state->quoted = arg; /* a Verilog file with '"' cannot be quoted for read_verilog */
    }
    size_t len = strlen(arg);
    state->script_file |= len > strlen(".o3") && strcmp(arg + len - strlen(".o3"), ".o3") == 0;
    state->scripts = true;
    return EXIT_OK;
}

/* True when the other arguments are Verilog files for read_verilog, not scripts. */
static bool files_mode(const cli_state *state) {
    return state->parse_only || state->elab_only;
}

/* --parse-only and --elab-only take no script (2C §4.1; plan point (w)): the files are their
 * arguments; a -p or a positional argument ending in .o3 (the script suffix) is refused, and so
 * are the two together. */
static int check_files_mode(const cli_state *state) {
    if (!files_mode(state)) {
        return EXIT_OK;
    }
    if (state->parse_only && state->elab_only) {
        (void)fputs("odin3: --parse-only and --elab-only cannot be combined\n", stderr);
        print_usage(stderr);
        return EXIT_USAGE;
    }
    if (state->inline_script || state->script_file) {
        (void)fputs(state->elab_only
                        ? "odin3: --elab-only cannot be combined with a script; use read_verilog "
                          "in the script\n"
                        : "odin3: --parse-only cannot be combined with a script: use the pass "
                          "option (read_verilog --parse-only)\n",
                    stderr);
        print_usage(stderr);
        return EXIT_USAGE;
    }
    if (state->quoted != NULL) {
        return bad_value(state->elab_only ? "--elab-only" : "--parse-only", state->quoted,
                         "a path holding '\"' cannot be passed to read_verilog");
    }
    return EXIT_OK;
}

/* read_project on a --project or -f path (PRJ-18: prepended to the scripts). */
static odin3_status read_project(odin3_design *design, const char *option, const char *path) {
    char args[PROJECT_ARGS_BUF];
    int len = snprintf(args, sizeof args, "%s\"%s\"",
                       strcmp(option, "-f") == 0 ? "--format f " : "", path);
    if (len < 0 || (size_t)len >= sizeof args) {
        (void)fprintf(stderr, "odin3: %s %s: path too long\n", option, path);
        return ODIN3_ERR_INVALID_ARG;
    }
    return odin3_design_run_pass(design, "read_project", args);
}

/* Every --project and -f in command-line order; stops at the first failure. */
static odin3_status each_project(char **argv, int argc, odin3_design *design) {
    odin3_status st = ODIN3_OK;
    for (int i = 1; st == ODIN3_OK && i < argc; i++) {
        const char *arg = argv[i];
        const char *value = NULL;
        if (option_value(argv, argc, &i, &value) && is_project_option(arg)) {
            st = read_project(design, arg, value);
        }
    }
    return st;
}

/* The -p or script file at argv[*at] (advancing past an option's value): run on design, or only
 * resolved when design is NULL. OK for every other argument, and for every positional argument
 * under --parse-only (a Verilog file, not a script). */
static odin3_status script_arg(char **argv, int argc, int *at, odin3_design *design,
                               bool parse_only) {
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
    if (arg[0] == '-' || parse_only) {
        return ODIN3_OK;
    }
    return design == NULL ? odin3_script_resolve_file(arg)
                          : odin3_design_run_script_file(design, arg);
}

/* Every -p and script file in command-line order (see script_arg); stops at the first failure. */
static odin3_status each_script(char **argv, int argc, odin3_design *design, bool parse_only) {
    odin3_status st = ODIN3_OK;
    for (int i = 1; st == ODIN3_OK && i < argc; i++) {
        st = script_arg(argv, argc, &i, design, parse_only);
    }
    return st;
}

/* The positional argument at argv[*at] (advancing past an option's value), else NULL. */
static const char *positional(char **argv, int argc, int *at) {
    const char *value = NULL;
    if (option_value(argv, argc, at, &value) || argv[*at][0] == '-') {
        return NULL;
    }
    return argv[*at];
}

/* --parse-only and --elab-only: read_verilog [--parse-only] "file"... (2C §4.1), after any
 * --project/-f; flag is "--parse-only" or "" (--elab-only, which runs stats afterwards). */
static odin3_status run_read_files(char **argv, int argc, odin3_design *design, const char *flag) {
    size_t len = strlen(flag) + 1;
    for (int i = 1; i < argc; i++) {
        const char *file = positional(argv, argc, &i);
        len += file != NULL ? strlen(file) + strlen(" \"\"") : 0;
    }
    char *args = malloc(len);
    if (args == NULL) {
        (void)fprintf(stderr, "odin3: out of memory\n");
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t used = (size_t)snprintf(args, len, "%s", flag);
    for (int i = 1; i < argc; i++) {
        const char *file = positional(argv, argc, &i);
        used += file != NULL ? (size_t)snprintf(args + used, len - used, " \"%s\"", file) : 0;
    }
    odin3_status st = odin3_design_run_pass(design, "read_verilog", args);
    free(args);
    return st;
}

/* The step after the projects: the files given to --parse-only or --elab-only (the latter then
 * runs stats: -p "read_verilog FILE...; stats"), else the scripts in command-line order. */
static odin3_status each_files_or_scripts(char **argv, int argc, odin3_design *design,
                                          const cli_state *state) {
    if (state->parse_only) {
        return run_read_files(argv, argc, design, "--parse-only");
    }
    if (!state->elab_only) {
        return each_script(argv, argc, design, false);
    }
    odin3_status st = run_read_files(argv, argc, design, "");
    return st == ODIN3_OK ? odin3_design_run_pass(design, "stats", "") : st;
}

/*
 * A --top that no pass applied is reported, not ignored. read_blif applies it after reading (or
 * fails when the file has no such module), and only read_blif creates modules, so the option was
 * applied when the design has a module of that name; a later `hierarchy --top` may then have
 * chosen another top on purpose, which is not warned about. read_project applies it to the
 * project's top (PRJ-18), so a project read counts too.
 */
static void warn_unapplied_top(const odin3_design *design, const char *top) {
    uint32_t id = 0;
    if (top != NULL && odin3_design_lookup_module(design, top, &id) == ODIN3_OK && id == 0 &&
        !odin3_design_has_project(design)) {
        char msg[MSG_BUF];
        (void)snprintf(msg, sizeof msg,
                       "--top %s was not applied (no read_blif, read_project or hierarchy ran)",
                       top);
        (void)odin3_log_write(ODIN3_LOG_WARN, msg);
    }
}

/* Resolves every -p and script file (a typo anywhere costs nothing), then runs on one design
 * every --project and -f (read_project), the scripts in command-line order (with --parse-only:
 * read_verilog --parse-only on the files given instead; with --elab-only: read_verilog, then
 * stats), and dump_project when --dump-project is given. */
static int run_scripts(char **argv, int argc, const cli_state *state) {
    if (each_script(argv, argc, NULL, files_mode(state)) != ODIN3_OK) {
        return EXIT_FAIL;
    }
    odin3_design *design = odin3_design_create();
    if (design == NULL) {
        (void)fprintf(stderr, "odin3: out of memory\n");
        return EXIT_FAIL;
    }
    odin3_status st = odin3_design_set_keep_ast(design, state->keep);
    if (st == ODIN3_OK) {
        st = each_project(argv, argc, design);
    }
    if (st == ODIN3_OK) {
        st = each_files_or_scripts(argv, argc, design, state);
    }
    if (st == ODIN3_OK && state->dump) {
        st = odin3_design_run_pass(design, "dump_project", "");
    }
    if (st == ODIN3_OK && !state->parse_only) { /* --top means nothing to a parse (2D's) */
        warn_unapplied_top(design, state->top);
    }
    odin3_design_destroy(design);
    return st == ODIN3_OK ? EXIT_OK : EXIT_FAIL;
}

/* The options that are copied strings; false when one runs out of memory. */
static bool apply_string_options(const cli_state *state) {
    return odin3_pass_set_top_name(state->top) == ODIN3_OK &&
           odin3_pass_set_revision(state->revision) == ODIN3_OK &&
           odin3_pass_set_abc_script(state->abc_script) == ODIN3_OK &&
           odin3_pass_set_opt_snapshot(state->opt_snapshot) == ODIN3_OK;
}

int main(int argc, char **argv) {
    cli_state state = {0};
    state.linked_abc = init_linked_abc();
    (void)init_linked_slang();
    for (int i = 1; i < argc; i++) {
        int rc = scan_one(argv, argc, &i, &state);
        if (rc != EXIT_OK) {
            return rc;
        }
    }
    if (argc == 1 || state.help) {
        print_usage(stdout);
    }
    int rc = check_files_mode(&state);
    if (rc != EXIT_OK) {
        return rc;
    }
    if (!(state.scripts || state.inline_script || state.project || state.dump ||
          files_mode(&state)) ||
        state.help) {
        return EXIT_OK;
    }
    odin3_pass_set_check(state.check);
    odin3_pass_set_diag_paths(state.resolved);
    if (!apply_string_options(&state)) {
        (void)fprintf(stderr, "odin3: out of memory\n");
        return EXIT_FAIL;
    }
    return run_scripts(argv, argc, &state);
}
