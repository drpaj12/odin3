/*
 * test_proc_rmdead.c — proc's rmdead phase and the pattern pools (2E spec §5.2, PR-8; plan
 * Task 4): a case listing every value (Review Focus 3), a casez item shadowed by an earlier one, a
 * non-constant item, no default, a repeated compare, nested switches, a signal with constant bits,
 * the pattern cap (W5a), the pools on their own, and out of memory.
 *
 * Yosys re-derivation: each test's RTLIL text, saved as <test>.il (in a test that builds several,
 * the variant's name), gives its expectation with
 *   tools/proc-check/yosys-phase --yosys $VTR_ROOT/build/bin/yosys --upto rmdead <test>.il
 * (Yosys 0.55, git sha1 60f126cd0; run 2026-10-11), which runs proc_clean first; none of these
 * texts has anything for proc_clean to change. The expected texts are Yosys's process bodies as
 * pb_print indents them. The pattern cap is 2E's (§20 #7): Yosys's result is the one without it.
 */
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "passes/proc/ppool.h"
#include "passes/proc/proc.h"
#include "passes/proc/proc_internal.h"
#include "passes/proc/proc_test.h"
#include "passes/proc/pw.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    LOG_LINES = 16,
    LOG_TEXT = 512,
    OOM_LIMIT = 4000,
    POOL_TEXT = 256,
    PAT_MAX = 72,
    POOL_PATS = 16,
    CAP_BITS = 8,
    NESTED_CAP_BITS = 24,
    WIDE = 40,
    NET_BIT = 5,
    NO_CAP = 0,
    PAIR_MASK = 3,
    PAIRS_PER_WORD = 32,
};

static odin3_log_level levels[LOG_LINES];
static char lines[LOG_LINES][LOG_TEXT];
static size_t n_lines;

static void line_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (n_lines < LOG_LINES) {
        size_t len = strlen(msg);
        len = len < LOG_TEXT ? len : LOG_TEXT - 1;
        levels[n_lines] = level;
        memcpy(lines[n_lines], msg, len);
        lines[n_lines][len] = '\0';
    }
    n_lines++;
}

void setUp(void) {
    n_lines = 0;
    memset(lines, 0, sizeof lines);
    odin3_log_set_sink(line_sink, NULL);
    odin3_proc_test_set_hooks(NULL);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_proc_test_set_limits(NULL);
    odin3_fdpool_test_set_hash(NULL);
    odin3_log_set_sink(NULL, NULL);
}

/* Warning line nth, as the default sink prints it ("odin3: warning: <msg>"), must read want. */
static size_t expect_warning(size_t nth, const char *want) {
    char full[LOG_TEXT + LOG_LINES];
    int len = snprintf(full, sizeof full, "odin3: warning: %s", lines[nth]);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof full);
    TEST_ASSERT_EQUAL_STRING(want, full);
    return 1;
}

/* The lines rmdead logged in the last rmdead_run (check's own lines come after them). */
static size_t rmdead_lines;

/* The number of warning lines the last rmdead_run's rmdead logged; each must read want. */
static size_t check_warnings(const char *want) {
    size_t count = 0;
    for (size_t i = 0; i < rmdead_lines && i < LOG_LINES; i++) {
        count += levels[i] == ODIN3_LOG_WARN ? expect_warning(i, want) : 0;
    }
    return count;
}

/* --- fixture: build, decode, rmdead, encode, print -------------------------------------------- */

typedef struct fixture {
    pb_result res;
    odin3_proc_mctx mctx;
    odin3_proc_stats stats;
    odin3_pw pw;
} fixture;

static void check_full(const pb_result *res) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res->design, opts));
}

static void fx_open(fixture *fx, const char *text) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &fx->res));
    check_full(&fx->res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_mctx_init(&fx->mctx, &fx->res.ctx, fx->res.module));
    memset(&fx->stats, 0, sizeof fx->stats);
    fx->mctx.stats = &fx->stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_decode(&fx->mctx, pb_proc(&fx->res, "m", 0), &fx->pw));
}

static void fx_expect(fixture *fx, const char *want) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_encode(&fx->mctx, &fx->pw));
    check_full(&fx->res);
    odin3_strbuf got;
    odin3_strbuf_init(&got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_print(fx->res.module, fx->pw.node, &got));
    TEST_ASSERT_EQUAL_STRING(want, got.data);
    odin3_strbuf_free(&got);
}

static void fx_close(fixture *fx) {
    odin3_pw_free(&fx->pw);
    odin3_proc_mctx_free(&fx->mctx);
    pb_free(&fx->res);
}

/* The W5a count of the last rmdead_run. */
static uint32_t last_w5a;

/*
 * rmdead on text's process under a pattern cap of cap bits (NO_CAP: the default, and then no
 * warning may be given), then its body must print as want; last_w5a gets the W5a count.
 */
static void rmdead_run(const char *text, uint32_t cap, const char *want) {
    odin3_proc_limits limits = odin3_proc_limits_now();
    limits.max_pattern_bits = cap != NO_CAP ? cap : ODIN3_PROC_MAX_PATTERN_BITS;
    odin3_proc_test_set_limits(&limits);
    fixture fx;
    fx_open(&fx, text);
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_rmdead(&fx.mctx, &fx.pw));
    rmdead_lines = n_lines;
    last_w5a = fx.stats.warnings[ODIN3_PROC_W5A];
    fx_expect(&fx, want);
    fx_close(&fx);
    odin3_proc_test_set_limits(NULL);
    TEST_ASSERT_EQUAL_UINT32(0, cap == NO_CAP ? last_w5a + check_warnings("") : 0);
}

#define HEAD2(more)                                                                                \
    "module \\m\n  wire width 2 \\s\n" more "  wire \\a\n  wire \\b\n  wire \\c\n  wire \\d\n"     \
    "  wire \\e\n  wire \\f\n  wire \\y\n  process $p\n"
#define TAIL "  end\nend\n"

/* --- the two pools in rmdead ------------------------------------------------------------------ */

/* rmdead_all_values.il (Review Focus 3): the fully defined pool takes all four values, so the
 * default is dead and the switch gains full_case. */
static void test_rmdead_all_values(void) {
    rmdead_run(HEAD2("") "    switch \\s\n"
                         "      case 2'00\n        assign \\y \\a\n"
                         "      case 2'01\n        assign \\y \\b\n"
                         "      case 2'10\n        assign \\y \\c\n"
                         "      case 2'11\n        assign \\y \\e\n"
                         "      case\n        assign \\y \\d\n"
                         "    end\n" TAIL,
               NO_CAP,
               "  attribute \\full_case 1\n"
               "  switch \\s\n"
               "    case 2'00\n      assign \\y \\a\n"
               "    case 2'01\n      assign \\y \\b\n"
               "    case 2'10\n      assign \\y \\c\n"
               "    case 2'11\n      assign \\y \\e\n"
               "  end\n"
               "end\n");
}

/* rmdead_casez_shadow.il: the bit-pattern pool; 2'10 is inside 2'1- and goes; the default takes
 * the rest of the pool, so the switch gains full_case. */
static void test_rmdead_casez_shadow(void) {
    rmdead_run(HEAD2("") "    switch \\s\n"
                         "      case 2'1-\n        assign \\y \\a\n"
                         "      case 2'10\n        assign \\y \\b\n"
                         "      case\n        assign \\y \\c\n"
                         "    end\n" TAIL,
               NO_CAP,
               "  attribute \\full_case 1\n"
               "  switch \\s\n"
               "    case 2'1-\n      assign \\y \\a\n"
               "    case\n      assign \\y \\c\n"
               "  end\n"
               "end\n");
}

/* rmdead_nonconst_item.il: the compare \k is skipped, not tested, so no case goes; the default
 * sets full_case. */
static void test_rmdead_nonconst_item(void) {
    const char *body = "  switch \\s\n"
                       "    case \\k\n      assign \\y \\a\n"
                       "    case 2'01\n      assign \\y \\b\n"
                       "    case\n      assign \\y \\c\n"
                       "  end\n"
                       "end\n";
    char want[LOG_TEXT];
    int len = snprintf(want, sizeof want, "  attribute \\full_case 1\n%s", body);
    TEST_ASSERT_TRUE(len > 0 && len < (int)sizeof want);
    rmdead_run(HEAD2("  wire width 2 \\k\n") "    switch \\s\n"
                                             "      case \\k\n        assign \\y \\a\n"
                                             "      case 2'01\n        assign \\y \\b\n"
                                             "      case\n        assign \\y \\c\n"
                                             "    end\n" TAIL,
               NO_CAP, want);
}

/* rmdead_no_default.il: two of four values taken, no default: unchanged, no full_case. */
static void test_rmdead_no_default(void) {
    rmdead_run(HEAD2("") "    switch \\s\n"
                         "      case 2'00\n        assign \\y \\a\n"
                         "      case 2'01\n        assign \\y \\b\n"
                         "    end\n" TAIL,
               NO_CAP,
               "  switch \\s\n"
               "    case 2'00\n      assign \\y \\a\n"
               "    case 2'01\n      assign \\y \\b\n"
               "  end\n"
               "end\n");
}

/* rmdead_dup_compare.il: the second 2'01 is not taken again and goes. */
static void test_rmdead_dup_compare(void) {
    rmdead_run(HEAD2("") "    switch \\s\n"
                         "      case 2'01 , 2'01\n        assign \\y \\a\n"
                         "      case\n        assign \\y \\b\n"
                         "    end\n" TAIL,
               NO_CAP,
               "  attribute \\full_case 1\n"
               "  switch \\s\n"
               "    case 2'01\n      assign \\y \\a\n"
               "    case\n      assign \\y \\b\n"
               "  end\n"
               "end\n");
}

/*
 * rmdead_nested.il: the switch on t inside case 2'01 has its own pool (its second 1'1 goes, then
 * 1'0 completes it and its default goes); the outer pool is untouched by it (the second 2'01 still
 * goes, the default stays).
 */
static void test_rmdead_nested(void) {
    rmdead_run(HEAD2("  wire \\t\n") "    switch \\s\n"
                                     "      case 2'01\n"
                                     "        switch \\t\n"
                                     "          case 1'1\n            assign \\y \\a\n"
                                     "          case 1'1\n            assign \\y \\b\n"
                                     "          case 1'0\n            assign \\y \\c\n"
                                     "          case\n            assign \\y \\e\n"
                                     "        end\n"
                                     "      case 2'01\n        assign \\y \\d\n"
                                     "      case\n        assign \\y \\f\n"
                                     "    end\n" TAIL,
               NO_CAP,
               "  attribute \\full_case 1\n"
               "  switch \\s\n"
               "    case 2'01\n"
               "      attribute \\full_case 1\n"
               "      switch \\t\n"
               "        case 1'1\n          assign \\y \\a\n"
               "        case 1'0\n          assign \\y \\c\n"
               "      end\n"
               "    case\n      assign \\y \\f\n"
               "  end\n"
               "end\n");
}

/* rmdead_const_signal.il: a signal with a constant bit takes the bit-pattern pool from the signal
 * (-1), although every compare is fully defined: 2'00 goes, which the fully defined pool keeps. */
static void test_rmdead_const_signal(void) {
    rmdead_run("module \\m\n  wire \\t\n  wire \\a\n  wire \\b\n  wire \\c\n  wire \\y\n"
               "  process $p\n"
               "    switch { \\t 1'1 }\n"
               "      case 2'00\n        assign \\y \\a\n"
               "      case 2'01\n        assign \\y \\b\n"
               "      case\n        assign \\y \\c\n"
               "    end\n" TAIL,
               NO_CAP,
               "  attribute \\full_case 1\n"
               "  switch { \\t 1'1 }\n"
               "    case 2'01\n      assign \\y \\b\n"
               "    case\n      assign \\y \\c\n"
               "  end\n"
               "end\n");
}

/*
 * clean_const_not_first.il with --upto rmdead: clean leaves a switch whose signal it cleared
 * (`switch { }`); its pool is empty from the start, its one case is still the default (no
 * signal), and the switch gains full_case, as the switch on s does.
 */
static void test_rmdead_empty_signal(void) {
    fixture fx;
    fx_open(&fx, "module \\m\n  wire \\s\n  wire \\a\n  wire \\b\n  wire \\c\n  wire \\y\n"
                 "  wire \\z\n  process $p\n"
                 "    switch \\s\n"
                 "      case 1'1\n        assign \\y \\b\n"
                 "      case\n        assign \\y \\c\n"
                 "    end\n"
                 "    switch 1'1\n"
                 "      case 1'0\n        assign \\z \\a\n"
                 "      case 1'1\n        assign \\z \\b\n"
                 "    end\n" TAIL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_clean(&fx.pw, 0, ODIN3_PW_CLEAN_WHOLE_TREE));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_rmdead(&fx.mctx, &fx.pw));
    fx_expect(&fx, "  attribute \\full_case 1\n"
                   "  switch \\s\n"
                   "    case 1'1\n      assign \\y \\b\n"
                   "    case\n      assign \\y \\c\n"
                   "  end\n"
                   "  attribute \\full_case 1\n"
                   "  switch { }\n"
                   "    case\n      assign \\z \\b\n"
                   "  end\n"
                   "end\n");
    fx_close(&fx);
}

/* --- the pattern cap (W5a) -------------------------------------------------------------------- */

#define CAP_HEAD                                                                                   \
    "module \\m\n  wire width 4 \\a4\n  wire \\a\n  wire \\b\n  wire \\c\n  wire \\d\n"            \
    "  wire \\e\n  wire \\f\n  wire \\y\n  wire \\z\n  process $p\n"

static const char *const k_cap_text = CAP_HEAD "    switch \\a4\n"
                                               "      case 4'1---\n        assign \\y \\a\n"
                                               "      case 4'-1--\n        assign \\y \\b\n"
                                               "      case 4'--1-\n        assign \\y \\c\n"
                                               "      case 4'---1\n        assign \\y \\d\n"
                                               "      case 4'0000\n        assign \\y \\e\n"
                                               "      case\n        assign \\y \\f\n"
                                               "    end\n" TAIL;

#define CAP_CASES                                                                                  \
    "  switch \\a4\n"                                                                              \
    "    case 4'1---\n      assign \\y \\a\n"                                                      \
    "    case 4'-1--\n      assign \\y \\b\n"                                                      \
    "    case 4'--1-\n      assign \\y \\c\n"                                                      \
    "    case 4'---1\n      assign \\y \\d\n"                                                      \
    "    case 4'0000\n      assign \\y \\e\n"

static const char *const k_cap_warning =
    "odin3: warning: proc: module 'm': case analysis needs more than 8 pattern bits; no more dead "
    "cases are removed from this case statement";

/*
 * rmdead_pattern_cap.il: under the cap of 8 the first take needs two 4-bit patterns at once (the
 * pool's one and the child it adds: 2 x 2 x 4 = 16 bits), so the pool stops: every case is kept, no
 * full_case, W5a once. Without the cap (Yosys) 4'0000 takes the last pattern, so the default is
 * dead and the switch gains full_case. rmdead_cap_two.il: the cap is per pool, so each switch warns
 * once.
 */
static void test_rmdead_pattern_cap(void) {
    rmdead_run(k_cap_text, CAP_BITS, CAP_CASES "    case\n      assign \\y \\f\n  end\nend\n");
    TEST_ASSERT_EQUAL_UINT32(1, last_w5a);
    TEST_ASSERT_EQUAL_size_t(1, check_warnings(k_cap_warning));
    rmdead_run(k_cap_text, NO_CAP, "  attribute \\full_case 1\n" CAP_CASES "  end\nend\n");
    rmdead_run(
        CAP_HEAD "    switch \\a4\n      case 4'1---\n        assign \\y \\a\n"
                 "      case\n        assign \\y \\b\n    end\n"
                 "    switch \\a4\n      case 4'-1--\n        assign \\z \\a\n"
                 "      case\n        assign \\z \\b\n    end\n" TAIL,
        CAP_BITS,
        "  switch \\a4\n    case 4'1---\n      assign \\y \\a\n    case\n      assign \\y \\b\n"
        "  end\n"
        "  switch \\a4\n    case 4'-1--\n      assign \\z \\a\n    case\n      assign \\z \\b\n"
        "  end\n"
        "end\n");
    TEST_ASSERT_EQUAL_UINT32(2, last_w5a);
    TEST_ASSERT_EQUAL_size_t(2, check_warnings(k_cap_warning));
}

/* rmdead_cap_located.il: W5a is located at the switch's record when it has one (`attribute \\src`
 * before the switch; Yosys prints that attribute, pb_print does not). */
static void test_rmdead_cap_located(void) {
    char text[LOG_TEXT * 2];
    const char *needle = "    switch \\a4\n";
    const char *at = strstr(k_cap_text, needle);
    TEST_ASSERT_NOT_NULL(at);
    int len = snprintf(text, sizeof text, "%.*s    attribute \\src \"t.v:7.5\"\n%s",
                       (int)(at - k_cap_text), k_cap_text, at);
    TEST_ASSERT_TRUE(len > 0 && len < (int)sizeof text);
    rmdead_run(text, CAP_BITS, CAP_CASES "    case\n      assign \\y \\f\n  end\nend\n");
    TEST_ASSERT_EQUAL_size_t(1,
                             check_warnings("odin3: warning: t.v:7:5: warning: case analysis needs "
                                            "more than 8 pattern bits; no more dead cases are "
                                            "removed from this case statement"));
}

/*
 * rmdead_cap_parallel.il: 4'11-- is inside 4'1---. Yosys (no cap) removes it and the switch stays
 * parallel_case. Under the cap both stay, and the tripped switch keeps parallel_case too (§20 #7:
 * the kept dead item can then give x where Yosys gives a).
 */
static void test_rmdead_cap_parallel(void) {
    const char *text = CAP_HEAD "    attribute \\parallel_case 1\n"
                                "    switch \\a4\n"
                                "      case 4'1---\n        assign \\y \\a\n"
                                "      case 4'11--\n        assign \\y \\b\n"
                                "      case\n        assign \\y \\c\n"
                                "    end\n" TAIL;
    rmdead_run(text, NO_CAP,
               "  attribute \\full_case 1\n"
               "  attribute \\parallel_case 1\n"
               "  switch \\a4\n"
               "    case 4'1---\n      assign \\y \\a\n"
               "    case\n      assign \\y \\c\n"
               "  end\n"
               "end\n");
    rmdead_run(text, CAP_BITS,
               "  attribute \\parallel_case 1\n"
               "  switch \\a4\n"
               "    case 4'1---\n      assign \\y \\a\n"
               "    case 4'11--\n      assign \\y \\b\n"
               "    case\n      assign \\y \\c\n"
               "  end\n"
               "end\n");
    TEST_ASSERT_EQUAL_UINT32(1, last_w5a);
}

/*
 * rmdead_cap_all_dash.il: under the cap of 8 the pool stops at 4'1---, but 4'--x- (no 0/1 bit: x
 * reads as '-') empties Yosys's pool whether it takes it or not, so 2E still removes what follows:
 * 4'0000 (the non-constant \b4 stays, as compares with nets are never tested), the case 4'11--,
 * and the default; the switch gains full_case and keeps parallel_case. This is Yosys's result
 * without the cap. rmdead_cap_all_dash2.il: the all-'-' item that would otherwise reach an append
 * with an empty select in §6 (Yosys asserts) is last after rmdead.
 */
static void test_rmdead_cap_all_dash(void) {
    const char *text = "module \\m\n  wire width 4 \\a4\n  wire width 4 \\b4\n  wire \\a\n"
                       "  wire \\b\n  wire \\c\n  wire \\d\n  wire \\e\n  wire \\y\n  process $p\n"
                       "    attribute \\parallel_case 1\n"
                       "    switch \\a4\n"
                       "      case 4'1---\n        assign \\y \\a\n"
                       "      case 4'--x-\n        assign \\y \\b\n"
                       "      case 4'0000 , \\b4\n        assign \\y \\c\n"
                       "      case 4'11--\n        assign \\y \\d\n"
                       "      case\n        assign \\y \\e\n"
                       "    end\n" TAIL;
    const char *want = "  attribute \\full_case 1\n"
                       "  attribute \\parallel_case 1\n"
                       "  switch \\a4\n"
                       "    case 4'1---\n      assign \\y \\a\n"
                       "    case 4'--x-\n      assign \\y \\b\n"
                       "    case \\b4\n      assign \\y \\c\n"
                       "  end\n"
                       "end\n";
    rmdead_run(text, NO_CAP, want);
    rmdead_run(text, CAP_BITS, want);
    TEST_ASSERT_EQUAL_UINT32(1, last_w5a);
    const char *text2 = HEAD2("") "    attribute \\parallel_case 1\n"
                                  "    switch \\s\n"
                                  "      case 2'01\n        assign \\y \\c\n"
                                  "      case 2'--\n        assign \\y \\a\n"
                                  "      case 2'1-\n        assign \\y \\b\n"
                                  "    end\n" TAIL;
    const char *want2 = "  attribute \\full_case 1\n"
                        "  attribute \\parallel_case 1\n"
                        "  switch \\s\n"
                        "    case 2'01\n      assign \\y \\c\n"
                        "    case 2'--\n      assign \\y \\a\n"
                        "  end\n"
                        "end\n";
    rmdead_run(text2, NO_CAP, want2);
    rmdead_run(text2, CAP_BITS, want2);
    TEST_ASSERT_EQUAL_UINT32(1, last_w5a);
}

/*
 * rmdead_cap_nested.il under a cap of 24: the outer pool holds 2 patterns of 4 bits (16 bits)
 * while the inner switch runs; the inner pool alone stays within 24 bits (8, then 16), but the two
 * together reach 32, so the inner pool stops: its 4'1100 and default stay, no full_case, one W5a.
 * The outer switch is unaffected and gains full_case. Without the cap: Yosys's result.
 */
static void test_rmdead_cap_nested(void) {
    const char *text = "module \\m\n  wire width 4 \\a4\n  wire width 4 \\b4\n  wire \\a\n"
                       "  wire \\b\n  wire \\c\n  wire \\y\n  wire \\z\n  process $p\n"
                       "    switch \\a4\n"
                       "      case 4'11--\n"
                       "        switch \\b4\n"
                       "          case 4'11--\n            assign \\z \\a\n"
                       "          case 4'1100\n            assign \\z \\b\n"
                       "          case\n            assign \\z \\c\n"
                       "        end\n"
                       "      case\n        assign \\y \\c\n"
                       "    end\n" TAIL;
    rmdead_run(text, NESTED_CAP_BITS,
               "  attribute \\full_case 1\n"
               "  switch \\a4\n"
               "    case 4'11--\n"
               "      switch \\b4\n"
               "        case 4'11--\n          assign \\z \\a\n"
               "        case 4'1100\n          assign \\z \\b\n"
               "        case\n          assign \\z \\c\n"
               "      end\n"
               "    case\n      assign \\y \\c\n"
               "  end\n"
               "end\n");
    TEST_ASSERT_EQUAL_UINT32(1, last_w5a);
    rmdead_run(text, NO_CAP,
               "  attribute \\full_case 1\n"
               "  switch \\a4\n"
               "    case 4'11--\n"
               "      attribute \\full_case 1\n"
               "      switch \\b4\n"
               "        case 4'11--\n          assign \\z \\a\n"
               "        case\n          assign \\z \\c\n"
               "      end\n"
               "    case\n      assign \\y \\c\n"
               "  end\n"
               "end\n");
}

/* --- through the pass ------------------------------------------------------------------------- */

/* The info line among the captured lines (the pass manager logs after it). */
static bool logged(const char *want) {
    for (size_t i = 0; i < n_lines && i < LOG_LINES; i++) {
        if (strcmp(lines[i], want) == 0) {
            return true;
        }
    }
    return false;
}

/* proc --stop-after rmdead on rmdead_stop_after.il (rmdead_casez_shadow.il with port wires, so
 * check finds every net driven): clean, then rmdead, then the re-encoding. */
static void test_rmdead_stop_after(void) {
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build("module \\m\n  wire width 2 input 1 \\s\n"
                                             "  wire input 2 \\a\n  wire input 3 \\b\n"
                                             "  wire input 4 \\c\n  wire output 5 \\y\n"
                                             "  process $p\n"
                                             "    switch \\s\n"
                                             "      case 2'1-\n        assign \\y \\a\n"
                                             "      case 2'10\n        assign \\y \\b\n"
                                             "      case\n        assign \\y \\c\n"
                                             "    end\n" TAIL,
                                             &res));
    check_full(&res);
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_pass_run(res.design, "proc", odin3_bytes_cstr("--stop-after rmdead")));
    check_full(&res);
    TEST_ASSERT_TRUE(logged("proc: stopped after rmdead; 1 processes kept"));
    for (size_t i = 0; i < n_lines && i < LOG_LINES; i++) {
        TEST_ASSERT_TRUE_MESSAGE(levels[i] != ODIN3_LOG_WARN, lines[i]);
    }
    odin3_strbuf got;
    odin3_strbuf_init(&got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_print(res.module, pb_proc(&res, "m", 0), &got));
    TEST_ASSERT_EQUAL_STRING("  attribute \\full_case 1\n"
                             "  switch \\s\n"
                             "    case 2'1-\n      assign \\y \\a\n"
                             "    case\n      assign \\y \\c\n"
                             "  end\n"
                             "end\n",
                             got.data);
    odin3_strbuf_free(&got);
    pb_free(&res);
}

/* --- the pools on their own ------------------------------------------------------------------- */

/* A working form holding only bits. */
static void bits_pw(odin3_pw *pw) {
    memset(pw, 0, sizeof *pw);
    odin3_vec_init(&pw->bits, sizeof(odin3_pw_bit));
}

/* The bit a pattern character stands for: 0 1 x z, n (a net), anything else '-'. */
static odin3_pw_bit bit_of(char ch) {
    switch (ch) {
    case '0':
        return ODIN3_PW_S0;
    case '1':
        return ODIN3_PW_S1;
    case 'x':
        return ODIN3_PW_SX;
    case 'z':
        return ODIN3_PW_SZ;
    case 'n':
        return NET_BIT;
    default:
        return ODIN3_PW_SA;
    }
}

/* A run of pw for text, MSB first. */
static odin3_pw_sig sig_of(odin3_pw *pw, const char *text) {
    odin3_pw_bit bits[PAT_MAX];
    size_t len = strlen(text);
    TEST_ASSERT_TRUE(len <= PAT_MAX);
    for (size_t i = 0; i < len; i++) {
        bits[i] = bit_of(text[len - 1 - i]);
    }
    odin3_pw_sig sig = {0, 0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_sig_new(pw, bits, (uint32_t)len, &sig));
    return sig;
}

static int pat_cmp(const void *lhs, const void *rhs) {
    return strcmp(lhs, rhs);
}

/* Pattern pi of pool, MSB first, into out (width + 1 bytes); an invalid code 10 reads '?'. */
static void pattern_text(const odin3_ppool *pool, size_t pi, char *out) {
    static const char k_pairs[] = "01?-";
    const uint64_t *pat = (const uint64_t *)pool->words.data + pi * pool->words_per_pattern;
    for (uint32_t i = 0; i < pool->width; i++) {
        uint64_t pair = (pat[i / PAIRS_PER_WORD] >> (2 * (i % PAIRS_PER_WORD))) & PAIR_MASK;
        out[pool->width - 1 - i] = k_pairs[pair];
    }
    out[pool->width] = '\0';
}

/* The pool's patterns, MSB first, sorted and joined with ','; bits_used must match them. */
static void pool_text(const odin3_ppool *pool, char *out) {
    static char pats[POOL_PATS][PAT_MAX + 1];
    size_t held = pool->words_per_pattern == 0 ? 0 : pool->words.len / pool->words_per_pattern;
    TEST_ASSERT_TRUE(held <= POOL_PATS && held * (pool->width + 1) < POOL_TEXT);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)held * 2 * pool->width, pool->bits_used);
    for (size_t pi = 0; pi < held; pi++) {
        pattern_text(pool, pi, pats[pi]);
    }
    qsort(pats, held, sizeof pats[0], pat_cmp);
    size_t at = 0;
    for (size_t pi = 0; pi < held; pi++) {
        size_t len = strlen(pats[pi]);
        out[at] = ',';
        at += pi > 0 ? 1 : 0;
        memcpy(out + at, pats[pi], len);
        at += len;
    }
    out[at] = '\0';
}

static void expect_pool(const odin3_ppool *pool, const char *want) {
    char got[POOL_TEXT];
    pool_text(pool, got);
    TEST_ASSERT_EQUAL_STRING(want, got);
}

static void expect_take(odin3_ppool *pool, odin3_pw *pw, const char *pattern, bool want) {
    bool taken = !want;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ppool_take(pool, pw, sig_of(pw, pattern), &taken));
    TEST_ASSERT_EQUAL_INT(want, taken);
}

static void expect_has_all(const odin3_ppool *pool, odin3_pw *pw, const char *pattern, bool want) {
    TEST_ASSERT_EQUAL_INT(want, odin3_ppool_has_all(pool, pw, sig_of(pw, pattern)));
}

/* The brief's sequence, BitPatternPool(2). */
static void test_ppool_ops(void) {
    odin3_pw pw;
    bits_pw(&pw);
    odin3_ppool pool;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ppool_init(&pool, 2));
    expect_pool(&pool, "--");
    expect_take(&pool, &pw, "01", true);
    expect_pool(&pool, "-0,1-");
    expect_take(&pool, &pw, "01", false);
    expect_has_all(&pool, &pw, "1-", true);
    expect_has_all(&pool, &pw, "0-", false);
    expect_take(&pool, &pw, "00", true);
    expect_pool(&pool, "1-,10");
    TEST_ASSERT_EQUAL_INT(0, odin3_ppool_empty(&pool));
    odin3_ppool_take_all(&pool);
    TEST_ASSERT_EQUAL_INT(1, odin3_ppool_empty(&pool));
    expect_take(&pool, &pw, "--", false);
    expect_has_all(&pool, &pw, "--", false);
    odin3_ppool_free(&pool);
    odin3_pw_free(&pw);
}

/* Children are deduplicated against the kept patterns and each other; x, z and nets read '-'. */
static void test_ppool_dedupe(void) {
    odin3_pw pw;
    bits_pw(&pw);
    odin3_ppool pool;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ppool_init(&pool, 3));
    expect_take(&pool, &pw, "x11", true);
    expect_pool(&pool, "--0,-0-");
    expect_take(&pool, &pw, "z00", true);
    expect_pool(&pool, "-01,-10");
    expect_take(&pool, &pw, "101", true);
    expect_pool(&pool, "-10,001");
    expect_has_all(&pool, &pw, "x10", true);
    expect_has_all(&pool, &pw, "n10", true);
    expect_has_all(&pool, &pw, "-01", false);
    expect_has_all(&pool, &pw, "10", false);
    odin3_ppool_free(&pool);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ppool_init(&pool, 2));
    expect_take(&pool, &pw, "0-", true);
    expect_take(&pool, &pw, "-0", true);
    expect_pool(&pool, "11");
    odin3_ppool_free(&pool);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ppool_init(&pool, 2));
    expect_take(&pool, &pw, "00", true);
    expect_take(&pool, &pw, "01", true); /* -1 gives 11, 1- stays */
    expect_take(&pool, &pw, "10", true); /* 1- gives 11 again: one copy */
    expect_pool(&pool, "11");
    odin3_ppool_free(&pool);
    odin3_pw_free(&pw);
}

/* BitPatternPool(signal): 0/1 bits fixed, nets, x and z '-'; width 0 is an empty pool. */
static void test_ppool_init_sig(void) {
    odin3_pw pw;
    bits_pw(&pw);
    odin3_ppool pool;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ppool_init_sig(&pool, &pw, sig_of(&pw, "n1x0z")));
    expect_pool(&pool, "-1-0-");
    odin3_ppool_free(&pool);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ppool_init_sig(&pool, NULL, (odin3_pw_sig){0, 0}));
    TEST_ASSERT_EQUAL_INT(1, odin3_ppool_empty(&pool));
    odin3_ppool_free(&pool);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ppool_init(&pool, 0));
    TEST_ASSERT_EQUAL_INT(1, odin3_ppool_empty(&pool));
    expect_take(&pool, &pw, "", false);
    odin3_ppool_free(&pool);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_ppool_init(NULL, 1));
    odin3_pw_free(&pw);
}

/* A pattern over two words: position 32 is in the second word; a pattern of another width. */
static void test_ppool_wide(void) {
    odin3_pw pw;
    bits_pw(&pw);
    odin3_ppool pool;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ppool_init(&pool, WIDE));
    TEST_ASSERT_EQUAL_UINT32(2, pool.words_per_pattern);
    char pat[WIDE + 1];
    memset(pat, '-', WIDE);
    pat[WIDE] = '\0';
    pat[WIDE - 1 - PAIRS_PER_WORD] = '1';
    expect_take(&pool, &pw, pat, true);
    expect_has_all(&pool, &pw, pat, false);
    pat[WIDE - 1 - PAIRS_PER_WORD] = '0';
    expect_has_all(&pool, &pw, pat, true);
    expect_take(&pool, &pw, pat, true);
    TEST_ASSERT_EQUAL_INT(1, odin3_ppool_empty(&pool));
    odin3_ppool_free(&pool);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ppool_init(&pool, 2));
    bool taken = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_ppool_take(&pool, &pw, sig_of(&pw, "011"), &taken));
    expect_has_all(&pool, &pw, "011", false);
    odin3_ppool_free(&pool);
    odin3_pw_free(&pw);
}

/* The cap on the pool itself: past it the pool is unchanged and over_cap, then every take is a
 * no-op that reports taken, has_all is false and the pool is never empty. */
static void test_ppool_cap(void) {
    odin3_proc_limits limits = odin3_proc_limits_now();
    limits.max_pattern_bits = CAP_BITS;
    odin3_proc_test_set_limits(&limits);
    odin3_pw pw;
    bits_pw(&pw);
    odin3_ppool pool;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ppool_init(&pool, 2));
    expect_take(&pool, &pw, "01", true); /* 1 + 2 patterns of 4 bits: 12 > 8 */
    TEST_ASSERT_EQUAL_INT(1, pool.over_cap);
    expect_pool(&pool, ""); /* the patterns are dropped */
    expect_take(&pool, &pw, "11", true);
    expect_has_all(&pool, &pw, "--", false);
    odin3_ppool_take_all(&pool);
    TEST_ASSERT_EQUAL_INT(0, odin3_ppool_empty(&pool));
    odin3_ppool_free(&pool);
    TEST_ASSERT_EQUAL_INT(0, pool.over_cap);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ppool_init(&pool, 5)); /* one pattern is 10 bits */
    TEST_ASSERT_EQUAL_INT(1, pool.over_cap);
    TEST_ASSERT_EQUAL_size_t(0, pool.words.len);
    odin3_ppool_free(&pool);
    odin3_proc_test_set_limits(NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ppool_init(&pool, 2));
    odin3_ppool_stop(&pool); /* rmdead's walk-wide cap */
    TEST_ASSERT_EQUAL_INT(1, pool.over_cap);
    expect_pool(&pool, "");
    expect_take(&pool, &pw, "01", true);
    TEST_ASSERT_EQUAL_INT(0, odin3_ppool_empty(&pool));
    odin3_ppool_free(&pool);
    odin3_ppool_stop(NULL);
    odin3_pw_free(&pw);
}

/* One injection into a take that adds children: NO_MEMORY leaves the pool unchanged. */
static bool ppool_oom_step(odin3_pw *pw, long fail) {
    odin3_ppool pool;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_ppool_init(&pool, 3));
    expect_take(&pool, pw, "1-1", true);
    odin3_pw_sig pat = sig_of(pw, "0-0");
    bool taken = false;
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_ppool_take(&pool, pw, pat, &taken);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, "status");
    expect_pool(&pool, st == ODIN3_OK ? "0-1,1-0" : "--0,0--");
    odin3_ppool_free(&pool);
    return st == ODIN3_OK;
}

static void test_ppool_oom(void) {
    odin3_pw pw;
    bits_pw(&pw);
    bool done = false;
    long fail = 0;
    for (; fail < OOM_LIMIT && !done; fail++) {
        done = ppool_oom_step(&pw, fail);
    }
    TEST_ASSERT_TRUE(done && fail > 1);
    odin3_ppool pool;
    odin3_util_set_alloc_fail_after(0);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, odin3_ppool_init(&pool, 3));
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_EQUAL_INT(1, odin3_ppool_empty(&pool));
    odin3_ppool_free(&pool);
    odin3_pw_free(&pw);
}

static void expect_fd_take(odin3_fdpool *pool, odin3_pw *pw, const char *value, bool want) {
    bool taken = !want;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fdpool_take(pool, pw, sig_of(pw, value), &taken));
    TEST_ASSERT_EQUAL_INT(want, taken);
}

/* FullyDefinedPool(2): each value once; all four values, or the default, make it empty. */
static void test_fdpool_ops(void) {
    odin3_pw pw;
    bits_pw(&pw);
    odin3_fdpool pool;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fdpool_init(&pool, 2));
    expect_fd_take(&pool, &pw, "01", true);
    expect_fd_take(&pool, &pw, "01", false);
    expect_fd_take(&pool, &pw, "00", true);
    expect_fd_take(&pool, &pw, "10", true);
    TEST_ASSERT_EQUAL_INT(0, odin3_fdpool_empty(&pool));
    expect_fd_take(&pool, &pw, "11", true);
    TEST_ASSERT_EQUAL_UINT64(4, pool.taken);
    TEST_ASSERT_EQUAL_INT(1, odin3_fdpool_empty(&pool));
    odin3_fdpool_free(&pool);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fdpool_init(&pool, 2));
    expect_fd_take(&pool, &pw, "10", true);
    odin3_fdpool_take_all(&pool);
    TEST_ASSERT_EQUAL_INT(1, odin3_fdpool_empty(&pool));
    expect_fd_take(&pool, &pw, "11", false);
    bool taken = true;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_fdpool_take(&pool, &pw, sig_of(&pw, "1x"), &taken));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG,
                          odin3_fdpool_take(&pool, &pw, sig_of(&pw, "101"), &taken));
    odin3_fdpool_free(&pool);
    odin3_pw_free(&pw);
}

static uint64_t same_hash(odin3_bytes words) {
    (void)words;
    return 1;
}

/* Every value on one chain (the hash hook); 40 bits: never full by count, only by the default. */
static void test_fdpool_chain(void) {
    odin3_fdpool_test_set_hash(same_hash);
    odin3_pw pw;
    bits_pw(&pw);
    odin3_fdpool pool;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fdpool_init(&pool, 2));
    expect_fd_take(&pool, &pw, "01", true);
    expect_fd_take(&pool, &pw, "10", true);
    expect_fd_take(&pool, &pw, "11", true);
    expect_fd_take(&pool, &pw, "01", false);
    expect_fd_take(&pool, &pw, "10", false);
    expect_fd_take(&pool, &pw, "00", true);
    TEST_ASSERT_EQUAL_INT(1, odin3_fdpool_empty(&pool));
    odin3_fdpool_free(&pool);
    odin3_fdpool_test_set_hash(NULL);
    char value[WIDE + 1];
    memset(value, '0', WIDE);
    value[WIDE] = '\0';
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fdpool_init(&pool, WIDE));
    expect_fd_take(&pool, &pw, value, true);
    value[0] = '1';
    expect_fd_take(&pool, &pw, value, true);
    expect_fd_take(&pool, &pw, value, false);
    TEST_ASSERT_EQUAL_INT(0, odin3_fdpool_empty(&pool));
    odin3_fdpool_take_all(&pool);
    TEST_ASSERT_EQUAL_INT(1, odin3_fdpool_empty(&pool));
    odin3_fdpool_free(&pool);
    odin3_pw_free(&pw);
}

/* The 4-bit value as text, MSB first. */
static void value4(uint32_t value, char *out) {
    for (int i = 0; i < 4; i++) {
        out[3 - i] = (value >> i) & 1U ? '1' : '0';
    }
    out[4] = '\0';
}

/* One injection while taking all 16 values of a 4-bit pool: the takes before the failure stand,
 * the failed one is not taken, and taking all 16 again takes exactly the rest. */
static bool fdpool_oom_step(odin3_pw *pw, long fail) {
    odin3_fdpool pool;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_fdpool_init(&pool, 4));
    char text[5];
    uint32_t done = 0;
    odin3_status st = ODIN3_OK;
    for (; st == ODIN3_OK && done < 16; done += st == ODIN3_OK ? 1 : 0) {
        value4(done, text);
        odin3_pw_sig value = sig_of(pw, text);
        bool taken = false;
        odin3_util_set_alloc_fail_after(fail);
        st = odin3_fdpool_take(&pool, pw, value, &taken);
        odin3_util_set_alloc_fail_after(-1);
    }
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, "status");
    TEST_ASSERT_EQUAL_UINT64(done, pool.taken);
    for (uint32_t value = 0; value < 16; value++) {
        value4(value, text);
        expect_fd_take(&pool, pw, text, value >= done);
    }
    TEST_ASSERT_EQUAL_INT(1, odin3_fdpool_empty(&pool));
    odin3_fdpool_free(&pool);
    return st == ODIN3_OK;
}

static void test_fdpool_oom(void) {
    odin3_pw pw;
    bits_pw(&pw);
    bool done = false;
    long fail = 0;
    for (; fail < OOM_LIMIT && !done; fail++) {
        done = fdpool_oom_step(&pw, fail);
    }
    TEST_ASSERT_TRUE(done && fail > 2);
    odin3_pw_free(&pw);
}

/* --- out of memory ---------------------------------------------------------------------------- */

/* Both pools, a nested switch, a pattern take with children, removed compares and cases. */
static const char *const k_oom_text = HEAD2("  wire \\t\n") "    switch \\s\n"
                                                            "      case 2'1-\n"
                                                            "        switch \\t\n"
                                                            "          case 1'1\n"
                                                            "            assign \\y \\a\n"
                                                            "          case 1'1\n"
                                                            "            assign \\y \\b\n"
                                                            "          case 1'0\n"
                                                            "            assign \\y \\c\n"
                                                            "          case\n"
                                                            "            assign \\y \\e\n"
                                                            "        end\n"
                                                            "      case 2'10\n"
                                                            "        assign \\y \\b\n"
                                                            "      case 2'0- , 2'01\n"
                                                            "        assign \\y \\c\n"
                                                            "      case\n"
                                                            "        assign \\y \\d\n"
                                                            "    end\n" TAIL;

/* rmdead_oom.il. */
static const char *const k_oom_want = "  attribute \\full_case 1\n"
                                      "  switch \\s\n"
                                      "    case 2'1-\n"
                                      "      attribute \\full_case 1\n"
                                      "      switch \\t\n"
                                      "        case 1'1\n          assign \\y \\a\n"
                                      "        case 1'0\n          assign \\y \\c\n"
                                      "      end\n"
                                      "    case 2'0-\n      assign \\y \\c\n"
                                      "  end\n"
                                      "end\n";

static bool oom_step(long fail) {
    fixture fx;
    fx_open(&fx, k_oom_text);
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_pw_rmdead(&fx.mctx, &fx.pw);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, "status");
    if (st == ODIN3_OK) {
        fx_expect(&fx, k_oom_want);
    } else {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_encode(&fx.mctx, &fx.pw));
        check_full(&fx.res);
    }
    fx_close(&fx);
    return st == ODIN3_OK;
}

static void test_rmdead_oom(void) {
    rmdead_run(k_oom_text, NO_CAP, k_oom_want);
    bool done = false;
    long fail = 0;
    for (; fail < OOM_LIMIT && !done; fail++) {
        done = oom_step(fail);
    }
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_TRUE(fail > 1); /* at least one injection failed */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rmdead_all_values);
    RUN_TEST(test_rmdead_casez_shadow);
    RUN_TEST(test_rmdead_nonconst_item);
    RUN_TEST(test_rmdead_no_default);
    RUN_TEST(test_rmdead_dup_compare);
    RUN_TEST(test_rmdead_nested);
    RUN_TEST(test_rmdead_const_signal);
    RUN_TEST(test_rmdead_empty_signal);
    RUN_TEST(test_rmdead_pattern_cap);
    RUN_TEST(test_rmdead_cap_located);
    RUN_TEST(test_rmdead_cap_parallel);
    RUN_TEST(test_rmdead_cap_all_dash);
    RUN_TEST(test_rmdead_cap_nested);
    RUN_TEST(test_rmdead_stop_after);
    RUN_TEST(test_ppool_ops);
    RUN_TEST(test_ppool_dedupe);
    RUN_TEST(test_ppool_init_sig);
    RUN_TEST(test_ppool_wide);
    RUN_TEST(test_ppool_cap);
    RUN_TEST(test_ppool_oom);
    RUN_TEST(test_fdpool_ops);
    RUN_TEST(test_fdpool_chain);
    RUN_TEST(test_fdpool_oom);
    RUN_TEST(test_rmdead_oom);
    return UNITY_END();
}
