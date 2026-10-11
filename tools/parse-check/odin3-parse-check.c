/*
 * odin3-parse-check.c — the coverage-row driver (2C spec §7.2, plan Task 9).
 *
 *   odin3-parse-check [--write] [--dump] [--no-oracle] [--smallest-first] <file>…
 *
 * Per file, from the file's own directory under its basename (plan point (o)): the read_verilog
 * pass's steps 1–7 on the one file with the token map → check FULL → print the UNIT → read the
 * printed text as a second one-file run → check FULL → equal child counts and pairwise equal
 * children (the UNITs' names differ) → the children's dump against <stem>.ast when it exists
 * (--write writes it instead, refusing a "# hand-written" golden) → the location oracle (unless
 * --no-oracle). --dump prints the dump to stdout; --smallest-first runs the files in ascending
 * size. Exit 0 every file passes, 1 any differs or fails to read, 2 usage or I/O.
 */
#include "parse_check.h"

#include "ast/ast.h"
#include "ast/check.h"
#include "ast/print.h"
#include "odin3/odin3.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { PC_PASS = 0, PC_DIFF = 1, PC_USAGE = 2, PC_IO = 2 };
enum { PC_MAX_GOLDEN = 1 << 26, PC_PATH_BYTES = 4096, PC_TMP_BYTES = 256 };

static const char k_hand[] = "# hand-written\n";
static const char k_generated[] = "# generated: odin3-parse-check --write\n";

typedef struct pc_args {
    bool write, dump, no_oracle, smallest_first;
    char **files;
    int nfiles;
} pc_args;

static void usage(void) {
    (void)fprintf(stderr, "usage: odin3-parse-check [--write] [--dump] [--no-oracle] "
                          "[--smallest-first] <file.v>...\n");
}

/* Options first or interleaved; the files are gathered at the front of argv. */
static bool parse_args(int argc, char **argv, pc_args *args) {
    *args = (pc_args){.files = argv + 1};
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--write") == 0) {
            args->write = true;
        } else if (strcmp(arg, "--dump") == 0) {
            args->dump = true;
        } else if (strcmp(arg, "--no-oracle") == 0) {
            args->no_oracle = true;
        } else if (strcmp(arg, "--smallest-first") == 0) {
            args->smallest_first = true;
        } else if (arg[0] == '-') {
            return false;
        } else {
            argv[1 + args->nfiles++] = argv[i];
        }
    }
    return args->nfiles > 0;
}

/* --- the round trip ------------------------------------------------------------------------- */

/* text into a new temporary file; its path in path. */
static int write_temp(const odin3_strbuf *text, char *path, size_t cap) {
    const char *dir = getenv("TMPDIR");
    int len = snprintf(path, cap, "%s/odin3-parse-check-XXXXXX",
                       dir != NULL && dir[0] != '\0' ? dir : "/tmp");
    if (len < 0 || (size_t)len >= cap) {
        return PC_IO;
    }
    int fd = mkstemp(path);
    FILE *fp = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (fp == NULL) {
        (void)fprintf(stderr, "odin3-parse-check: cannot create %s: %s\n", path, strerror(errno));
        if (fd >= 0) {
            (void)close(fd);
            (void)unlink(path);
        }
        return PC_IO;
    }
    bool ok = text->len == 0 || fwrite(text->data, 1, text->len, fp) == text->len;
    ok = fclose(fp) == 0 && ok;
    if (!ok) {
        (void)unlink(path);
        return PC_IO;
    }
    return PC_PASS;
}

/* The two UNITs have equally many, pairwise equal children. */
static bool children_equal(const pc_read *first, const pc_read *again) {
    uint32_t count = odin3_ast_nchild(first->ast, first->unit);
    if (count != odin3_ast_nchild(again->ast, again->unit)) {
        (void)fprintf(stderr, "  re-parse: %u items, not %u\n",
                      (unsigned)odin3_ast_nchild(again->ast, again->unit), (unsigned)count);
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        bool equal = false;
        if (odin3_ast_equal(first->ast, odin3_ast_child(first->ast, first->unit, i), again->ast,
                            odin3_ast_child(again->ast, again->unit, i), &equal) != ODIN3_OK ||
            !equal) {
            (void)fprintf(stderr, "  re-parse: item %u differs\n", (unsigned)i + 1);
            return false;
        }
    }
    return true;
}

/* print → read the printed text → check FULL → equal children. */
static int round_trip(const pc_read *read, const char *name) {
    odin3_strbuf printed;
    odin3_strbuf_init(&printed);
    int rc = odin3_ast_print(read->ast, read->unit, &printed) == ODIN3_OK ? PC_PASS : PC_DIFF;
    char path[PC_TMP_BYTES];
    rc = rc != PC_PASS ? rc : write_temp(&printed, path, sizeof path);
    if (rc == PC_PASS) {
        pc_read again;
        odin3_status st = pc_read_file(path, &again);
        if (st != ODIN3_OK || odin3_ast_check(again.ast, ODIN3_CHECK_FULL) != ODIN3_OK ||
            !children_equal(read, &again)) {
            (void)fprintf(stderr, "%s: the printed text does not read back equal:\n%s\n", name,
                          printed.data != NULL ? printed.data : "");
            rc = PC_DIFF;
        }
        pc_read_free(&again);
        (void)unlink(path);
    }
    odin3_strbuf_free(&printed);
    return rc;
}

/* --- the golden ----------------------------------------------------------------------------- */

static bool dump_children(const pc_read *read, odin3_strbuf *out) {
    for (uint32_t i = 0; i < odin3_ast_nchild(read->ast, read->unit); i++) {
        if (odin3_ast_dump(read->ast, odin3_ast_child(read->ast, read->unit, i), out) != ODIN3_OK ||
            odin3_strbuf_append(out, odin3_bytes_cstr("\n")) != ODIN3_OK) {
            return false;
        }
    }
    return true;
}

static bool starts_with(const odin3_file_bytes *bytes, const char *prefix) {
    size_t len = strlen(prefix);
    return bytes->len >= len && memcmp(bytes->data, prefix, len) == 0;
}

/* The golden's body: everything after a first line starting with "# ". */
static odin3_bytes golden_body(const odin3_file_bytes *bytes) {
    odin3_bytes body = {bytes->data, bytes->len};
    if (starts_with(bytes, "# ")) {
        const uint8_t *nl = memchr(bytes->data, '\n', bytes->len);
        size_t skip = nl != NULL ? (size_t)(nl - bytes->data) + 1 : bytes->len;
        body = (odin3_bytes){bytes->data + skip, bytes->len - skip};
    }
    return body;
}

static int write_golden(const char *golden, const odin3_strbuf *dump) {
    odin3_atomic_file file;
    if (odin3_atomic_file_open(&file, golden) != ODIN3_OK) {
        return PC_IO;
    }
    bool ok = fputs(k_generated, file.fp) >= 0 &&
              (dump->len == 0 || fwrite(dump->data, 1, dump->len, file.fp) == dump->len);
    odin3_status st = odin3_atomic_file_close(&file, ok ? ODIN3_OK : ODIN3_ERR_IO);
    return st == ODIN3_OK ? PC_PASS : PC_IO;
}

/* The dump against <stem>.ast (or written to it under --write). */
static int golden_check(const pc_args *args, const char *golden, const odin3_strbuf *dump) {
    odin3_file_bytes bytes = {0};
    odin3_status st = odin3_file_read_all(golden, PC_MAX_GOLDEN, &bytes);
    bool exists = st == ODIN3_OK;
    int rc = PC_PASS;
    if (!exists && !(st == ODIN3_ERR_IO && bytes.err == ENOENT)) {
        (void)fprintf(stderr, "%s: cannot read the golden\n", golden);
        rc = PC_IO;
    } else if (args->write && exists && starts_with(&bytes, k_hand)) {
        (void)fprintf(stderr, "%s: refusing to overwrite a hand-written golden\n", golden);
        rc = PC_USAGE;
    } else if (args->write) {
        rc = write_golden(golden, dump);
    } else if (exists) {
        odin3_bytes body = golden_body(&bytes);
        if (body.len != dump->len ||
            (body.len > 0 && memcmp(body.ptr, dump->data, body.len) != 0)) {
            (void)fprintf(stderr, "%s: the dump differs from the golden; the dump:\n%s", golden,
                          dump->data != NULL ? dump->data : "");
            rc = PC_DIFF;
        }
    }
    odin3_file_bytes_free(&bytes);
    return rc;
}

/* <stem>.ast of name (".v" replaced, else ".ast" appended) into out. */
static bool golden_path(const char *name, char *out, size_t cap) {
    size_t len = strlen(name);
    size_t stem = len > 2 && strcmp(name + len - 2, ".v") == 0 ? len - 2 : len;
    int written = snprintf(out, cap, "%.*s.ast", (int)stem, name);
    return written > 0 && (size_t)written < cap;
}

/* --- one file ------------------------------------------------------------------------------- */

static int max_rc(int lhs, int rhs) {
    return lhs > rhs ? lhs : rhs;
}

/* A file being checked: its name in its own directory (the golden's stem) and as given. */
typedef struct pc_file {
    const char *base;
    const char *path;
} pc_file;

/* The checks on one read. */
static int check_read(const pc_args *args, const pc_read *read, pc_file file) {
    const char *base = file.base;
    const char *name = file.path;
    if (odin3_ast_check(read->ast, ODIN3_CHECK_FULL) != ODIN3_OK) {
        return PC_DIFF;
    }
    int rc = round_trip(read, name);
    odin3_strbuf dump;
    odin3_strbuf_init(&dump);
    if (!dump_children(read, &dump)) {
        rc = PC_DIFF;
    } else {
        char golden[PC_PATH_BYTES];
        if (args->dump && dump.len > 0) {
            (void)fwrite(dump.data, 1, dump.len, stdout);
        }
        rc = max_rc(rc, golden_path(base, golden, sizeof golden) ? golden_check(args, golden, &dump)
                                                                 : PC_IO);
    }
    odin3_strbuf_free(&dump);
    uint32_t failures = 0;
    if (!args->no_oracle && (pc_loc_oracle(read, &failures) != ODIN3_OK || failures > 0)) {
        (void)fprintf(stderr, "%s: location oracle: %u failures\n", name, (unsigned)failures);
        rc = max_rc(rc, PC_DIFF);
    }
    return rc;
}

/* The file in its own directory under its basename; the working directory is restored. */
static int check_file(const pc_args *args, const char *path) {
    const char *slash = strrchr(path, '/');
    const char *name = slash != NULL ? slash + 1 : path;
    char dir[PC_PATH_BYTES];
    int len = slash == NULL ? snprintf(dir, sizeof dir, ".")
                            : snprintf(dir, sizeof dir, "%.*s", (int)(slash - path + 1), path);
    int home = open(".", O_RDONLY | O_DIRECTORY);
    if (len < 0 || (size_t)len >= sizeof dir || home < 0 || chdir(dir) != 0 ||
        access(name, R_OK) != 0) {
        (void)fprintf(stderr, "%s: cannot read: %s\n", path, strerror(errno));
        if (home >= 0) {
            (void)close(home);
        }
        return PC_IO;
    }
    pc_read read;
    odin3_status st = pc_read_file(name, &read);
    int rc = st == ODIN3_OK ? check_read(args, &read, (pc_file){name, path}) : PC_DIFF;
    pc_read_free(&read);
    if (fchdir(home) != 0) {
        rc = PC_IO;
    }
    (void)close(home);
    if (rc != PC_PASS) {
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
    pc_args args;
    if (!parse_args(argc, argv, &args)) {
        usage();
        return PC_USAGE;
    }
    (void)odin3_log_set_level(ODIN3_LOG_WARN);
    if (args.smallest_first) {
        qsort((void *)args.files, (size_t)args.nfiles, sizeof *args.files, by_size);
    }
    int rc = PC_PASS;
    int failed = 0;
    for (int i = 0; i < args.nfiles; i++) {
        int one = check_file(&args, args.files[i]);
        failed += one != PC_PASS ? 1 : 0;
        rc = max_rc(rc, one);
    }
    (void)fprintf(stderr, "odin3-parse-check: %d files, %d failed\n", args.nfiles, failed);
    return rc;
}
