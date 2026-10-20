/*
 * test_hdl_loc.c — unit tests for the location text, the name decoder and the writers' location
 * filter (spec 5.4: L-1 to L-6, W-1 to W-5, T-1).
 */
#include "backends/common/hdl_loc.h"
#include "ir/design.h"
#include "ir/prov.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    RANDOM_LOCS = 100000,
    NAME_BUF = 64,
    LCG_MUL = 1103515245,
    LCG_ADD = 12345,
    LCG_SEED = 20261020,
    RAND_SHIFT = 8,
    FILE_CHOICES = 8,
    NUM_RANGE = 1000,
    MANY = 12,
    WARN_MAX = 8,
    WARN_LINE = 256
};

static odin3_design *design;
static char warnings[WARN_MAX][WARN_LINE];
static unsigned warn_count;

static void sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (level == ODIN3_LOG_WARN && warn_count < WARN_MAX) {
        (void)snprintf(warnings[warn_count], WARN_LINE, "%s", msg);
        warn_count++;
    }
}

void setUp(void) {
    design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(design);
    warn_count = 0;
    odin3_log_set_sink(sink, NULL);
}

void tearDown(void) {
    odin3_log_set_sink(NULL, NULL);
    odin3_design_destroy(design);
    design = NULL;
}

static odin3_bytes text_of(const char *text) {
    return odin3_bytes_cstr(text);
}

static bool file_is(const odin3_hdl_loc *loc, const char *want) {
    return loc->file.len == strlen(want) && memcmp(loc->file.ptr, want, loc->file.len) == 0;
}

static void expect_one(const char *text, odin3_hdl_loc want) {
    odin3_hdl_loc loc[2];
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, odin3_hdl_loc_parse(text_of(text), loc, 2), text);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(want.file.len, loc[0].file.len, text);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want.file.ptr, loc[0].file.ptr, want.file.len, text);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(want.line, loc[0].line, text);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(want.col, loc[0].col, text);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(want.end_line, loc[0].end_line, text);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(want.end_col, loc[0].end_col, text);
}

static void expect_none(const char *text) {
    odin3_hdl_loc loc[2];
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, odin3_hdl_loc_parse(text_of(text), loc, 2), text);
}

/* --- L-1: the forms ------------------------------------------------------------------------- */

static void test_forms(void) {
    expect_one("a.v:12", (odin3_hdl_loc){text_of("a.v"), 12, 0, 0, 0});
    expect_one("a.v:12.3", (odin3_hdl_loc){text_of("a.v"), 12, 3, 0, 0});
    expect_one("a.v:3.1-4.10", (odin3_hdl_loc){text_of("a.v"), 3, 1, 4, 10});
    expect_one("dir/sub/a.v:1.1-1.2", (odin3_hdl_loc){text_of("dir/sub/a.v"), 1, 1, 1, 2});
}

/* L-2: the file is every byte before the tail, whatever they are. */
static void test_odd_file_names(void) {
    expect_one("a:b.v:3", (odin3_hdl_loc){text_of("a:b.v"), 3, 0, 0, 0});
    expect_one("a-b.v:3.4", (odin3_hdl_loc){text_of("a-b.v"), 3, 4, 0, 0});
    expect_one("a.b.c.v:3", (odin3_hdl_loc){text_of("a.b.c.v"), 3, 0, 0, 0});
    expect_one("x@y.v:5", (odin3_hdl_loc){text_of("x@y.v"), 5, 0, 0, 0});
    expect_one("my dir/a b.v:7", (odin3_hdl_loc){text_of("my dir/a b.v"), 7, 0, 0, 0});
    expect_one("C:\\x\\a.v:3.1-4.2", (odin3_hdl_loc){text_of("C:\\x\\a.v"), 3, 1, 4, 2});
    expect_one("a.v:1:2", (odin3_hdl_loc){text_of("a.v:1"), 2, 0, 0, 0});
    expect_one("1.2-3.4:5", (odin3_hdl_loc){text_of("1.2-3.4"), 5, 0, 0, 0});
    expect_one("x|y.v:3", (odin3_hdl_loc){text_of("x|y.v"), 3, 0, 0, 0});
}

/* L-3 and L-6: nothing partial. */
static void test_non_matches(void) {
    expect_none("");
    expect_none("a.v");
    expect_none("a.v:");
    expect_none(":3");
    expect_none("a.v:0");
    expect_none("a.v:x");
    expect_none("a.v:3.");
    expect_none("a.v:3.1-4");
    expect_none("a.v:3.1-4.");
    expect_none("a.v:-3");
    expect_none("a.v:+3");
    expect_none("a.v:3 ");
    expect_none("<<interf-bypass>>");
    expect_none("a.v:3|garbage");
    expect_none("a.v:3|");
    expect_none("a.v:4294967296");
    expect_none("a.v:3.4294967296");
    expect_none("a.v:99999999999999999999");
}

static void test_uint32_max(void) {
    expect_one("a.v:4294967295.4294967295-4294967295.4294967295",
               (odin3_hdl_loc){text_of("a.v"), UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX});
}

static void test_end_before_start_is_kept(void) {
    expect_one("a.v:9.9-1.1", (odin3_hdl_loc){text_of("a.v"), 9, 9, 1, 1});
}

/* --- L-2: splitting ------------------------------------------------------------------------- */

static void test_split(void) {
    odin3_hdl_loc loc[4];
    TEST_ASSERT_EQUAL_UINT(3,
                           odin3_hdl_loc_parse(text_of("a.v:3.1-4.10|b.v:5.2-5.3|c.v:6"), loc, 4));
    TEST_ASSERT_TRUE(file_is(&loc[0], "a.v"));
    TEST_ASSERT_EQUAL_UINT(4, loc[0].end_line);
    TEST_ASSERT_TRUE(file_is(&loc[1], "b.v"));
    TEST_ASSERT_EQUAL_UINT(5, loc[1].line);
    TEST_ASSERT_TRUE(file_is(&loc[2], "c.v"));
    TEST_ASSERT_EQUAL_UINT(6, loc[2].line);
}

/* A `|` splits only after something that ends like a location. */
static void test_split_only_after_a_tail(void) {
    odin3_hdl_loc loc[4];
    TEST_ASSERT_EQUAL_UINT(2, odin3_hdl_loc_parse(text_of("a.v:3|b|c.v:4"), loc, 4));
    TEST_ASSERT_TRUE(file_is(&loc[1], "b|c.v"));
    TEST_ASSERT_EQUAL_UINT(1, odin3_hdl_loc_parse(text_of("a|b.v:4"), loc, 4));
    TEST_ASSERT_TRUE(file_is(&loc[0], "a|b.v"));
}

/* D-7: the count is the whole text's, the storage is the cap's. */
static void test_count_over_cap(void) {
    char text[NAME_BUF * 4] = "";
    odin3_hdl_loc loc[ODIN3_HDL_LOC_MAX];
    for (uint32_t i = 0; i < MANY; i++) {
        char one[NAME_BUF];
        (void)snprintf(one, sizeof one, "%sf%u.v:%u", i > 0 ? "|" : "", (unsigned)i,
                       (unsigned)i + 1);
        (void)strcat(text, one);
    }
    TEST_ASSERT_EQUAL_UINT(MANY, odin3_hdl_loc_parse(text_of(text), loc, ODIN3_HDL_LOC_MAX));
    TEST_ASSERT_EQUAL_UINT(ODIN3_HDL_LOC_MAX, loc[ODIN3_HDL_LOC_MAX - 1].line);
    TEST_ASSERT_EQUAL_UINT(MANY, odin3_hdl_loc_parse(text_of(text), NULL, 0));
}

/* --- W-4: printing -------------------------------------------------------------------------- */

static void expect_print(odin3_hdl_loc loc, const char *want) {
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_loc_print(&loc, 1, &out));
    TEST_ASSERT_EQUAL_STRING(want, out.data);
    odin3_strbuf_free(&out);
}

static void test_print_forms(void) {
    odin3_bytes file = text_of("a.v");
    expect_print((odin3_hdl_loc){file, 12, 0, 0, 0}, "a.v:12");
    expect_print((odin3_hdl_loc){file, 12, 3, 0, 0}, "a.v:12.3");
    expect_print((odin3_hdl_loc){file, 3, 1, 4, 10}, "a.v:3.1-4.10");
    expect_print((odin3_hdl_loc){file, 3, 0, 4, 10}, "a.v:3.0-4.10");
}

static void test_print_many_and_none(void) {
    odin3_hdl_loc locs[2] = {{text_of("a.v"), 1, 2, 3, 4}, {text_of("b.v"), 5, 0, 0, 0}};
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_loc_print(locs, 2, &out));
    TEST_ASSERT_EQUAL_STRING("a.v:1.2-3.4|b.v:5", out.data);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_loc_print(locs, 0, &out));
    TEST_ASSERT_EQUAL_STRING("a.v:1.2-3.4|b.v:5", out.data);
    odin3_strbuf_free(&out);
}

static uint32_t rng_state = LCG_SEED;

static uint32_t rng_next(void) {
    rng_state = rng_state * LCG_MUL + LCG_ADD;
    return rng_state >> RAND_SHIFT;
}

/* A random location of the grammar: every form, files with odd (but not `|`-splitting) bytes. */
static odin3_hdl_loc random_loc(void) {
    static const char *const FILES[FILE_CHOICES] = {"a.v",      "dir/x.v", "a:b.v",      "a-b.1.v",
                                                    "my dir/q", "x@y.v",   "C:\\w\\z.v", "9"};
    odin3_hdl_loc loc = {text_of(FILES[rng_next() % FILE_CHOICES]), 1 + rng_next() % NUM_RANGE, 0,
                         0, 0};
    uint32_t form = rng_next() % 3;
    if (rng_next() % 16 == 0) {
        loc.line = UINT32_MAX - rng_next() % 3;
    }
    if (form >= 1) {
        loc.col = 1 + rng_next() % NUM_RANGE;
    }
    if (form == 2) {
        loc.end_line = 1 + rng_next() % NUM_RANGE;
        loc.end_col = rng_next() % NUM_RANGE;
    }
    return loc;
}

static bool same(const odin3_hdl_loc *lhs, const odin3_hdl_loc *rhs) {
    return lhs->file.len == rhs->file.len &&
           memcmp(lhs->file.ptr, rhs->file.ptr, lhs->file.len) == 0 && lhs->line == rhs->line &&
           lhs->col == rhs->col && lhs->end_line == rhs->end_line && lhs->end_col == rhs->end_col;
}

/* Print then parse is the identity (T-1), over 100,000 seeded locations in groups of 1 to 4. */
static void test_print_parse_identity(void) {
    odin3_strbuf out;
    odin3_strbuf_init(&out);
    rng_state = LCG_SEED;
    for (uint32_t done = 0; done < RANDOM_LOCS;) {
        odin3_hdl_loc in[4];
        odin3_hdl_loc back[4];
        uint32_t count = 1 + rng_next() % 4;
        for (uint32_t i = 0; i < count; i++) {
            in[i] = random_loc();
        }
        odin3_strbuf_clear(&out);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_loc_print(in, count, &out));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(
            count, odin3_hdl_loc_parse((odin3_bytes){out.data, out.len}, back, 4), out.data);
        for (uint32_t i = 0; i < count; i++) {
            TEST_ASSERT_TRUE_MESSAGE(same(&in[i], &back[i]), out.data);
        }
        done += count;
    }
    odin3_strbuf_free(&out);
}

/* --- L-5: names ----------------------------------------------------------------------------- */

static bool bytes_are(odin3_bytes got, const char *want) {
    return got.len == strlen(want) && (got.len == 0 || memcmp(got.ptr, want, got.len) == 0);
}

/* What a name decodes to; suffix is -1 for none. */
typedef struct name_case {
    const char *name;
    const char *hier;
    const char *short_name;
    const char *file;
    uint32_t line;
    int suffix;
} name_case;

/* The suffix as the cases give it: -1 for none. */
static int suffix_of(const odin3_hdl_name *name) {
    return name->has_suffix ? (int)name->suffix : -1;
}

static void expect_split(const name_case *want, const odin3_hdl_name *got) {
    TEST_ASSERT_TRUE_MESSAGE(bytes_are(got->hier, want->hier), want->name);
    TEST_ASSERT_TRUE_MESSAGE(bytes_are(got->short_name, want->short_name), want->name);
}

static void expect_name(const name_case *want) {
    odin3_hdl_name got;
    TEST_ASSERT_TRUE_MESSAGE(odin3_hdl_name_decode(text_of(want->name), &got), want->name);
    expect_split(want, &got);
    TEST_ASSERT_TRUE_MESSAGE(file_is(&got.loc, want->file), want->name);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(want->line, got.loc.line, want->name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(want->suffix, suffix_of(&got), want->name);
}

static void test_name_basic(void) {
    expect_name(&(name_case){"top/u1/x@a.v:12", "top/u1", "x", "a.v", 12, -1});
    expect_name(&(name_case){"n@t.blif:4", "", "n", "t.blif", 4, -1});
}

/* The `$N` is the writer's, not the location's; `$n7` as a short name is not a suffix. */
static void test_name_suffix(void) {
    expect_name(&(name_case){"top/x@a.v:12$3", "top", "x", "a.v", 12, 3});
    expect_name(&(name_case){"top/$n7@a.v:12", "top", "$n7", "a.v", 12, -1});
    expect_name(&(name_case){"a$1@b.v:2", "", "a$1", "b.v", 2, -1});
}

/* Split at the last `/` before the last `@`: a `/` in the writer's short name moves into hier. */
static void test_name_slash_in_short(void) {
    expect_name(&(name_case){"top/a/b@a.v:3", "top/a", "b", "a.v", 3, -1});
    expect_name(&(name_case){"dir/x@y/a.v:3", "dir", "x", "y/a.v", 3, -1});
    expect_name(&(name_case){"/x@a.v:3", "", "x", "a.v", 3, -1});
}

static void expect_name_none(const char *name) {
    odin3_hdl_name decoded;
    TEST_ASSERT_FALSE_MESSAGE(odin3_hdl_name_decode(text_of(name), &decoded), name);
}

static void test_name_non_matches(void) {
    expect_name_none("");
    expect_name_none("plain");
    expect_name_none("x@");
    expect_name_none("@a.v:3");
    expect_name_none("top/@a.v:3");
    expect_name_none("x@a.v");
    expect_name_none("x@a.v:");
    expect_name_none("x@a.v:3.1");
    expect_name_none("x@a.v:3.1-4.2");
    expect_name_none("x@a.v:0");
    expect_name_none("x@a.v:3$");
    expect_name_none("x@a.v:3$x");
    expect_name_none("x@a.v:4294967296");
    expect_name_none("x@:3");
}

static void expect_vtr(const char *written, uint32_t line, const char *short_name) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    odin3_hdl_name name;
    bool matched = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_hdl_name_vtr_decode(text_of(written), &buf, &name, &matched));
    TEST_ASSERT_TRUE_MESSAGE(matched, written);
    TEST_ASSERT_TRUE_MESSAGE(bytes_are(name.short_name, short_name), written);
    TEST_ASSERT_EQUAL_UINT(line, name.loc.line);
    odin3_strbuf_free(&buf);
}

static void expect_vtr_none(const char *written) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    odin3_hdl_name name;
    bool matched = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_hdl_name_vtr_decode(text_of(written), &buf, &name, &matched));
    TEST_ASSERT_FALSE_MESSAGE(matched, written);
    odin3_strbuf_free(&buf);
}

/* VB-10: `%20` is a blank, a leading `%` before a non-escape is the reserved prefix. */
static void test_name_vtr_decode(void) {
    expect_vtr("top/n@my%20dir/t.blif:7", 7, "n");
    expect_vtr("%fe@a.v:3", 3, "fe");
    expect_vtr("%2Ex@a.v:3", 3, ".x");
    expect_vtr("a%25b@t.v:1", 1, "a%b");
    expect_vtr("a%2b@t.v:1", 1, "a%2b");
    expect_vtr("x@a.v:3$2", 3, "x");
    expect_vtr_none("plain");
    expect_vtr_none("%");
    expect_vtr_none("");
}

static void test_name_vtr_decode_keeps_file_bytes(void) {
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    odin3_hdl_name name;
    bool matched = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_name_vtr_decode(text_of("n@my%20dir/t%23.blif:7"),
                                                              &buf, &name, &matched));
    TEST_ASSERT_TRUE(matched);
    TEST_ASSERT_TRUE(file_is(&name.loc, "my dir/t#.blif"));
    odin3_strbuf_free(&buf);
}

/* --- W-2, W-3: the HDL locations of an object ------------------------------------------------- */

static uint32_t intern(const char *text) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(design, text_of(text), &str));
    return str;
}

static odin3_pass_ctx begin_run(void) {
    odin3_pass_ctx ctx;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pass_run_begin(design, intern("probe"), &ctx));
    return ctx;
}

static odin3_prov_id leaf_of_kind(odin3_pass_ctx *ctx, bool imported, odin3_srcloc loc) {
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_id id = {0};
    odin3_status st =
        imported ? odin3_prov_imported(ctx, &origin, &id) : odin3_prov_source(ctx, &origin, &id);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    return id;
}

static odin3_srcloc at(const char *file, uint32_t line) {
    return (odin3_srcloc){intern(file), line, 2, line, 9, 0};
}

static odin3_prov_id derive(odin3_pass_ctx *ctx, const odin3_prov_id *parents, uint32_t n) {
    odin3_prov_id id = {0};
    odin3_prov_begin_op(ctx);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_derive(ctx, (odin3_prov_list){parents, n}, &id));
    return id;
}

static odin3_hdl_ctx *make_ctx(void) {
    odin3_hdl_ctx *ctx = odin3_hdl_ctx_create(design, "test");
    TEST_ASSERT_NOT_NULL(ctx);
    return ctx;
}

static void test_locs_source_leaf(void) {
    odin3_pass_ctx run = begin_run();
    odin3_prov_id src = leaf_of_kind(&run, false, at("a.v", 7));
    odin3_hdl_ctx *ctx = make_ctx();
    odin3_hdl_locs_out out;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_locs(ctx, src, &out));
    TEST_ASSERT_EQUAL_UINT(1, out.n);
    TEST_ASSERT_EQUAL_UINT(7, out.locs[0].line);
    TEST_ASSERT_EQUAL_UINT(2, out.locs[0].col);
    TEST_ASSERT_EQUAL_UINT(9, out.locs[0].end_col);
    TEST_ASSERT_FALSE(out.capped);
    odin3_hdl_ctx_destroy(ctx);
}

/* T-1, W-2: an IMPORTED leaf is never an HDL location. */
static void test_locs_imported_never(void) {
    odin3_pass_ctx run = begin_run();
    odin3_prov_id imp = leaf_of_kind(&run, true, at("d.blif", 3));
    odin3_hdl_ctx *ctx = make_ctx();
    odin3_hdl_locs_out out;
    odin3_srcloc first;
    bool found = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_locs(ctx, imp, &out));
    TEST_ASSERT_EQUAL_UINT(0, out.n);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_first(ctx, imp, &first, &found));
    TEST_ASSERT_FALSE(found);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_locs(ctx, (odin3_prov_id){0}, &out));
    TEST_ASSERT_EQUAL_UINT(0, out.n);
    odin3_hdl_ctx_destroy(ctx);
}

/* W-3: a DERIVED record of three SOURCE parents gives three locations in parent order, deduped. */
static void test_locs_derived_parent_order(void) {
    odin3_pass_ctx run = begin_run();
    odin3_prov_id parents[4] = {
        leaf_of_kind(&run, false, at("c.v", 3)), leaf_of_kind(&run, false, at("a.v", 1)),
        leaf_of_kind(&run, false, at("b.v", 2)), leaf_of_kind(&run, false, at("a.v", 1))};
    odin3_prov_id derived = derive(&run, parents, 4);
    odin3_hdl_ctx *ctx = make_ctx();
    odin3_hdl_locs_out out;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_locs(ctx, derived, &out));
    TEST_ASSERT_EQUAL_UINT(3, out.n);
    TEST_ASSERT_EQUAL_UINT(3, out.locs[0].line);
    TEST_ASSERT_EQUAL_UINT(1, out.locs[1].line);
    TEST_ASSERT_EQUAL_UINT(2, out.locs[2].line);
    odin3_hdl_ctx_destroy(ctx);
}

/* The IMPORTED parent is skipped wherever it stands; the first HDL leaf is still first. */
static void test_locs_imported_first_parent(void) {
    odin3_pass_ctx run = begin_run();
    odin3_prov_id parents[2] = {leaf_of_kind(&run, true, at("d.blif", 3)),
                                leaf_of_kind(&run, false, at("a.v", 8))};
    odin3_prov_id derived = derive(&run, parents, 2);
    odin3_hdl_ctx *ctx = make_ctx();
    odin3_srcloc first;
    bool found = false;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_first(ctx, derived, &first, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT(8, first.line);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_first(ctx, derived, &first, &found));
    TEST_ASSERT_TRUE(found);
    odin3_hdl_ctx_destroy(ctx);
}

/* W-3: the cap is 8; the first 8 are kept and `capped` says so (X-3). */
static void test_locs_cap(void) {
    odin3_pass_ctx run = begin_run();
    odin3_prov_id parents[MANY];
    for (uint32_t i = 0; i < MANY; i++) {
        parents[i] = leaf_of_kind(&run, false, at("a.v", i + 1));
    }
    odin3_prov_id derived = derive(&run, parents, MANY);
    odin3_hdl_ctx *ctx = make_ctx();
    odin3_hdl_locs_out out;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_locs(ctx, derived, &out));
    TEST_ASSERT_EQUAL_UINT(ODIN3_HDL_LOC_MAX, out.n);
    TEST_ASSERT_TRUE(out.capped);
    TEST_ASSERT_EQUAL_UINT(ODIN3_HDL_LOC_MAX, out.locs[ODIN3_HDL_LOC_MAX - 1].line);
    odin3_hdl_ctx_destroy(ctx);
}

/* A record with several locations gives each (one per node it covers). */
static void test_locs_many_in_one_record(void) {
    odin3_pass_ctx run = begin_run();
    odin3_srcloc locs[2] = {at("a.v", 4), at("b.v", 5)};
    odin3_prov_origin origin = {locs, 2, 0, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&run, &origin, &id));
    odin3_hdl_ctx *ctx = make_ctx();
    odin3_hdl_locs_out out;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_locs(ctx, id, &out));
    TEST_ASSERT_EQUAL_UINT(2, out.n);
    odin3_hdl_ctx_destroy(ctx);
}

/* W-5, X-4: a file with a byte outside 0x20..0x7e is skipped with one WARNING per file. */
static void test_locs_unwritable_file_warns_once(void) {
    odin3_pass_ctx run = begin_run();
    odin3_prov_id bad = leaf_of_kind(&run, false, at("a\tb.v", 1));
    odin3_prov_id bad2 = leaf_of_kind(&run, false, at("a\tb.v", 2));
    odin3_prov_id good = leaf_of_kind(&run, false, at("ok.v", 3));
    odin3_prov_id parents[3] = {bad, bad2, good};
    odin3_prov_id derived = derive(&run, parents, 3);
    odin3_hdl_ctx *ctx = make_ctx();
    odin3_hdl_locs_out out;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_locs(ctx, derived, &out));
    TEST_ASSERT_EQUAL_UINT(1, out.n);
    TEST_ASSERT_EQUAL_UINT(3, out.locs[0].line);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_locs(ctx, bad, &out));
    TEST_ASSERT_EQUAL_UINT(0, out.n);
    TEST_ASSERT_EQUAL_UINT(1, warn_count);
    TEST_ASSERT_NOT_NULL(strstr(warnings[0], "test: source file name 'a?b.v'"));
    odin3_hdl_ctx_destroy(ctx);
}

static void test_file_printable(void) {
    TEST_ASSERT_TRUE(odin3_hdl_file_printable(design, intern("a b/c.v")));
    TEST_ASSERT_FALSE(odin3_hdl_file_printable(design, intern("a\nb")));
    TEST_ASSERT_FALSE(odin3_hdl_file_printable(design, intern("\xc3\xa9.v")));
}

static void test_file_printable_zero_is_none(void) {
    TEST_ASSERT_FALSE(odin3_hdl_file_printable(design, 0));
}

/* --- the text a writer prints ----------------------------------------------------------------- */

static void src_text_of(const odin3_hdl_locs_out *locs, bool quote_free, const char *want,
                        uint32_t want_n) {
    odin3_hdl_ctx *ctx = make_ctx();
    odin3_strbuf out;
    uint32_t written = 0;
    odin3_strbuf_init(&out);
    odin3_hdl_print_req req = {locs, quote_free};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_hdl_src_text(ctx, &req, &out, &written));
    TEST_ASSERT_EQUAL_UINT(want_n, written);
    TEST_ASSERT_EQUAL_STRING(want, out.data != NULL ? out.data : "");
    odin3_strbuf_free(&out);
    odin3_hdl_ctx_destroy(ctx);
}

static void test_src_text_forms_and_skips(void) {
    odin3_hdl_locs_out locs = {{at("a.v", 3),
                                {intern("b.v"), 5, 0, 0, 0, 0},
                                {intern("x|y.v"), 6, 1, 0, 0, 0},
                                at("q\"r.v", 7)},
                               4,
                               false};
    src_text_of(&locs, false, "a.v:3.2-3.9|b.v:5|q\"r.v:7.2-7.9", 3);
    TEST_ASSERT_EQUAL_UINT(1, warn_count);
    warn_count = 0;
    src_text_of(&locs, true, "a.v:3.2-3.9|b.v:5", 2);
    TEST_ASSERT_EQUAL_UINT(2, warn_count);
}

static void test_src_text_empty(void) {
    odin3_hdl_locs_out locs = {{{0}}, 0, false};
    src_text_of(&locs, false, "", 0);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_forms);
    RUN_TEST(test_odd_file_names);
    RUN_TEST(test_non_matches);
    RUN_TEST(test_uint32_max);
    RUN_TEST(test_end_before_start_is_kept);
    RUN_TEST(test_split);
    RUN_TEST(test_split_only_after_a_tail);
    RUN_TEST(test_count_over_cap);
    RUN_TEST(test_print_forms);
    RUN_TEST(test_print_many_and_none);
    RUN_TEST(test_print_parse_identity);
    RUN_TEST(test_name_basic);
    RUN_TEST(test_name_suffix);
    RUN_TEST(test_name_slash_in_short);
    RUN_TEST(test_name_non_matches);
    RUN_TEST(test_name_vtr_decode);
    RUN_TEST(test_name_vtr_decode_keeps_file_bytes);
    RUN_TEST(test_locs_source_leaf);
    RUN_TEST(test_locs_imported_never);
    RUN_TEST(test_locs_derived_parent_order);
    RUN_TEST(test_locs_imported_first_parent);
    RUN_TEST(test_locs_cap);
    RUN_TEST(test_locs_many_in_one_record);
    RUN_TEST(test_locs_unwritable_file_warns_once);
    RUN_TEST(test_file_printable);
    RUN_TEST(test_file_printable_zero_is_none);
    RUN_TEST(test_src_text_forms_and_skips);
    RUN_TEST(test_src_text_empty);
    return UNITY_END();
}
