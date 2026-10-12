/*
 * test_proc_clean.c — proc's clean phase (2E spec §5.1, PR-7; plan Task 4): a constant switch
 * whose match is the first case or a later one, x and '-' compares kept, the inline's
 * first-switch condition, the three empty-case rules, empty syncs and actions, max_depth 1, a
 * 200-deep chain without recursion, and out of memory.
 *
 * Yosys re-derivation: each test's RTLIL text, saved as <test>.il (in a test that builds several,
 * the variant's name), gives its expectation with
 *   tools/proc-check/yosys-phase --yosys $VTR_ROOT/build/bin/yosys --upto clean <test>.il
 * (Yosys 0.55, git sha1 60f126cd0; run 2026-10-11). The expected texts are Yosys's process bodies
 * as pb_print indents them (two spaces less, and `case` without Yosys's trailing blank).
 */
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "passes/proc/proc_internal.h"
#include "passes/proc/proc_test.h"
#include "passes/proc/pw.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    LOG_LINES = 16,
    LOG_TEXT = 512,
    OOM_LIMIT = 4000,
    CHAIN_DEPTH = 200,
    CHAIN_SWEEPS_MAX = 201,
};

static odin3_log_level levels[LOG_LINES];
static char lines[LOG_LINES][LOG_TEXT];
static size_t n_lines;

static void line_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (n_lines < LOG_LINES) {
        levels[n_lines] = level;
        (void)snprintf(lines[n_lines], LOG_TEXT, "%s", msg);
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
    odin3_log_set_sink(NULL, NULL);
}

/* --- fixture: build, decode, clean, encode, print --------------------------------------------- */

typedef struct fixture {
    pb_result res;
    odin3_proc_mctx mctx;
    odin3_pw pw;
} fixture;

static void check_full(const pb_result *res) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res->design, opts));
}

/* Builds text and decodes the first process of module m. */
static void fx_open(fixture *fx, const char *text) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &fx->res));
    check_full(&fx->res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_mctx_init(&fx->mctx, &fx->res.ctx, fx->res.module));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_decode(&fx->mctx, pb_proc(&fx->res, "m", 0), &fx->pw));
}

/* Re-encodes the working form and compares its pb_print text with want. */
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

/* clean at max_depth on text's process, then its body must print as want. */
static void clean_expect(const char *text, int32_t max_depth, const char *want) {
    fixture fx;
    fx_open(&fx, text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_clean(&fx.pw, 0, max_depth));
    fx_expect(&fx, want);
    fx_close(&fx);
}

/* --- a constant switch ------------------------------------------------------------------------ */

#define WIRES_ABCYZ "  wire \\a\n  wire \\b\n  wire \\c\n  wire \\y\n  wire \\z\n"

/* root [] sw <sig> {2'01: [y <- a] | 2'10: [y <- b] | *: [y <- c]} @always [z <- y] */
#define CONST_SWITCH_TEXT(sig)                                                                     \
    "module \\m\n" WIRES_ABCYZ "  process $p\n"                                                    \
    "    switch " sig "\n"                                                                         \
    "      case 2'01\n"                                                                            \
    "        assign \\y \\a\n"                                                                     \
    "      case 2'10\n"                                                                            \
    "        assign \\y \\b\n"                                                                     \
    "      case\n"                                                                                 \
    "        assign \\y \\c\n"                                                                     \
    "    end\n"                                                                                    \
    "    sync always\n"                                                                            \
    "      update \\z \\y\n"                                                                       \
    "  end\n"                                                                                      \
    "end\n"

/* clean_const_first.il: the match is the first case, the signal is cleared, the switch inlined
 * and the sync kept. */
static void test_clean_const_first(void) {
    clean_expect(CONST_SWITCH_TEXT("2'01"), -1,
                 "  assign \\y \\a\n"
                 "  sync always\n"
                 "    update \\z \\y\n"
                 "end\n");
}

/* clean_const_later.il: 2'01 loses its only compare and goes, the match becomes case 0, then the
 * switch is inlined. */
static void test_clean_const_later(void) {
    clean_expect(CONST_SWITCH_TEXT("2'10"), -1,
                 "  assign \\y \\b\n"
                 "  sync always\n"
                 "    update \\z \\y\n"
                 "end\n");
}

/* clean_x_compare_kept.il: the x compare is kept and skipped, the match keeps index 1 and loses its
 * compares, and the signal stays. */
static void test_clean_x_compare_kept(void) {
    clean_expect("module \\m\n  wire \\a\n  wire \\b\n  wire \\y\n  process $p\n"
                 "    switch 1'1\n"
                 "      case 1'x\n"
                 "        assign \\y \\a\n"
                 "      case 1'1\n"
                 "        assign \\y \\b\n"
                 "    end\n"
                 "  end\nend\n",
                 -1,
                 "  switch 1'1\n"
                 "    case 1'x\n"
                 "      assign \\y \\a\n"
                 "    case\n"
                 "      assign \\y \\b\n"
                 "  end\n"
                 "end\n");
}

/* A '-' compare is not fully defined either: kept and skipped, and no case matches 1'0 later, so
 * the 1'1 case goes and nothing is inlined (yosys-phase on clean_dash_compare_kept.il). */
static void test_clean_dash_compare_kept(void) {
    clean_expect("module \\m\n  wire \\a\n  wire \\b\n  wire \\y\n  process $p\n"
                 "    switch 2'10\n"
                 "      case 2'0-\n"
                 "        assign \\y \\a\n"
                 "      case 2'11\n"
                 "        assign \\y \\b\n"
                 "    end\n"
                 "  end\nend\n",
                 -1,
                 "  switch 2'10\n"
                 "    case 2'0-\n"
                 "      assign \\y \\a\n"
                 "  end\n"
                 "end\n");
}

/* clean_const_not_first.il: the match is case 0, so the signal is cleared (Yosys prints
 * `switch { }`), but the switch is not its parent's first, so it is not inlined. */
static void test_clean_const_not_first(void) {
    clean_expect("module \\m\n  wire \\s\n" WIRES_ABCYZ "  process $p\n"
                 "    switch \\s\n"
                 "      case 1'1\n        assign \\y \\b\n"
                 "      case\n        assign \\y \\c\n"
                 "    end\n"
                 "    switch 1'1\n"
                 "      case 1'0\n        assign \\z \\a\n"
                 "      case 1'1\n        assign \\z \\b\n"
                 "    end\n"
                 "  end\nend\n",
                 ODIN3_PW_CLEAN_WHOLE_TREE,
                 "  switch \\s\n"
                 "    case 1'1\n      assign \\y \\b\n"
                 "    case\n      assign \\y \\c\n"
                 "  end\n"
                 "  switch { }\n"
                 "    case\n      assign \\z \\b\n"
                 "  end\n"
                 "end\n");
    TEST_ASSERT_EQUAL_UINT32(1, odin3_proc_test_clean_sweeps()); /* a fold sets no did_something */
}

/* --- the inline ------------------------------------------------------------------------------- */

#define SW_S                                                                                       \
    "    switch \\s\n      case 1'1\n        assign \\y \\a\n      case\n        assign \\y "      \
    "\\b\n    end\n"
#define SW_T "    switch \\t\n      case\n        assign \\z \\c\n    end\n"
#define INLINE_HEAD "module \\m\n  wire \\s\n  wire \\t\n" WIRES_ABCYZ "  process $p\n"

/* clean_inline_second.il / clean_inline_first.il: a single-case switch is inlined only when it is
 * its parent's first switch. */
static void test_clean_inline_first_only(void) {
    const char *printed_s = "  switch \\s\n"
                            "    case 1'1\n"
                            "      assign \\y \\a\n"
                            "    case\n"
                            "      assign \\y \\b\n"
                            "  end\n";
    char want[LOG_TEXT];
    int len = snprintf(want, sizeof want,
                       "%s  switch \\t\n    case\n      assign \\z \\c\n  end\nend\n", printed_s);
    TEST_ASSERT_TRUE(len > 0 && len < (int)sizeof want);
    clean_expect(INLINE_HEAD SW_S SW_T "  end\nend\n", -1, want);
    len = snprintf(want, sizeof want, "  assign \\z \\c\n%send\n", printed_s);
    TEST_ASSERT_TRUE(len > 0 && len < (int)sizeof want);
    clean_expect(INLINE_HEAD SW_T SW_S "  end\nend\n", -1, want);
}

/* --- empty cases ------------------------------------------------------------------------------ */

/* sw<attr> s {1'0: [] | 1'1: [y <- a] | *: []} */
#define EMPTY_CASES_TEXT(attr)                                                                     \
    "module \\m\n  wire \\s\n  wire \\a\n  wire \\y\n  process $p\n" attr "    switch \\s\n"       \
    "      case 1'0\n"                                                                             \
    "      case 1'1\n"                                                                             \
    "        assign \\y \\a\n"                                                                     \
    "      case\n"                                                                                 \
    "    end\n"                                                                                    \
    "  end\nend\n"

#define FULL_ATTR "    attribute \\full_case 1\n"
#define PAR_ATTR "    attribute \\parallel_case 1\n"

#define FULL_EMPTY_SWITCH FULL_ATTR "    switch \\s\n      case 1'0\n      case\n    end\n"

/*
 * sw[full] s {1'0: [] | *: []}: all cases are empty, so all go and the switch has no cases. Yosys
 * sets no did_something for that, so alone (clean_full_empty.il) the caseless switch stays: Yosys
 * 0.55 prints `switch \s` / `end`. After an inline in the same sweep (clean_full_after_inline.il)
 * a second sweep runs and removes it: the switch disappears. (The sync gives the first process an
 * output: a $proc node has at least one O pin.)
 */
static void expect_full_all_empty(void) {
    clean_expect("module \\m\n  wire \\s\n  wire \\a\n  wire \\y\n  process $p\n" FULL_EMPTY_SWITCH
                 "    sync always\n      update \\y \\a\n  end\nend\n",
                 -1,
                 "  attribute \\full_case 1\n"
                 "  switch \\s\n"
                 "  end\n"
                 "  sync always\n"
                 "    update \\y \\a\n"
                 "end\n");
    TEST_ASSERT_EQUAL_UINT32(1, odin3_proc_test_clean_sweeps());
    clean_expect(INLINE_HEAD SW_T FULL_EMPTY_SWITCH "  end\nend\n", -1, "  assign \\z \\c\nend\n");
    TEST_ASSERT_EQUAL_UINT32(3, odin3_proc_test_clean_sweeps());
}

/* clean_empty_none.il, clean_empty_par.il, clean_empty_full.il: only trailing empty cases go;
 * under parallel_case every empty case; under full_case nothing unless all are empty, then all. */
static void test_clean_empty_cases(void) {
    clean_expect(EMPTY_CASES_TEXT(""), -1,
                 "  switch \\s\n"
                 "    case 1'0\n"
                 "    case 1'1\n"
                 "      assign \\y \\a\n"
                 "  end\n"
                 "end\n");
    clean_expect(EMPTY_CASES_TEXT(PAR_ATTR), -1,
                 "  attribute \\parallel_case 1\n"
                 "  switch \\s\n"
                 "    case 1'1\n"
                 "      assign \\y \\a\n"
                 "  end\n"
                 "end\n");
    clean_expect(EMPTY_CASES_TEXT(FULL_ATTR), -1,
                 "  attribute \\full_case 1\n"
                 "  switch \\s\n"
                 "    case 1'0\n"
                 "    case 1'1\n"
                 "      assign \\y \\a\n"
                 "    case\n"
                 "  end\n"
                 "end\n");
    expect_full_all_empty();
}

/* --- empty syncs and actions ------------------------------------------------------------------ */

/* Appends an action with an empty lhs and rhs to actions (2E never decodes one; arst and ff make
 * them). */
static void push_empty_action(odin3_vec *actions) {
    odin3_pw_action *act = odin3_vec_push(actions);
    TEST_ASSERT_NOT_NULL(act);
    act->lhs = (odin3_pw_sig){0, 0};
    act->rhs = (odin3_pw_sig){0, 0};
}

/*
 * clean_sync_empty.il is the text below with `assign { } { }` added to the root case and to case
 * 1'1, and `update { } { }` to the always sync; the test adds those actions to the working form
 * instead, since a body cannot hold them. `sync posedge \c` has no actions and goes.
 */
static void test_clean_sync_empty(void) {
    fixture fx;
    fx_open(&fx, "module \\m\n  wire \\a\n  wire \\c\n  wire \\s\n  wire \\y\n  wire \\z\n"
                 "  process $p\n"
                 "    assign \\y \\a\n"
                 "    switch \\s\n"
                 "      case 1'1\n"
                 "        assign \\y \\c\n"
                 "      case\n"
                 "    end\n"
                 "    sync posedge \\c\n"
                 "    sync always\n"
                 "      update \\z \\y\n"
                 "  end\nend\n");
    TEST_ASSERT_EQUAL_size_t(2, fx.pw.syncs.len);
    push_empty_action(&((odin3_pw_case *)odin3_vec_at(&fx.pw.cases, 0))->actions);
    push_empty_action(&((odin3_pw_case *)odin3_vec_at(&fx.pw.cases, 1))->actions);
    push_empty_action(&((odin3_pw_sync *)odin3_vec_at(&fx.pw.syncs, 1))->actions);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_clean(&fx.pw, 0, -1));
    TEST_ASSERT_EQUAL_size_t(1, fx.pw.syncs.len);
    fx_expect(&fx, "  assign \\y \\a\n"
                   "  switch \\s\n"
                   "    case 1'1\n"
                   "      assign \\y \\c\n"
                   "  end\n"
                   "  sync always\n"
                   "    update \\z \\y\n"
                   "end\n");
    fx_close(&fx);
}

/* A call below the whole tree (arst's) leaves the syncs alone, as proc_arst's loop does. */
static void test_clean_syncs_whole_process_only(void) {
    fixture fx;
    fx_open(&fx, "module \\m\n  wire \\c\n  wire \\d\n  wire \\q\n  process $p\n"
                 "    sync posedge \\c\n"
                 "      update \\q \\d\n"
                 "  end\nend\n");
    push_empty_action(&((odin3_pw_sync *)odin3_vec_at(&fx.pw.syncs, 0))->actions);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_clean(&fx.pw, 0, 1));
    TEST_ASSERT_EQUAL_size_t(2,
                             ((const odin3_pw_sync *)odin3_vec_cat(&fx.pw.syncs, 0))->actions.len);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_clean(&fx.pw, 0, -1));
    TEST_ASSERT_EQUAL_size_t(1,
                             ((const odin3_pw_sync *)odin3_vec_cat(&fx.pw.syncs, 0))->actions.len);
    fx_close(&fx);
}

/* --- depth ------------------------------------------------------------------------------------ */

/* sw 1'1 {1'x: [y <- d] | 1'1: [] sw 1'0 {1'0: [y <- a] | *: [y <- b]} | *: [y <- c]} */
static const char *const k_depth_text =
    "module \\m\n  wire \\a\n  wire \\b\n  wire \\c\n  wire \\d\n"
    "  wire \\y\n  process $p\n"
    "    switch 1'1\n"
    "      case 1'x\n"
    "        assign \\y \\d\n"
    "      case 1'1\n"
    "        switch 1'0\n"
    "          case 1'0\n"
    "            assign \\y \\a\n"
    "          case\n"
    "            assign \\y \\b\n"
    "        end\n"
    "      case\n"
    "        assign \\y \\c\n"
    "    end\n"
    "  end\nend\n";

/*
 * max_depth 1 (proc_arst's call, proc_arst.cc 148-153; no yosys-phase command of its own) folds the
 * root's switch (its match keeps index 1, the default after it goes) and leaves the inner constant
 * switch untouched; so does max_depth 2, whose calls on the root switch's cases run with max_depth
 * 0 and skip their switches; the whole tree (clean_depth_one.il, --upto clean) folds and inlines
 * it.
 */
static void test_clean_depth_one(void) {
    const char *untouched = "  switch 1'1\n"
                            "    case 1'x\n"
                            "      assign \\y \\d\n"
                            "    case\n"
                            "      switch 1'0\n"
                            "        case 1'0\n"
                            "          assign \\y \\a\n"
                            "        case\n"
                            "          assign \\y \\b\n"
                            "      end\n"
                            "  end\n"
                            "end\n";
    clean_expect(k_depth_text, 1, untouched);
    clean_expect(k_depth_text, 2, untouched);
    clean_expect(k_depth_text, -1,
                 "  switch 1'1\n"
                 "    case 1'x\n"
                 "      assign \\y \\d\n"
                 "    case\n"
                 "      assign \\y \\a\n"
                 "  end\n"
                 "end\n");
}

/* A case below the root: only that subtree is cleaned. */
static void test_clean_case_index(void) {
    fixture fx;
    fx_open(&fx, k_depth_text);
    const odin3_pw_switch *outer = odin3_vec_cat(&fx.pw.switches, 0);
    uint32_t second = *(const uint32_t *)odin3_vec_cat(&outer->cases, 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_clean(&fx.pw, second, -1));
    fx_expect(&fx, "  switch 1'1\n"
                   "    case 1'x\n"
                   "      assign \\y \\d\n"
                   "    case 1'1\n"
                   "      assign \\y \\a\n"
                   "    case\n"
                   "      assign \\y \\c\n"
                   "  end\n"
                   "end\n");
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pw_clean(&fx.pw, UINT32_MAX, -1));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pw_clean(NULL, 0, -1));
    fx_close(&fx);
}

/* --- termination ------------------------------------------------------------------------------ */

/* A CHAIN_DEPTH-deep chain of single-case switches on t around bottom. */
static void chain_text(odin3_strbuf *out, const char *bottom) {
    odin3_strbuf_init(out);
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("module \\m\n  wire \\s\n  wire \\t\n"
                                                            "  wire \\a\n  wire \\b\n  wire \\y\n"
                                                            "  process $p\n")));
    for (int i = 0; i < CHAIN_DEPTH; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_strbuf_append(out, odin3_bytes_cstr("switch \\t\ncase\n")));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(bottom)));
    for (int i = 0; i < CHAIN_DEPTH; i++) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("end\n")));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr("  end\nend\n")));
}

/*
 * clean_chain_a.il: 200 single-case switches around a two-case switch. Each sweep inlines one
 * level (the inlined switch's slot, left without cases, goes in the same sweep, behind the switch
 * moved in front of it), so 200 sweeps and a last one that changes nothing: 201, iteratively.
 * clean_chain_b.il: the same chain around a lone action. The last inline moves no switch in front
 * of the emptied slot, which therefore stays until a 201st sweep removes it; 202 sweeps (Yosys's
 * own loop does the same, and clean_full_after_inline.il shows that slot is observable).
 */
static void test_clean_terminates(void) {
    odin3_strbuf text;
    chain_text(&text, "switch \\s\ncase 1'1\nassign \\y \\a\ncase\nassign \\y \\b\nend\n");
    clean_expect(text.data, -1,
                 "  switch \\s\n"
                 "    case 1'1\n"
                 "      assign \\y \\a\n"
                 "    case\n"
                 "      assign \\y \\b\n"
                 "  end\n"
                 "end\n");
    TEST_ASSERT_TRUE(odin3_proc_test_clean_sweeps() <= CHAIN_SWEEPS_MAX);
    TEST_ASSERT_EQUAL_UINT32(CHAIN_SWEEPS_MAX, odin3_proc_test_clean_sweeps());
    odin3_strbuf_free(&text);
    chain_text(&text, "assign \\y \\a\n");
    clean_expect(text.data, -1, "  assign \\y \\a\nend\n");
    TEST_ASSERT_EQUAL_UINT32(CHAIN_SWEEPS_MAX + 1, odin3_proc_test_clean_sweeps());
    odin3_strbuf_free(&text);
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

/* proc --stop-after clean on clean_stop_after.il (clean_const_later.il with port wires, so check
 * finds every net driven) re-encodes the cleaned process. */
static void test_clean_stop_after(void) {
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build("module \\m\n  wire input 1 \\a\n  wire input 2 \\b\n"
                                             "  wire input 3 \\c\n  wire output 4 \\y\n"
                                             "  wire output 5 \\z\n  process $p\n"
                                             "    switch 2'10\n"
                                             "      case 2'01\n        assign \\y \\a\n"
                                             "      case 2'10\n        assign \\y \\b\n"
                                             "      case\n        assign \\y \\c\n"
                                             "    end\n"
                                             "    sync always\n      update \\z \\y\n"
                                             "  end\nend\n",
                                             &res));
    check_full(&res);
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_pass_run(res.design, "proc", odin3_bytes_cstr("--stop-after clean")));
    check_full(&res);
    TEST_ASSERT_TRUE(logged("proc: stopped after clean; 1 processes kept"));
    for (size_t i = 0; i < n_lines && i < LOG_LINES; i++) {
        TEST_ASSERT_TRUE_MESSAGE(levels[i] != ODIN3_LOG_WARN, lines[i]);
    }
    odin3_strbuf got;
    odin3_strbuf_init(&got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_print(res.module, pb_proc(&res, "m", 0), &got));
    TEST_ASSERT_EQUAL_STRING("  assign \\y \\b\n  sync always\n    update \\z \\y\nend\n",
                             got.data);
    odin3_strbuf_free(&got);
    pb_free(&res);
}

/* --- out of memory ---------------------------------------------------------------------------- */

/* Inlines (both reservations), constant folds, nesting and empty cases. */
static const char *const k_oom_text =
    INLINE_HEAD SW_T "    switch \\s\n"
                     "      case 1'1\n"
                     "        switch \\t\n"
                     "          case\n"
                     "            assign \\z \\b\n" SW_T "        end\n"
                     "      case\n"
                     "    end\n"
                     "  end\nend\n";

static const char *const k_oom_want = "  assign \\z \\c\n"
                                      "  switch \\s\n"
                                      "    case 1'1\n"
                                      "      assign \\z \\b\n"
                                      "      assign \\z \\c\n"
                                      "  end\n"
                                      "end\n";

/* One injection: NO_MEMORY or OK, the working form still a tree (encode works, no leak). */
static bool oom_step(long fail) {
    fixture fx;
    fx_open(&fx, k_oom_text);
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_pw_clean(&fx.pw, 0, -1);
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

static void test_clean_oom(void) {
    clean_expect(k_oom_text, -1, k_oom_want);
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
    RUN_TEST(test_clean_const_first);
    RUN_TEST(test_clean_const_later);
    RUN_TEST(test_clean_x_compare_kept);
    RUN_TEST(test_clean_dash_compare_kept);
    RUN_TEST(test_clean_const_not_first);
    RUN_TEST(test_clean_inline_first_only);
    RUN_TEST(test_clean_empty_cases);
    RUN_TEST(test_clean_sync_empty);
    RUN_TEST(test_clean_syncs_whole_process_only);
    RUN_TEST(test_clean_depth_one);
    RUN_TEST(test_clean_case_index);
    RUN_TEST(test_clean_terminates);
    RUN_TEST(test_clean_stop_after);
    RUN_TEST(test_clean_oom);
    return UNITY_END();
}
