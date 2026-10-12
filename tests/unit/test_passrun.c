/*
 * test_passrun.c — the pass-run table's arguments and run information (3D Task 8; IR-12, spec
 * AB-25 §8.1 and §8.5): the manager's trimmed arguments, readers' own runs, odin3_passrun_set_info,
 * odin3_passrun_info_append, rows across compact, and the ABI accessors.
 */
#include "frontends/blif/reader.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { PATH_BUF = 512, MAX_ROWS = 16 };

/* The fixture every case reads: one module, read as t.blif from the test's own directory. */
static const char *const T_BLIF = ".model top\n.inputs a b\n.outputs y\n.names a b y\n11 1\n.end\n";

/* The run information the tests record (spec §8.5's form). */
static const char *const ABC_INFO =
    "script=a.abc sha256="
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    " abc=0123456789abcdef0123456789abcdef01234567 K=6 exchange=blif "
    "depth=3";

static odin3_design *design;
static odin3_strbuf log_text; /* "<E|W|I|D> <msg>\n" per delivered message */
static char test_dir[PATH_BUF];
static char start_dir[PATH_BUF];

static const char *const LEVEL_TAGS[ODIN3_LOG_LEVEL_COUNT] = {"E", "W", "I", "D"};

static void capture_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    (void)odin3_strbuf_appendf(&log_text, "%s %s\n", LEVEL_TAGS[level], msg);
}

/* Only the error lines of the log, in order. */
static const char *errors_of_log(odin3_strbuf *out) {
    odin3_strbuf_clear(out);
    const char *at = log_text.data != NULL ? log_text.data : "";
    while (*at != '\0') {
        const char *end = strchr(at, '\n');
        size_t len = end != NULL ? (size_t)(end - at) + 1 : strlen(at);
        if (at[0] == 'E') {
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, (odin3_bytes){at, len}));
        }
        at += len;
    }
    return out->data != NULL ? out->data : "";
}

/* The log's error lines are exactly `E <want>`. */
static void expect_only_error(const char *want) {
    odin3_strbuf errs;
    odin3_strbuf line;
    odin3_strbuf_init(&errs);
    odin3_strbuf_init(&line);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(&line, "E %s\n", want));
    TEST_ASSERT_EQUAL_STRING(line.data, errors_of_log(&errs));
    odin3_strbuf_free(&errs);
    odin3_strbuf_free(&line);
    odin3_strbuf_clear(&log_text);
}

static uint32_t intern(const char *text) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, odin3_bytes_cstr(text), &str));
    return str;
}

/* The text of strtab ID str (0 gives ""). */
static const char *str_of(uint32_t str) {
    const char *text = odin3_strtab_get(odin3_design_strtab(design), str);
    TEST_ASSERT_NOT_NULL(text);
    return text;
}

/* --- the test pass t_args: accepts the one word -x (finding 15) ----------------------------- */

static odin3_status t_args_pass(odin3_pass_ctx *ctx, odin3_design *des, odin3_bytes args,
                                void *user) {
    (void)ctx;
    (void)des;
    (void)user;
    odin3_bytes rest = args;
    odin3_bytes word = odin3_pass_arg_next(&rest);
    if (!odin3_pass_arg_is(word, "-x") || odin3_pass_arg_next(&rest).len != 0) {
        odin3_log(ODIN3_LOG_ERROR, "t_args: expected -x");
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

static const odin3_pass_def T_ARGS_PASS = {"t_args", "t_args -x: accepts -x",    t_args_pass, NULL,
                                           false,    ODIN3_PASS_PROCESSES_REFUSE};

static odin3_status run_p(const char *text) {
    return odin3_pass_run_script(design, odin3_bytes_cstr(text),
                                 (odin3_script_src){"-p", ODIN3_SCRIPT_BY_COMMAND});
}

/* --- fixture -------------------------------------------------------------------------------- */

void setUp(void) {
    odin3_strbuf_clear(&log_text);
    odin3_log_set_sink(capture_sink, NULL);
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
}

void tearDown(void) {
    odin3_design_destroy(design);
    design = NULL;
    odin3_log_set_sink(NULL, NULL);
}

/* One row of the table as stored. */
typedef struct row {
    uint32_t name, args, info;
} row;

static row row_of(uint32_t run) {
    odin3_passrun_id id = {run};
    return (row){odin3_passrun_name(design, id), odin3_passrun_args(design, id),
                 odin3_passrun_info(design, id)};
}

/* Opens a run named name with args (NULL: none) directly, as abc's tests do. */
static odin3_pass_ctx open_direct(const char *name, const char *args) {
    odin3_pass_ctx ctx = {0};
    uint32_t args_str = args != NULL ? intern(args) : 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run_begin_args(design, intern(name), args_str, &ctx));
    return ctx;
}

/* --- arguments ------------------------------------------------------------------------------ */

/* The script gives each run the text after its pass name, trimmed; blank arguments give 0. */
static void test_script_args(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_p("read_blif t.blif; stats   ; t_args -x  "));
    TEST_ASSERT_EQUAL_UINT32(4, odin3_passrun_end(design));
    row read = row_of(1);
    row stats = row_of(2);
    row targs = row_of(3);
    TEST_ASSERT_EQUAL_STRING("read_blif", str_of(read.name));
    TEST_ASSERT_EQUAL_STRING("t.blif", str_of(read.args));
    TEST_ASSERT_EQUAL_STRING("stats", str_of(stats.name));
    TEST_ASSERT_EQUAL_UINT32(0, stats.args);
    TEST_ASSERT_EQUAL_STRING("t_args", str_of(targs.name));
    TEST_ASSERT_EQUAL_STRING("-x", str_of(targs.args));
    TEST_ASSERT_EQUAL_UINT32(0, read.info);
    const char *name = NULL;
    const char *args = NULL;
    const char *info = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_passrun(design, 2, &name, &args, &info));
    TEST_ASSERT_EQUAL_STRING("stats", name);
    TEST_ASSERT_EQUAL_STRING("", args);
    TEST_ASSERT_EQUAL_STRING("", info);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_passrun(design, 3, &name, &args, &info));
    TEST_ASSERT_EQUAL_STRING("-x", args);
}

/* A pass run through odin3_pass_run (the ABI's odin3_design_run_pass) is trimmed the same way. */
static void test_direct_run_trims(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run(design, "t_args", odin3_bytes_cstr(" \t-x \r\n")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "stats", odin3_bytes_cstr("  \t ")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run(design, "stats", (odin3_bytes){NULL, 0}));
    TEST_ASSERT_EQUAL_STRING("-x", str_of(row_of(1).args));
    TEST_ASSERT_EQUAL_UINT32(0, row_of(2).args);
    TEST_ASSERT_EQUAL_UINT32(0, row_of(3).args);
}

/* Quoted words keep their quotes: the record is the text as the script gave it. */
static void test_args_kept_as_given(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_p("read_blif   \"t.blif\"  # the fixture"));
    TEST_ASSERT_EQUAL_STRING("\"t.blif\"", str_of(row_of(1).args));
}

/* A run whose pass fails keeps its row and its arguments, a one-byte argument included. */
static void test_failed_pass_keeps_args(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, run_p("t_args y"));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_passrun_end(design));
    TEST_ASSERT_EQUAL_STRING("y", str_of(row_of(1).args));
}

/* A reader that opens its own run (odin3_pass_run_begin) records no arguments. */
static void test_reader_own_run_has_no_args(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_blif_read(design, "t.blif"));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_passrun_end(design));
    TEST_ASSERT_EQUAL_STRING("read_blif", str_of(row_of(1).name));
    TEST_ASSERT_EQUAL_UINT32(0, row_of(1).args);
    TEST_ASSERT_EQUAL_UINT32(0, row_of(1).info);
}

/* begin_args: 0 is none; an ID past the strtab is refused and opens nothing. */
static void test_begin_args(void) {
    odin3_pass_ctx ctx = open_direct("p", "a b");
    TEST_ASSERT_EQUAL_UINT32(1, ctx.run.v);
    TEST_ASSERT_EQUAL_UINT32(0, ctx.op);
    TEST_ASSERT_TRUE(ctx.design == design);
    TEST_ASSERT_EQUAL_STRING("a b", str_of(row_of(1).args));
    uint32_t past = (uint32_t)odin3_strtab_count(odin3_design_strtab(design));
    odin3_pass_ctx bad = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_pass_run_begin_args(design, intern("p"), past + 1, &bad));
    expect_only_error("odin3_pass_run_begin_args: invalid argument args_str");
    TEST_ASSERT_EQUAL_UINT32(2, odin3_passrun_end(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pass_run_begin_args(design, 0, 0, &bad));
    TEST_ASSERT_EQUAL_UINT32(2, odin3_passrun_end(design));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_passrun_args(design, (odin3_passrun_id){0}));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_passrun_args(design, (odin3_passrun_id){2}));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_passrun_info(design, (odin3_passrun_id){2}));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_passrun_args(NULL, (odin3_passrun_id){1}));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_passrun_info(NULL, (odin3_passrun_id){1}));
}

/* --- run information ------------------------------------------------------------------------ */

/* Set once, read back through the IR and the ABI; a second time is refused, the first kept. */
static void test_set_info(void) {
    (void)open_direct("first", NULL);
    odin3_pass_ctx ctx = open_direct("abc", NULL);
    TEST_ASSERT_EQUAL_UINT32(2, ctx.run.v);
    uint32_t info = intern(ABC_INFO);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_passrun_set_info(&ctx, info));
    TEST_ASSERT_EQUAL_UINT32(info, odin3_passrun_info(design, ctx.run));
    TEST_ASSERT_EQUAL_UINT32(0, odin3_passrun_info(design, (odin3_passrun_id){1}));
    const char *name = NULL;
    const char *args = NULL;
    const char *text = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_passrun(design, 2, &name, &args, &text));
    TEST_ASSERT_EQUAL_STRING("abc", name);
    TEST_ASSERT_EQUAL_STRING("", args);
    TEST_ASSERT_EQUAL_STRING(ABC_INFO, text);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_passrun_set_info(&ctx, intern("K=4")));
    expect_only_error("odin3_passrun_set_info: run 2 already has information");
    TEST_ASSERT_EQUAL_UINT32(info, odin3_passrun_info(design, ctx.run));
}

/* Run 0 and a run past the table are refused by number. */
static void test_set_info_no_run(void) {
    odin3_pass_ctx ctx = open_direct("abc", NULL);
    odin3_pass_ctx none = {design, {0}, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_passrun_set_info(&none, intern("K=6")));
    expect_only_error("odin3_passrun_set_info: no run 0");
    odin3_pass_ctx past = {design, {2}, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_passrun_set_info(&past, intern("K=6")));
    expect_only_error("odin3_passrun_set_info: no run 2");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_passrun_set_info(NULL, intern("K=6")));
    expect_only_error("odin3_passrun_set_info: no pass context");
    TEST_ASSERT_EQUAL_UINT32(0, odin3_passrun_info(design, ctx.run));
}

/* info_str 0 and an ID past the strtab are refused; the run keeps no information. */
static void test_set_info_bad_str(void) {
    odin3_pass_ctx ctx = open_direct("abc", NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_passrun_set_info(&ctx, 0));
    expect_only_error("odin3_passrun_set_info: invalid argument info_str");
    uint32_t past = (uint32_t)odin3_strtab_count(odin3_design_strtab(design));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_passrun_set_info(&ctx, past));
    expect_only_error("odin3_passrun_set_info: invalid argument info_str");
    TEST_ASSERT_EQUAL_UINT32(0, odin3_passrun_info(design, ctx.run));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_passrun_set_info(&ctx, intern("K=6")));
}

/* --- the formatter -------------------------------------------------------------------------- */

static odin3_status append(odin3_strbuf *out, const char *key, const char *value) {
    return odin3_passrun_info_append(out, key, odin3_bytes_cstr(value));
}

static void test_info_append(void) {
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, append(&out, "script", "lib/abc/vtr.abc"));
    TEST_ASSERT_EQUAL_STRING("script=lib/abc/vtr.abc", out.data);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, append(&out, "K", "6"));
    TEST_ASSERT_EQUAL_STRING("script=lib/abc/vtr.abc K=6", out.data);
    odin3_strbuf_clear(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, append(&out, "x", "a b\"c\\d"));
    TEST_ASSERT_EQUAL_STRING("x=\"a b\\\"c\\\\d\"", out.data);
    odin3_strbuf_clear(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, append(&out, "t", "a\tb"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, append(&out, "q", "\""));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, append(&out, "e", ""));
    TEST_ASSERT_EQUAL_STRING("t=\"a\tb\" q=\"\\\"\" e=", out.data);
    odin3_strbuf_clear(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(&out, odin3_bytes_cstr("a")));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, append(&out, "K", "6"));
    TEST_ASSERT_EQUAL_STRING("a K=6", out.data); /* the blank after a one-byte buffer too */
    odin3_strbuf_free(&out);
}

/* I1: every byte outside `!`..`~` quotes the value (Python's str.isspace() splits on VT, FF,
 * 0x1c-0x1f, U+0085 and U+00A0); DEL and `~`/`!` at the edges. */
static void test_info_append_quotes_non_printing(void) {
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    /* each escape is followed by `z`, which is not a hex digit */
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, append(&out, "nbsp", "a\xc2\xa0z.abc"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, append(&out, "vt", "a\x0bz"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, append(&out, "us", "a\x1fz"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, append(&out, "del", "a\x7fz"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, append(&out, "edges", "!~"));
    TEST_ASSERT_EQUAL_STRING("nbsp=\"a\xc2\xa0z.abc\" vt=\"a\x0bz\" us=\"a\x1fz\" del=\"a\x7fz\" "
                             "edges=!~",
                             out.data);
    odin3_strbuf_free(&out);
}

/* Out of memory at every allocation of a growing quoted append leaves the buffer as it was. */
static void test_info_append_out_of_memory(void) {
    const char value[] =
        "a b c d e f g h i j k l m n o p q r s t u v w x y z \" A B C D E F G H I J K "
        "L M N O P Q R S T U V W X Y Z \\ 0 1 2 3 4 5 6 7 8 9 a b c d e f g h i j k";
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, append(&out, "K", "6"));
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    long failed = 0;
    for (long after = 0; st == ODIN3_ERR_NO_MEMORY && after < 64; after++) {
        odin3_util_set_alloc_fail_after(after);
        st = append(&out, "x", value);
        odin3_util_set_alloc_fail_after(-1);
        if (st != ODIN3_OK) {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            TEST_ASSERT_EQUAL_STRING("K=6", out.data);
            TEST_ASSERT_EQUAL_size_t(3, out.len);
            failed++;
        }
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    TEST_ASSERT_GREATER_OR_EQUAL_INT32(2, failed); /* the growth fails mid-word at least once */
    TEST_ASSERT_EQUAL_INT(0, strncmp(out.data, "K=6 x=\"a b c", strlen("K=6 x=\"a b c")));
    odin3_strbuf_free(&out);
}

/* Refusals leave the buffer as it was. */
static void test_info_append_refusals(void) {
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, append(&out, "K", "6"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, append(&out, "", "1"));
    expect_only_error("odin3_passrun_info_append: invalid argument key");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, append(&out, "a b", "1"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, append(&out, "a=b", "1"));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, append(&out, NULL, "1"));
    odin3_strbuf_clear(&log_text);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, append(&out, "s", "a\nb"));
    expect_only_error("odin3_passrun_info_append: invalid argument value");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, append(&out, "s", "a\rb"));
    expect_only_error("odin3_passrun_info_append: invalid argument value");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_passrun_info_append(&out, "s", (odin3_bytes){"a\0b", 3}));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_passrun_info_append(&out, "s", (odin3_bytes){NULL, 1}));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, append(NULL, "s", "1"));
    TEST_ASSERT_EQUAL_STRING("K=6", out.data);
    TEST_ASSERT_EQUAL_size_t(3, out.len);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_passrun_info_append(&out, "n", (odin3_bytes){NULL, 0}));
    TEST_ASSERT_EQUAL_STRING("K=6 n=", out.data);
    odin3_strbuf_free(&out);
}

/* --- compact and the ABI -------------------------------------------------------------------- */

/* compact adds its own row and changes none of the earlier ones. */
static void test_compact_keeps_rows(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_p("read_blif t.blif; t_args -x"));
    odin3_pass_ctx ctx = open_direct("abc", "--lut 6");
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_passrun_set_info(&ctx, intern(ABC_INFO)));
    uint32_t end = odin3_passrun_end(design);
    TEST_ASSERT_TRUE(end <= MAX_ROWS);
    row before[MAX_ROWS] = {{0}};
    for (uint32_t i = 1; i < end; i++) {
        before[i] = row_of(i);
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_p("compact"));
    TEST_ASSERT_EQUAL_UINT32(end + 1, odin3_passrun_end(design));
    for (uint32_t i = 1; i < end; i++) {
        row after = row_of(i);
        TEST_ASSERT_EQUAL_UINT32(before[i].name, after.name);
        TEST_ASSERT_EQUAL_UINT32(before[i].args, after.args);
        TEST_ASSERT_EQUAL_UINT32(before[i].info, after.info);
    }
    TEST_ASSERT_EQUAL_STRING("compact", str_of(row_of(end).name));
    TEST_ASSERT_EQUAL_STRING(ABC_INFO, str_of(row_of(end - 1).info));
}

/* The count is one past the last run; run 0 and run = count are refused, outputs unchanged. */
static void test_abi_count_and_bounds(void) {
    uint32_t count = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_passrun_count(design, &count));
    TEST_ASSERT_EQUAL_UINT32(1, count);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_p("read_blif t.blif; stats"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_passrun_count(design, &count));
    TEST_ASSERT_EQUAL_UINT32(odin3_passrun_end(design), count);
    TEST_ASSERT_EQUAL_UINT32(3, count);
    const char *keep = "keep";
    const char *name = keep;
    const char *args = keep;
    const char *info = keep;
    odin3_strbuf_clear(&log_text);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_get_passrun(design, 0, &name, &args, &info));
    expect_only_error("odin3_design_get_passrun: invalid argument (a NULL pointer, an unknown "
                      "module, an ID or index out of range, or a bad kind)");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_get_passrun(design, count, &name, &args, &info));
    TEST_ASSERT_TRUE(name == keep && args == keep && info == keep);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_get_passrun(design, 1, NULL, &args, &info));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_get_passrun(design, 1, &name, NULL, &info));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_get_passrun(design, 1, &name, &args, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_design_get_passrun(NULL, 1, &name, &args, &info));
    TEST_ASSERT_TRUE(name == keep && args == keep && info == keep);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_get_passrun_count(NULL, &count));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_design_get_passrun_count(design, NULL));
    TEST_ASSERT_EQUAL_UINT32(3, count);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_passrun(design, 1, &name, &args, &info));
    TEST_ASSERT_EQUAL_STRING("read_blif", name);
    TEST_ASSERT_EQUAL_STRING("t.blif", args);
    TEST_ASSERT_EQUAL_STRING("", info);
}

/* Rows never change: fetched again after later passes (as the conventions say), a row reads the
 * same. */
static void test_abi_rows_unchanged_by_passes(void) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_p("read_blif t.blif"));
    const char *name = NULL;
    const char *args = NULL;
    const char *info = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, run_p("t_args -x; compact; stats; t_args -x"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_passrun(design, 1, &name, &args, &info));
    TEST_ASSERT_EQUAL_STRING("read_blif", name);
    TEST_ASSERT_EQUAL_STRING("t.blif", args);
    TEST_ASSERT_EQUAL_STRING("", info);
}

/* --- main ----------------------------------------------------------------------------------- */

/* Writes T_BLIF to t.blif in the working directory; false on failure. */
static bool write_t_blif(void) {
    FILE *file = fopen("t.blif", "w");
    if (file == NULL) {
        return false;
    }
    bool wrote = fwrite(T_BLIF, strlen(T_BLIF), 1, file) == 1;
    return fclose(file) == 0 && wrote;
}

/* Makes a fresh directory under the working directory, changes into it and writes t.blif there
 * (outside any test, so no Unity assertion); false on failure. */
static bool enter_test_dir(void) {
    static const char pattern[] = "passrun.XXXXXX";
    _Static_assert(sizeof pattern <= sizeof test_dir, "the pattern fits");
    memcpy(test_dir, pattern, sizeof pattern);
    return getcwd(start_dir, sizeof start_dir) != NULL && mkdtemp(test_dir) != NULL &&
           chdir(test_dir) == 0 && write_t_blif();
}

/* Removes t.blif and the directory; false on failure. */
static bool leave_test_dir(void) {
    return unlink("t.blif") == 0 && chdir(start_dir) == 0 && rmdir(test_dir) == 0;
}

int main(void) {
    odin3_strbuf_init(&log_text);
    if (odin3_pass_register_def(&T_ARGS_PASS) != ODIN3_OK || !enter_test_dir()) {
        return EXIT_FAILURE;
    }
    UNITY_BEGIN();
    RUN_TEST(test_script_args);
    RUN_TEST(test_direct_run_trims);
    RUN_TEST(test_args_kept_as_given);
    RUN_TEST(test_failed_pass_keeps_args);
    RUN_TEST(test_reader_own_run_has_no_args);
    RUN_TEST(test_begin_args);
    RUN_TEST(test_set_info);
    RUN_TEST(test_set_info_no_run);
    RUN_TEST(test_set_info_bad_str);
    RUN_TEST(test_info_append);
    RUN_TEST(test_info_append_quotes_non_printing);
    RUN_TEST(test_info_append_out_of_memory);
    RUN_TEST(test_info_append_refusals);
    RUN_TEST(test_compact_keeps_rows);
    RUN_TEST(test_abi_count_and_bounds);
    RUN_TEST(test_abi_rows_unchanged_by_passes);
    int failures = UNITY_END();
    odin3_strbuf_free(&log_text);
    return leave_test_dir() ? failures : EXIT_FAILURE;
}
