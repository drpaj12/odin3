/*
 * fuzz_pp.c — the preprocessor's crash fuzzer (2B spec §10.5), a libFuzzer target built with
 * -DODIN3_FUZZ=ON under clang, ASan and UBSan.
 *
 * Input: one options byte, then the source file. The harness writes the file into its own
 * temporary directory (beside a few fixed headers it may include), creates a preprocessor with
 * every §9 cap lowered through odin3_pp_test_create, runs odin3_pp_run on the file and requires:
 * the status is OK or PARSE (NO_MEMORY only when an allocation failure was injected), and
 * odin3_pp_check passes on every OK stream. A crash, a leak, a sanitizer report or a hang (each
 * run is bounded by the lowered caps; libFuzzer's -timeout is the test) is a finding.
 *
 * Options byte: bit 0 project defines; bit 1 an include directory; bit 2 run the file twice
 * (macros persist, the file comes from the cache); bits 3-5 inject an allocation failure (0:
 * none); bits 6-7 the cap profile (tight or roomy, both far below the defaults).
 */
#include "frontends/verilog/preproc.h"
#include "frontends/verilog/preproc_test.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/hash.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

enum {
    PATH_MAX_LEN = 1024,
    OPT_DEFINES = 1 << 0,
    OPT_INCDIR = 1 << 1,
    OPT_TWICE = 1 << 2,
    OPT_FAIL_SHIFT = 3,
    OPT_FAIL_MASK = 7,
    OPT_CAPS_SHIFT = 6,
    OPT_CAPS_ROOMY = 2, /* profiles 2 and 3 */
    FAIL_SCALE = 4,     /* the injected failure: after FAIL_SCALE * n^3 allocations */
    DIR_MODE = 0700
};

static char dir[PATH_MAX_LEN];
static char input[PATH_MAX_LEN];
static char incdir[PATH_MAX_LEN];

/* The headers an input may include from its own directory, and one in the include directory
 * (with a shadowed a.vh): plain macros, a nested include, a cycle, an unterminated `ifdef, an
 * empty header and one without a final newline. */
static const char *const k_headers[][2] = {
    {"a.vh", "`define A(x, y=1) (x + y)\n`define B `A(2)\nwire a = `B;\n"},
    {"b.vh", "`include \"a.vh\"\n`ifdef W wire w; `else wire nw; `endif\n"},
    {"self.vh", "`include \"self.vh\"\n"},
    {"open.vh", "`ifdef X\n"},
    {"empty.vh", ""},
    {"noeol.vh", "endmodule"},
    {"inc/d.vh", "`define D(p) {p, `W}\n"},
    {"inc/a.vh", "wire shadow;\n"},
};

static const odin3_pp_define k_defines[] = {
    {{"W", 1}, {"8", 1}, true},
    {{"E", 1}, {"", 0}, false},
    {{"U", 1}, {"`W + 1", 6}, true},
    {{"S", 1}, {"\"s // t\"", 8}, true},
};

/* The last message logged (printed with a failure). */
static char last_log[PATH_MAX_LEN];

static void fail(const char *what, odin3_status st) {
    (void)fprintf(stderr, "fuzz_pp: %s (status %d; last logged: %s)\n", what, (int)st, last_log);
    abort();
}

static void write_file(const char *path, const void *bytes, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (fp == NULL || (len > 0 && fwrite(bytes, 1, len, fp) != len) || fclose(fp) != 0) {
        fail("cannot write a file in the temporary directory", ODIN3_ERR_IO);
    }
}

static void remove_files(void) {
    for (size_t i = 0; i < sizeof k_headers / sizeof k_headers[0]; i++) {
        char path[PATH_MAX_LEN];
        (void)snprintf(path, sizeof path, "%s/%s", dir, k_headers[i][0]);
        (void)remove(path);
    }
    (void)remove(input);
    (void)rmdir(incdir);
    (void)rmdir(dir);
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    const char *base = getenv("TMPDIR");
    (void)snprintf(dir, sizeof dir, "%s/fuzz_pp_XXXXXX",
                   base != NULL && base[0] != '\0' ? base : "/tmp");
    if (mkdtemp(dir) == NULL) {
        fail("cannot create the temporary directory", ODIN3_ERR_IO);
    }
    (void)snprintf(incdir, sizeof incdir, "%s/inc", dir);
    (void)snprintf(input, sizeof input, "%s/in.v", dir);
    if (mkdir(incdir, DIR_MODE) != 0) {
        fail("cannot create the include directory", ODIN3_ERR_IO);
    }
    for (size_t i = 0; i < sizeof k_headers / sizeof k_headers[0]; i++) {
        char path[PATH_MAX_LEN];
        (void)snprintf(path, sizeof path, "%s/%s", dir, k_headers[i][0]);
        write_file(path, k_headers[i][1], strlen(k_headers[i][1]));
    }
    (void)atexit(remove_files);
    return 0;
}

/* Every cap far below its default, so the bounded worst case of one input stays within
 * libFuzzer's RSS limit and timeout; the tight profile hits the caps often. */
static odin3_pp_limits limits_for(unsigned profile) {
    odin3_pp_limits lim = odin3_pp_default_limits();
    bool roomy = profile >= OPT_CAPS_ROOMY;
    lim.include_depth = roomy ? 16 : 4;
    lim.expansion_chain = roomy ? 128 : 8;
    lim.expansion_depth = roomy ? 128 : 8;
    lim.live_runs = roomy ? 1U << 15 : 256;
    lim.cond_depth = roomy ? 64 : 8;
    lim.macro_args = roomy ? 32 : 4;
    lim.macro_text = roomy ? 1U << 14 : 256;
    lim.macros = roomy ? 1U << 10 : 16;
    lim.definitions = roomy ? 1U << 12 : 32;
    lim.macro_bytes = roomy ? 1U << 20 : 1U << 18; /* 4 arena chunks: the cap keeps one free */
    lim.stream_bytes = roomy ? 1U << 20 : 4096;
    lim.buffers_per_stream = roomy ? 1U << 14 : 256;
    lim.includes_per_stream = roomy ? 256 : 8;
    lim.loc_bytes_per_stream = roomy ? 1U << 22 : 1U << 14;
    lim.file_cache_bytes = roomy ? 1U << 22 : 1U << 16;
    lim.srcman_bytes_per_stream = roomy ? 1U << 22 : 1U << 14;
    return lim;
}

static void quiet(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    (void)snprintf(last_log, sizeof last_log, "%s", msg);
}

/* One run of the input: OK (and pp_check passes) or PARSE; NO_MEMORY only when injected. */
static void run_once(odin3_pp *pp, const odin3_pp_source *src, bool injected) {
    odin3_pp_stream stream;
    odin3_status st = odin3_pp_run(pp, src, &stream);
    odin3_util_set_alloc_fail_after(-1);
    if (st == ODIN3_OK) {
        st = odin3_pp_check(pp, stream.stream);
        if (st != ODIN3_OK) {
            fail("odin3_pp_check failed on an OK stream", st);
        }
    } else if (st != ODIN3_ERR_PARSE && !(st == ODIN3_ERR_NO_MEMORY && injected)) {
        fail("odin3_pp_run returned neither OK nor PARSE", st);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) {
        return 0;
    }
    unsigned opts = data[0];
    write_file(input, data + 1, size - 1);
    odin3_design *design = odin3_design_create();
    odin3_pp_source src = {0, 0, 0, {0}};
    if (design == NULL || odin3_design_intern(design, odin3_bytes_cstr("in.v"), &src.given) ||
        odin3_design_intern(design, odin3_bytes_cstr(input), &src.resolved) ||
        odin3_design_intern(design, odin3_bytes_cstr("work"), &src.library)) {
        fail("cannot set up the design", ODIN3_ERR_NO_MEMORY);
    }
    odin3_log_set_sink(quiet, NULL);
    last_log[0] = '\0';
    const char *const incdirs[] = {incdir};
    odin3_pp_config config = {(opts & OPT_DEFINES) ? k_defines : NULL,
                              (opts & OPT_DEFINES) ? sizeof k_defines / sizeof k_defines[0] : 0,
                              (opts & OPT_INCDIR) ? incdirs : NULL, (opts & OPT_INCDIR) ? 1 : 0};
    long nth = (long)((opts >> OPT_FAIL_SHIFT) & OPT_FAIL_MASK);
    bool injected = nth != 0;
    if (injected) {
        odin3_util_set_alloc_fail_after(FAIL_SCALE * nth * nth * nth);
    }
    odin3_pp *pp = NULL;
    odin3_status st =
        odin3_pp_test_create(design, &config, limits_for(opts >> OPT_CAPS_SHIFT), &pp);
    if (st == ODIN3_OK) {
        run_once(pp, &src, injected);
        if (opts & OPT_TWICE) {
            run_once(pp, &src, false);
        }
    } else if (!(st == ODIN3_ERR_NO_MEMORY && injected)) {
        fail("odin3_pp_create failed", st);
    }
    odin3_util_set_alloc_fail_after(-1);
    odin3_pp_destroy(pp);
    odin3_design_destroy(design);
    odin3_log_set_sink(NULL, NULL);
    return 0;
}
