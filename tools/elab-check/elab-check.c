/*
 * elab-check.c — the elaboration coverage-row driver (2D spec §14.2, plan Task 25).
 *
 *   elab-check [--write] [--nolatches] [--stage1] [--top T] [--dump] [--smallest-first] <file>...
 *
 * Per file <row>_<slug>.v, from its own directory under its basename: (1) the read_verilog pass
 * (both stages, Debug checks on; --nolatches as given), or stage 1 alone under --stage1 (the
 * controller's check of a PR head before 2D-5); (2) odin3_ast_check FULL of the elaborated store,
 * and odin3_check_design FULL unless --stage1; (3) odin3_ast_dump of that store against
 * <row>.elab; (4) unless --stage1, odin3_rtlil_text against <row>.il, or <row>.nolatches.il under
 * --nolatches. --write writes the goldens instead (a "# hand-written" .elab is refused).
 * --dump prints the dump to stdout; --smallest-first runs the files in ascending size. Exit 0 all
 * pass, 1 any differs or fails, 2 usage or I/O.
 *
 * In-file directive: a line "// elab-check: --auto-blackbox" in the first 1024 bytes of a row
 * (not on line 1) elaborates it with --auto-blackbox (undefined modules become black boxes).
 */
#include "ast/ast.h"
#include "ast/check.h"
#include "ast/print.h"
#include "backends/rtlil/writer.h"
#include "frontends/elab/elab.h"
#include "frontends/verilog/elab1.h"
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/read_verilog.h"
#include "ir/check.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "util/file.h"
#include "util/log.h"
#include "util/str.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { EC_PASS = 0, EC_DIFF = 1, EC_USAGE = 2, EC_IO = 2 };
enum { EC_MAX_GOLDEN = 1 << 26, EC_PATH_BYTES = 4096, EC_ARG_BYTES = 4200, EC_HEAD_BYTES = 1024 };

static const char k_hand[] = "# hand-written\n";
static const char k_generated[] = "# generated: elab-check --write\n";

typedef struct ec_args {
    bool write, nolatches, stage1, dump, smallest_first;
    const char *top;
    char **files;
    int nfiles;
} ec_args;

/* What one file is elaborated into. */
typedef struct ec_result {
    odin3_design *design;
    odin3_ast *own;     /* a store this result owns (stage-1 mode) */
    odin3_ast *elab;    /* the elaborated store (borrowed from the design, or own) */
    bool auto_blackbox; /* the file's "// elab-check: --auto-blackbox" directive */
    odin3_strbuf dump;
    odin3_strbuf il;
} ec_result;

static void usage(void) {
    (void)fprintf(stderr, "usage: elab-check [--write] [--nolatches] [--stage1] [--top T] "
                          "[--dump] [--smallest-first] <file.v>...\n"
                          "a '// elab-check: --auto-blackbox' line in a file's head "
                          "(after line 1) enables --auto-blackbox for it\n");
}

static bool parse_flag(const char *arg, ec_args *args) {
    bool *flag = NULL;
    if (strcmp(arg, "--write") == 0) {
        flag = &args->write;
    } else if (strcmp(arg, "--nolatches") == 0) {
        flag = &args->nolatches;
    } else if (strcmp(arg, "--stage1") == 0) {
        flag = &args->stage1;
    } else if (strcmp(arg, "--dump") == 0) {
        flag = &args->dump;
    } else if (strcmp(arg, "--smallest-first") == 0) {
        flag = &args->smallest_first;
    }
    if (flag != NULL) {
        *flag = true;
    }
    return flag != NULL;
}

/* Options first or interleaved; the files are gathered at the front of argv. */
static bool parse_args(int argc, char **argv, ec_args *args) {
    *args = (ec_args){.files = argv + 1};
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            args->top = argv[++i];
        } else if (argv[i][0] != '-') {
            argv[1 + args->nfiles++] = argv[i];
        } else if (!parse_flag(argv[i], args)) {
            return false;
        }
    }
    return args->nfiles > 0;
}

/* --- goldens -------------------------------------------------------------------------------- */

static bool starts_with(const odin3_file_bytes *bytes, const char *prefix) {
    size_t len = strlen(prefix);
    return bytes->len >= len && memcmp(bytes->data, prefix, len) == 0;
}

/* The golden's body: everything after a first line that is one of the two markers. */
static odin3_bytes golden_body(const odin3_file_bytes *bytes) {
    size_t skip = 0;
    if (starts_with(bytes, k_hand)) {
        /* the marker and the comment lines after it (a provisional note, review I8) */
        const char *text = (const char *)bytes->data;
        while (skip < bytes->len && text[skip] == '#') {
            const char *nl = memchr(text + skip, '\n', bytes->len - skip);
            skip = nl != NULL ? (size_t)(nl - text) + 1 : bytes->len;
        }
    } else if (starts_with(bytes, k_generated)) {
        skip = strlen(k_generated);
    }
    return (odin3_bytes){bytes->data + skip, bytes->len - skip};
}

static int write_golden(const char *golden, const odin3_strbuf *text) {
    odin3_atomic_file file;
    if (odin3_atomic_file_open(&file, golden) != ODIN3_OK) {
        return EC_IO;
    }
    bool ok = fputs(k_generated, file.fp) >= 0 &&
              (text->len == 0 || fwrite(text->data, 1, text->len, file.fp) == text->len);
    return odin3_atomic_file_close(&file, ok ? ODIN3_OK : ODIN3_ERR_IO) == ODIN3_OK ? EC_PASS
                                                                                    : EC_IO;
}

/* The line number (1-based) where a and b first differ. */
static unsigned first_diff_line(odin3_bytes want, const odin3_strbuf *got) {
    unsigned line = 1;
    size_t n = want.len < got->len ? want.len : got->len;
    const char *ref = want.ptr;
    for (size_t i = 0; i < n; i++) {
        if (ref[i] != got->data[i]) {
            break;
        }
        line += ref[i] == '\n' ? 1U : 0U;
    }
    return line;
}

/* text against the golden (or written to it under --write). */
static int golden_check(const ec_args *args, const char *golden, const odin3_strbuf *text) {
    odin3_file_bytes bytes = {0};
    odin3_status st = odin3_file_read_all(golden, EC_MAX_GOLDEN, &bytes);
    bool exists = st == ODIN3_OK;
    int rc = EC_PASS;
    if (!exists && !(st == ODIN3_ERR_IO && bytes.err == ENOENT)) {
        (void)fprintf(stderr, "%s: cannot read the golden\n", golden);
        rc = EC_IO;
    } else if (args->write && exists && starts_with(&bytes, k_hand)) {
        (void)fprintf(stderr, "%s: refusing to overwrite a hand-written golden\n", golden);
        rc = EC_USAGE;
    } else if (args->write) {
        rc = write_golden(golden, text);
    } else if (!exists) {
        (void)fprintf(stderr, "%s: no golden\n", golden);
        rc = EC_DIFF;
    } else {
        odin3_bytes body = golden_body(&bytes);
        if (body.len != text->len ||
            (body.len > 0 && memcmp(body.ptr, text->data, body.len) != 0)) {
            (void)fprintf(stderr, "%s: differs from the golden at line %u\n", golden,
                          first_diff_line(body, text));
            rc = EC_DIFF;
        }
    }
    odin3_file_bytes_free(&bytes);
    return rc;
}

/* <dir><row>.<ext> of the file path: the row is the name up to the first '_' (else the stem). */
static bool golden_path(const char *path, const char *ext, char *out, size_t cap) {
    const char *slash = strrchr(path, '/');
    const char *name = slash != NULL ? slash + 1 : path;
    size_t dir = (size_t)(name - path);
    size_t row = strcspn(name, "_.");
    int len = snprintf(out, cap, "%.*s%.*s.%s", (int)dir, path, (int)row, name, ext);
    return len > 0 && (size_t)len < cap;
}

/* --- elaboration ---------------------------------------------------------------------------- */

static void result_free(ec_result *res) {
    odin3_ast_destroy(res->own);
    odin3_design_destroy(res->design);
    odin3_strbuf_free(&res->dump);
    odin3_strbuf_free(&res->il);
    *res = (ec_result){0};
}

static uint32_t intern(odin3_design *design, const char *text) {
    uint32_t id = 0;
    return odin3_design_intern(design, odin3_bytes_cstr(text), &id) == ODIN3_OK ? id : 0;
}

/* Stage 1 alone on one file of the current directory (the controller's pre-2D-5 check). */
static odin3_status run_stage1(const ec_args *args, const char *name, ec_result *res) {
    char cwd[EC_PATH_BYTES];
    char resolved[EC_ARG_BYTES];
    if (getcwd(cwd, sizeof cwd) == NULL ||
        snprintf(resolved, sizeof resolved, "%s/%s", cwd, name) >= (int)sizeof resolved) {
        return ODIN3_ERR_IO;
    }
    odin3_pass_ctx ctx;
    odin3_pp_source src = {
        intern(res->design, name), intern(res->design, resolved), intern(res->design, "work"), {0}};
    odin3_status st = odin3_pass_run_begin(res->design, intern(res->design, "read_verilog"), &ctx);
    odin3_ast *parsed = NULL;
    odin3_rv_opts ropts = {&src, 1, ctx.run, NULL};
    st = st != ODIN3_OK ? st : odin3_read_verilog_files(res->design, &ropts, &parsed);
    odin3_elab_opts opts;
    st = st != ODIN3_OK ? st : odin3_elab1_options(res->design, &(odin3_elab_opts){0}, &opts);
    opts.nolatches = args->nolatches;
    opts.auto_blackbox = res->auto_blackbox;
    odin3_ast *elab = NULL;
    st = st != ODIN3_OK ? st : odin3_elab_stage1(&ctx, parsed, &opts, &elab);
    odin3_ast_destroy(parsed);
    res->own = elab;
    res->elab = elab;
    return st;
}

/* The held ELABORATED store of the design. */
static odin3_ast *find_elab(const odin3_design *design) {
    for (uint32_t i = 0; i < odin3_design_ast_count(design); i++) {
        odin3_ast *ast = odin3_design_ast_at(design, i);
        if (odin3_ast_form_of(ast) == ODIN3_AST_FORM_ELABORATED) {
            return ast;
        }
    }
    return NULL;
}

/* The whole read_verilog pass (both stages, checks on), the elaborated store kept. */
static odin3_status run_full(const ec_args *args, const char *name, ec_result *res) {
    odin3_status st = odin3_design_set_keep_ast(res->design, ODIN3_KEEP_AST_ELABORATED);
    char cmd[EC_ARG_BYTES];
    if (snprintf(cmd, sizeof cmd, "%s%s%s", args->nolatches ? "--nolatches " : "",
                 res->auto_blackbox ? "--auto-blackbox " : "", name) >= (int)sizeof cmd) {
        return ODIN3_ERR_INVALID_ARG;
    }
    st = st != ODIN3_OK ? st : odin3_pass_run(res->design, "read_verilog", odin3_bytes_cstr(cmd));
    res->elab = find_elab(res->design);
    return st != ODIN3_OK || res->elab != NULL ? st : ODIN3_ERR_CHECK;
}

static bool dump_store(ec_result *res) {
    for (uint32_t i = 0; i < odin3_ast_root_count(res->elab); i++) {
        if (odin3_ast_dump(res->elab, odin3_ast_root(res->elab, i), &res->dump) != ODIN3_OK ||
            odin3_strbuf_append(&res->dump, odin3_bytes_cstr("\n")) != ODIN3_OK) {
            return false;
        }
    }
    return true;
}

/* Elaborates, checks FULL and fills the dump (and the RTLIL text unless stage 1). */
static int elaborate(const ec_args *args, const char *name, ec_result *res) {
    odin3_strbuf_init(&res->dump);
    odin3_strbuf_init(&res->il);
    res->design = odin3_design_create();
    if (res->design == NULL) {
        return EC_IO;
    }
    odin3_status st = args->stage1 ? run_stage1(args, name, res) : run_full(args, name, res);
    if (st != ODIN3_OK || res->elab == NULL) {
        (void)fprintf(stderr, "%s: elaboration failed (status %d)\n", name, (int)st);
        return EC_DIFF;
    }
    odin3_check_opts full = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    if (odin3_ast_check(res->elab, ODIN3_CHECK_FULL) != ODIN3_OK ||
        (!args->stage1 && odin3_check_design(res->design, full) != ODIN3_OK)) {
        (void)fprintf(stderr, "%s: check FULL failed\n", name);
        return EC_DIFF;
    }
    if (!dump_store(res) ||
        (!args->stage1 && odin3_rtlil_text(res->design, &res->il) != ODIN3_OK)) {
        (void)fprintf(stderr, "%s: cannot print the result\n", name);
        return EC_DIFF;
    }
    return EC_PASS;
}

/* True when one of the file's first lines is "// elab-check: ... --auto-blackbox". */
static bool wants_auto_blackbox(const char *name) {
    odin3_file_bytes bytes = {0};
    bool found = false;
    if (odin3_file_read_all(name, EC_MAX_GOLDEN, &bytes) == ODIN3_OK) {
        static const char key[] = "\n// elab-check:";
        const char *text = (const char *)bytes.data;
        size_t head = bytes.len < EC_HEAD_BYTES ? bytes.len : EC_HEAD_BYTES;
        char *copy = strndup(text, head);
        const char *line = copy != NULL ? strstr(copy, key) : NULL;
        const char *eol = line != NULL ? strchr(line + 1, '\n') : NULL;
        found = line != NULL && strstr(line, "--auto-blackbox") != NULL &&
                (eol == NULL || strstr(line, "--auto-blackbox") < eol);
        free(copy);
    }
    odin3_file_bytes_free(&bytes);
    return found;
}

/* --- one file ------------------------------------------------------------------------------- */

static int max_rc(int lhs, int rhs) {
    return lhs > rhs ? lhs : rhs;
}

/* The goldens of name (the basename; the working directory is its directory) against the result. */
static int compare_goldens(const ec_args *args, const char *path, const ec_result *res) {
    char golden[EC_PATH_BYTES];
    if (args->dump && res->dump.len > 0) {
        (void)fwrite(res->dump.data, 1, res->dump.len, stdout);
    }
    int rc = golden_path(path, "elab", golden, sizeof golden)
                 ? golden_check(args, golden, &res->dump)
                 : EC_IO;
    if (!args->stage1) {
        const char *ext = args->nolatches ? "nolatches.il" : "il";
        rc = max_rc(rc, golden_path(path, ext, golden, sizeof golden)
                            ? golden_check(args, golden, &res->il)
                            : EC_IO);
    }
    return rc;
}

/* The file elaborated in its own directory under its basename; the working directory restored. */
static int check_file(const ec_args *args, const char *path) {
    const char *slash = strrchr(path, '/');
    const char *name = slash != NULL ? slash + 1 : path;
    char dir[EC_PATH_BYTES];
    int len = slash == NULL ? snprintf(dir, sizeof dir, ".")
                            : snprintf(dir, sizeof dir, "%.*s", (int)(slash - path + 1), path);
    int home = open(".", O_RDONLY | O_DIRECTORY);
    if (len < 0 || (size_t)len >= sizeof dir || home < 0 || chdir(dir) != 0 ||
        access(name, R_OK) != 0) {
        (void)fprintf(stderr, "%s: cannot read: %s\n", path, strerror(errno));
        if (home >= 0) {
            (void)close(home);
        }
        return EC_IO;
    }
    odin3_pass_set_options((odin3_pass_options){.check = true, .top = args->top});
    ec_result res = {.auto_blackbox = wants_auto_blackbox(name)};
    int rc = elaborate(args, name, &res);
    rc = rc == EC_PASS ? compare_goldens(args, name, &res) : rc;
    result_free(&res);
    if (fchdir(home) != 0) {
        rc = EC_IO;
    }
    (void)close(home);
    if (rc != EC_PASS) {
        (void)fprintf(stderr, "FAIL %s\n", path);
    }
    return rc;
}

/* --- main ----------------------------------------------------------------------------------- */

static off_t file_size(const char *path) {
    struct stat sb;
    return stat(path, &sb) == 0 ? sb.st_size : 0;
}

static int by_size(const void *lhs, const void *rhs) {
    const char *left = *(char *const *)lhs;
    const char *right = *(char *const *)rhs;
    off_t lsize = file_size(left);
    off_t rsize = file_size(right);
    if (lsize != rsize) {
        return lsize < rsize ? -1 : 1;
    }
    return strcmp(left, right);
}

int main(int argc, char **argv) {
    ec_args args;
    if (!parse_args(argc, argv, &args)) {
        usage();
        return EC_USAGE;
    }
    (void)odin3_log_set_level(ODIN3_LOG_WARN);
    if (args.smallest_first) {
        qsort((void *)args.files, (size_t)args.nfiles, sizeof *args.files, by_size);
    }
    int rc = EC_PASS;
    int failed = 0;
    for (int i = 0; i < args.nfiles; i++) {
        int one = check_file(&args, args.files[i]);
        failed += one != EC_PASS ? 1 : 0;
        rc = max_rc(rc, one);
    }
    (void)fprintf(stderr, "elab-check: %d files, %d failed\n", args.nfiles, failed);
    return rc;
}
