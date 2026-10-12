/*
 * test_proc_prune.c — proc's prune phase (2E spec §5.3, PR-9; plan Task 5): a root assignment
 * promoted to a connection, no promotion when a later switch assigns the bit, overwritten actions
 * removed, the intersection over the cases with a default and with full_case, part of an action
 * promoted and part kept, a nested switch that does not assign on every path and one that does,
 * cases counted rather than assignments and the counts reset between switches (internal risk
 * §21 #2), switches with one case, switches last to first, the promotion order, --stop-after
 * prune, and out of memory.
 *
 * Yosys re-derivation: each test's RTLIL text, saved as <test>.il (in a test that builds several,
 * the variant's name), gives its expectation with
 *   tools/proc-check/yosys-phase --yosys $VTR_ROOT/build/bin/yosys --upto prune <test>.il
 * (Yosys 0.55, git sha1 60f126cd0; run 2026-10-12), which runs proc_clean and proc_rmdead first,
 * as the fixture does (rmdead gives a switch with a default its full_case);
 * prune_default_without_full runs prune alone (`--passes proc_prune` in place of `--upto prune`).
 * The expected texts are Yosys's process bodies as pb_print indents them, and the expected
 * promotions are Yosys's module `connect` lines, in Yosys's order.
 */
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "passes/proc/proc.h"
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

enum { LOG_LINES = 16, LOG_TEXT = 512, OOM_LIMIT = 4000 };

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
    odin3_log_set_sink(NULL, NULL);
}

static bool logged(const char *want) {
    for (size_t i = 0; i < n_lines && i < LOG_LINES; i++) {
        if (strcmp(lines[i], want) == 0) {
            return true;
        }
    }
    return false;
}

/* --- fixture: build, decode, clean, rmdead, prune, encode, print ------------------------------ */

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

/* Builds text and decodes its process. */
static void fx_open_raw(fixture *fx, const char *text) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &fx->res));
    check_full(&fx->res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_mctx_init(&fx->mctx, &fx->res.ctx, fx->res.module));
    memset(&fx->stats, 0, sizeof fx->stats);
    fx->mctx.stats = &fx->stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_decode(&fx->mctx, pb_proc(&fx->res, "m", 0), &fx->pw));
}

/* fx_open_raw, then clean and rmdead, as yosys-phase --upto prune does before proc_prune. */
static void fx_open(fixture *fx, const char *text) {
    fx_open_raw(fx, text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_clean(&fx->pw, 0, ODIN3_PW_CLEAN_WHOLE_TREE));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_rmdead(&fx->mctx, &fx->pw));
}

/* Appends bit as the promotions print it: a constant as RTLIL spells it, a net as its primary
 * wire's name, with [k] on a wire wider than one bit. */
static void append_const(odin3_pw_bit bit, odin3_strbuf *out) {
    static const char *const k_const[] = {"1'0", "1'1", "1'x", "1'z", "1'-", "1'm"};
    TEST_ASSERT_TRUE((bit & ~ODIN3_PW_CONST) < sizeof k_const / sizeof k_const[0]);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_strbuf_appendf(out, "%s", k_const[bit & ~ODIN3_PW_CONST]));
}

static void append_bit(const odin3_module *module, odin3_pw_bit bit, odin3_strbuf *out) {
    if ((bit & ODIN3_PW_CONST) != 0) {
        append_const(bit, out);
        return;
    }
    odin3_wirebit wb = odin3_net_primary(module, (odin3_net_id){bit});
    TEST_ASSERT_TRUE(odin3_wire_valid(wb.wire));
    const char *name = odin3_strtab_get(odin3_design_strtab(odin3_module_design(module)),
                                        odin3_wire_name(module, wb.wire));
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out, "%s", name));
    if (odin3_wire_width(module, wb.wire) > 1) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out, "[%u]", (unsigned)wb.bit));
    }
}

/* Appends sig MSB first, as RTLIL writes it: one bit alone, several as {b_n ... b_0}. */
static void append_sig(const fixture *fx, odin3_pw_sig sig, odin3_strbuf *out) {
    const odin3_pw_bit *bits = odin3_pw_sig_bits(&fx->pw, sig);
    TEST_ASSERT_TRUE(sig.len > 0 && bits != NULL);
    bool many = sig.len > 1;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out, "%s", many ? "{" : ""));
    for (uint32_t i = sig.len; i > 0; i--) {
        append_bit(fx->res.module, bits[i - 1], out);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out, "%s", i > 1 ? " " : ""));
    }
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out, "%s", many ? "}" : ""));
}

/* pw->promoted as "lhs <- rhs" in promotion order, joined by "; " (empty: none). */
static void promoted_text(const fixture *fx, odin3_strbuf *out) {
    for (size_t i = 0; i < fx->pw.promoted.len; i++) {
        const odin3_pw_action *act = odin3_vec_cat(&fx->pw.promoted, i);
        TEST_ASSERT_EQUAL_UINT32(act->lhs.len, act->rhs.len);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out, "%s", i > 0 ? "; " : ""));
        append_sig(fx, act->lhs, out);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out, " <- "));
        append_sig(fx, act->rhs, out);
    }
}

/* The promotions must read promoted, then the re-encoded body must print as body. */
static void fx_expect(fixture *fx, const char *body, const char *promoted) {
    odin3_strbuf got;
    odin3_strbuf_init(&got);
    promoted_text(fx, &got);
    TEST_ASSERT_EQUAL_STRING(promoted, got.data != NULL ? got.data : ""); /* NULL: none */
    odin3_strbuf_clear(&got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_encode(&fx->mctx, &fx->pw));
    check_full(&fx->res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_print(fx->res.module, fx->pw.node, &got));
    TEST_ASSERT_EQUAL_STRING(body, got.data);
    odin3_strbuf_free(&got);
}

static void fx_close(fixture *fx) {
    odin3_pw_free(&fx->pw);
    odin3_proc_mctx_free(&fx->mctx);
    pb_free(&fx->res);
}

/* A result: the re-encoded body as pb_print prints it, and the promotions as promoted_text. */
typedef struct want {
    const char *body;
    const char *promoted;
} want;

/* clean, rmdead and prune on text's process, then its promotions and body must be as expected;
 * the scratch index is clear afterwards, and prune logs nothing. */
static void prune_run(const char *text, want expect) {
    fixture fx;
    fx_open(&fx, text);
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_prune(&fx.mctx, &fx.pw));
    TEST_ASSERT_EQUAL_size_t(0, n_lines);
    TEST_ASSERT_EQUAL_size_t(0, fx.mctx.touched.len);
    fx_expect(&fx, expect.body, expect.promoted);
    fx_close(&fx);
}

#define HEAD                                                                                       \
    "module \\m\n  wire \\clk\n  wire \\a\n  wire \\b\n  wire \\c\n  wire \\d\n  wire \\e\n"       \
    "  wire \\f\n  wire \\q\n  wire \\t0\n  wire \\t1\n  wire \\t2\n  wire \\t3\n  wire \\t4\n"    \
    "  process $p\n"
#define TAIL "  end\nend\n"

/* --- promotion -------------------------------------------------------------------------------- */

/* prune_promote_root.il: of `t0 <- q; t0 <- d` the first is overwritten (removed) and the second,
 * which nothing later assigns, becomes the connection t0 <- d. */
static void test_prune_promote_root(void) {
    prune_run(HEAD "    assign \\t0 \\q\n"
                   "    assign \\t0 \\d\n"
                   "    sync posedge \\clk\n      update \\q \\t0\n" TAIL,
              (want){"  sync posedge \\clk\n    update \\q \\t0\nend\n", "t0 <- d"});
}

/* prune_no_promote_later_switch.il: the switch assigns t0 (so it is in affected) but has no
 * default and no full_case (so not on every path): the root action stays, not promoted. */
static void test_prune_no_promote_later_switch(void) {
    prune_run(HEAD "    assign \\t0 \\q\n"
                   "    switch \\e\n      case 1'1\n        assign \\t0 \\d\n    end\n"
                   "    sync posedge \\clk\n      update \\q \\t0\n" TAIL,
              (want){"  assign \\t0 \\q\n"
                     "  switch \\e\n    case 1'1\n      assign \\t0 \\d\n  end\n"
                     "  sync posedge \\clk\n    update \\q \\t0\nend\n",
                     ""});
}

/* --- coverage on every path ------------------------------------------------------------------- */

/* prune_switch_default_covers.il: both cases assign t0 and one is the default, so the root action
 * is overwritten on every path: removed, not promoted. The switch stays (rmdead made it full). */
static void test_prune_switch_default_covers(void) {
    prune_run(HEAD "    assign \\t0 \\c\n"
                   "    switch \\e\n"
                   "      case 1'1\n        assign \\t0 \\a\n"
                   "      case\n        assign \\t0 \\b\n"
                   "    end\n" TAIL,
              (want){"  attribute \\full_case 1\n"
                     "  switch \\e\n"
                     "    case 1'1\n      assign \\t0 \\a\n"
                     "    case\n      assign \\t0 \\b\n"
                     "  end\n"
                     "end\n",
                     ""});
}

/*
 * prune_default_without_full.il (prune_switch_default_covers.il, prune alone: yosys-phase --passes
 * proc_prune): without rmdead the switch has no full_case, and its default alone makes it cover
 * t0, so the root action still goes. (After rmdead a switch with a default is always full_case,
 * unless W5a stopped its analysis.)
 */
static void test_prune_default_without_full(void) {
    fixture fx;
    fx_open_raw(&fx, HEAD "    assign \\t0 \\c\n"
                          "    switch \\e\n"
                          "      case 1'1\n        assign \\t0 \\a\n"
                          "      case\n        assign \\t0 \\b\n"
                          "    end\n" TAIL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_prune(&fx.mctx, &fx.pw));
    fx_expect(&fx,
              "  switch \\e\n"
              "    case 1'1\n      assign \\t0 \\a\n"
              "    case\n      assign \\t0 \\b\n"
              "  end\n"
              "end\n",
              "");
    fx_close(&fx);
}

/* prune_full_case_covers.il: no default, but full_case: the root action goes. */
static void test_prune_full_case_covers(void) {
    prune_run(HEAD "    assign \\t0 \\c\n"
                   "    attribute \\full_case 1\n"
                   "    switch \\e\n"
                   "      case 1'0\n        assign \\t0 \\a\n"
                   "      case 1'1\n        assign \\t0 \\b\n"
                   "    end\n" TAIL,
              (want){"  attribute \\full_case 1\n"
                     "  switch \\e\n"
                     "    case 1'0\n      assign \\t0 \\a\n"
                     "    case 1'1\n      assign \\t0 \\b\n"
                     "  end\n"
                     "end\n",
                     ""});
}

/* prune_partial_bits.il: {t0[1] t0[0]} <- {a b}; the switch covers t0[0] on every path, so that
 * bit goes, and t0[1], assigned nowhere later, is promoted; the action is left empty and goes. */
static void test_prune_partial_bits(void) {
    prune_run("module \\m\n  wire \\a\n  wire \\b\n  wire \\c\n  wire \\d\n  wire \\e\n"
              "  wire width 2 \\t0\n  process $p\n"
              "    assign { \\t0 [1] \\t0 [0] } { \\a \\b }\n"
              "    switch \\e\n"
              "      case 1'1\n        assign \\t0 [0] \\c\n"
              "      case\n        assign \\t0 [0] \\d\n"
              "    end\n" TAIL,
              (want){"  attribute \\full_case 1\n"
                     "  switch \\e\n"
                     "    case 1'1\n      assign \\t0 [0] \\c\n"
                     "    case\n      assign \\t0 [0] \\d\n"
                     "  end\n"
                     "end\n",
                     "t0[1] <- a"});
}

/* prune_narrow.il: of {t2 t1 t0} <- {a b c}, t0 is covered by f (full) and goes, t1 is assigned by
 * e (not on every path) and stays, and t2, assigned nowhere later, is promoted: the action keeps
 * `t1 <- b` alone. */
static void test_prune_narrow(void) {
    prune_run(HEAD "    assign { \\t2 \\t1 \\t0 } { \\a \\b \\c }\n"
                   "    switch \\e\n      case 1'1\n        assign \\t1 \\d\n    end\n"
                   "    switch \\f\n"
                   "      case 1'1\n        assign \\t0 \\d\n"
                   "      case\n        assign \\t0 \\q\n"
                   "    end\n" TAIL,
              (want){"  assign \\t1 \\b\n"
                     "  switch \\e\n    case 1'1\n      assign \\t1 \\d\n  end\n"
                     "  attribute \\full_case 1\n"
                     "  switch \\f\n"
                     "    case 1'1\n      assign \\t0 \\d\n"
                     "    case\n      assign \\t0 \\q\n"
                     "  end\n"
                     "end\n",
                     "t2 <- a"});
}

/* prune_nested_not_every_path.il (internal risk §21 #2): the default assigns t0 only under the
 * inner switch, which has no default and no full_case, so the intersection over e's cases must not
 * hold t0: the root action is kept. */
static void test_prune_nested_not_every_path(void) {
    prune_run(HEAD "    assign \\t0 \\c\n"
                   "    switch \\e\n"
                   "      case 1'1\n        assign \\t0 \\a\n"
                   "      case\n"
                   "        switch \\f\n          case 1'1\n            assign \\t0 \\b\n"
                   "        end\n"
                   "    end\n" TAIL,
              (want){"  assign \\t0 \\c\n"
                     "  attribute \\full_case 1\n"
                     "  switch \\e\n"
                     "    case 1'1\n      assign \\t0 \\a\n"
                     "    case\n"
                     "      switch \\f\n        case 1'1\n          assign \\t0 \\b\n      end\n"
                     "  end\n"
                     "end\n",
                     ""});
}

/* prune_two_branches_order.il: t1 is assigned on every path of e, t0 only in its first case: the
 * root keeps `t0 <- q` (affected, so not promoted) and loses `t1 <- q`. */
static void test_prune_two_branches_order(void) {
    prune_run(HEAD "    assign \\t0 \\q\n"
                   "    assign \\t1 \\q\n"
                   "    switch \\e\n"
                   "      case 1'1\n        assign \\t0 \\a\n        assign \\t1 \\b\n"
                   "      case\n        assign \\t1 \\c\n"
                   "    end\n" TAIL,
              (want){"  assign \\t0 \\q\n"
                     "  attribute \\full_case 1\n"
                     "  switch \\e\n"
                     "    case 1'1\n      assign \\t0 \\a\n      assign \\t1 \\b\n"
                     "    case\n      assign \\t1 \\c\n"
                     "  end\n"
                     "end\n",
                     ""});
}

/* prune_nested_full_propagates.il: e's first case assigns t0 only through the inner full switch
 * (every case of f), its default directly: t0 is covered on every path and the root action goes. */
static void test_prune_nested_full_propagates(void) {
    prune_run(HEAD "    assign \\t0 \\q\n"
                   "    switch \\e\n"
                   "      case 1'1\n"
                   "        switch \\f\n"
                   "          case 1'1\n            assign \\t0 \\a\n"
                   "          case\n            assign \\t0 \\b\n"
                   "        end\n"
                   "      case\n        assign \\t0 \\c\n"
                   "    end\n" TAIL,
              (want){"  attribute \\full_case 1\n"
                     "  switch \\e\n"
                     "    case 1'1\n"
                     "      attribute \\full_case 1\n"
                     "      switch \\f\n"
                     "        case 1'1\n          assign \\t0 \\a\n"
                     "        case\n          assign \\t0 \\b\n"
                     "      end\n"
                     "    case\n      assign \\t0 \\c\n"
                     "  end\n"
                     "end\n",
                     ""});
}

/*
 * prune_count_cases_not_bits.il (internal risk §21 #2): in e's first case t0 is assigned twice
 * (by the inner full switch, which overwrites `t0 <- a`, and by that action), in its second case
 * not at all. The intersection counts cases, not assignments, so t0 (1 of 2 cases) and t1 (1 of
 * 2) are not covered: the root keeps both actions, and only `t0 <- a` goes.
 */
static void test_prune_count_cases_not_bits(void) {
    prune_run(HEAD "    assign \\t0 \\q\n"
                   "    assign \\t1 \\q\n"
                   "    attribute \\full_case 1\n"
                   "    switch \\e\n"
                   "      case 1'1\n"
                   "        assign \\t0 \\a\n"
                   "        switch \\f\n"
                   "          case 1'1\n            assign \\t0 \\b\n"
                   "          case\n            assign \\t0 \\c\n"
                   "        end\n"
                   "      case 1'0\n        assign \\t1 \\d\n"
                   "    end\n" TAIL,
              (want){"  assign \\t0 \\q\n"
                     "  assign \\t1 \\q\n"
                     "  attribute \\full_case 1\n"
                     "  switch \\e\n"
                     "    case 1'1\n"
                     "      attribute \\full_case 1\n"
                     "      switch \\f\n"
                     "        case 1'1\n          assign \\t0 \\b\n"
                     "        case\n          assign \\t0 \\c\n"
                     "      end\n"
                     "    case 1'0\n      assign \\t1 \\d\n"
                     "  end\n"
                     "end\n",
                     ""});
}

/* prune_counts_reset.il: f, then e (last to first), each assigns t0 and t1 in one of its two
 * cases. Neither covers either, and f's counts must not carry over into e (1 + 1 would read as
 * "every case"): the root keeps `t0 <- q`. */
static void test_prune_counts_reset(void) {
    prune_run(HEAD "    assign \\t0 \\q\n"
                   "    switch \\e\n"
                   "      case 1'1\n        assign \\t0 \\a\n"
                   "      case\n        assign \\t1 \\b\n"
                   "    end\n"
                   "    switch \\f\n"
                   "      case 1'1\n        assign \\t0 \\c\n"
                   "      case\n        assign \\t1 \\d\n"
                   "    end\n" TAIL,
              (want){"  assign \\t0 \\q\n"
                     "  attribute \\full_case 1\n"
                     "  switch \\e\n"
                     "    case 1'1\n      assign \\t0 \\a\n"
                     "    case\n      assign \\t1 \\b\n"
                     "  end\n"
                     "  attribute \\full_case 1\n"
                     "  switch \\f\n"
                     "    case 1'1\n      assign \\t0 \\c\n"
                     "    case\n      assign \\t1 \\d\n"
                     "  end\n"
                     "end\n",
                     ""});
}

/*
 * A switch with one case: under full_case it covers what that case assigns
 * (prune_single_case_full.il: `t0 <- q` goes, `t1 <- q` stays, f being neither full nor
 * defaulted), also nested in a case of a full switch with several cases
 * (prune_single_case_nested.il: `t0 <- q` goes).
 */
static void test_prune_single_case(void) {
    prune_run(HEAD "    assign \\t0 \\q\n"
                   "    assign \\t1 \\q\n"
                   "    attribute \\full_case 1\n"
                   "    switch \\e\n      case 1'1\n        assign \\t0 \\a\n    end\n"
                   "    switch \\f\n      case 1'1\n        assign \\t1 \\b\n    end\n" TAIL,
              (want){"  assign \\t1 \\q\n"
                     "  attribute \\full_case 1\n"
                     "  switch \\e\n    case 1'1\n      assign \\t0 \\a\n  end\n"
                     "  switch \\f\n    case 1'1\n      assign \\t1 \\b\n  end\n"
                     "end\n",
                     ""});
    prune_run(HEAD "    assign \\t0 \\q\n"
                   "    switch \\e\n"
                   "      case 1'1\n"
                   "        attribute \\full_case 1\n"
                   "        switch \\f\n          case 1'1\n            assign \\t0 \\a\n"
                   "        end\n"
                   "      case\n        assign \\t0 \\b\n"
                   "    end\n" TAIL,
              (want){"  attribute \\full_case 1\n"
                     "  switch \\e\n"
                     "    case 1'1\n"
                     "      attribute \\full_case 1\n"
                     "      switch \\f\n        case 1'1\n          assign \\t0 \\a\n      end\n"
                     "    case\n      assign \\t0 \\b\n"
                     "  end\n"
                     "end\n",
                     ""});
}

/* prune_reverse_switches.il: the root's switches run last to first, so f (full) covers t0 before
 * e is seen, and e's `t0 <- a` is overwritten: its case is left empty (a later clean's business).
 */
static void test_prune_reverse_switches(void) {
    prune_run(HEAD "    switch \\e\n      case 1'1\n        assign \\t0 \\a\n    end\n"
                   "    switch \\f\n"
                   "      case 1'1\n        assign \\t0 \\b\n"
                   "      case\n        assign \\t0 \\c\n"
                   "    end\n" TAIL,
              (want){"  switch \\e\n    case 1'1\n  end\n"
                     "  attribute \\full_case 1\n"
                     "  switch \\f\n"
                     "    case 1'1\n      assign \\t0 \\b\n"
                     "    case\n      assign \\t0 \\c\n"
                     "  end\n"
                     "end\n",
                     ""});
}

/* --- order and determinism -------------------------------------------------------------------- */

/* prune_promote_order.il: five root actions, one of two bits, each promoted whole; the root's
 * actions run last to first, so that is the promotion order (Yosys's connect order). Two runs
 * give the same result. (The sync keeps an output: a $proc node needs one.) */
static void test_prune_promote_order(void) {
    for (int run = 0; run < 2; run++) {
        prune_run(HEAD "    assign \\t0 \\a\n"
                       "    assign { \\t2 \\t1 } { \\c \\b }\n"
                       "    assign \\t3 \\d\n"
                       "    assign \\t4 \\e\n"
                       "    assign \\f \\q\n"
                       "    sync posedge \\clk\n      update \\q \\t0\n" TAIL,
                  (want){"  sync posedge \\clk\n    update \\q \\t0\nend\n",
                         "f <- q; t4 <- e; t3 <- d; {t2 t1} <- {c b}; t0 <- a"});
    }
}

/* --- the pass -----------------------------------------------------------------------------------
 */

/* The net of bit 0 of the wire named name. */
static uint32_t net_of(const pb_result *res, const char *name) {
    uint32_t key = 0;
    TEST_ASSERT_TRUE(
        odin3_strtab_find(odin3_design_strtab(res->design), odin3_bytes_cstr(name), &key));
    return odin3_wire_net(res->module, odin3_module_find_wire(res->module, key), 0).v;
}

/* No line logged since n_lines was cleared is a warning. */
static void expect_no_warning(void) {
    for (size_t i = 0; i < n_lines && i < LOG_LINES; i++) {
        TEST_ASSERT_TRUE_MESSAGE(levels[i] != ODIN3_LOG_WARN, lines[i]);
    }
}

/* proc --stop-after prune on prune_stop_after.il (prune_promote_root.il with port wires): the body
 * is re-encoded and the promotion applied as a merge, so t0 and d become one net, named by the
 * port d (Yosys keeps `update \q \t0` and adds `connect \t0 \d`). */
static void test_prune_stop_after(void) {
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          pb_build("module \\m\n  wire input 1 \\clk\n  wire input 2 \\d\n"
                                   "  wire output 3 \\q\n  wire \\t0\n"
                                   "  process $p\n"
                                   "    assign \\t0 \\q\n"
                                   "    assign \\t0 \\d\n"
                                   "    sync posedge \\clk\n      update \\q \\t0\n" TAIL,
                                   &res));
    check_full(&res);
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_pass_run(res.design, "proc", odin3_bytes_cstr("--stop-after prune")));
    check_full(&res);
    TEST_ASSERT_TRUE(logged("proc: stopped after prune; 1 processes kept"));
    expect_no_warning();
    odin3_strbuf got;
    odin3_strbuf_init(&got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_print(res.module, pb_proc(&res, "m", 0), &got));
    TEST_ASSERT_EQUAL_STRING("  sync posedge \\clk\n    update \\q \\d\nend\n", got.data);
    odin3_strbuf_free(&got);
    uint32_t t0_net = net_of(&res, "t0");
    TEST_ASSERT_TRUE(t0_net != 0);
    TEST_ASSERT_EQUAL_UINT32(net_of(&res, "d"), t0_net);
    pb_free(&res);
}

/* A NULL argument, or a working form without a root case, is an internal error. */
static void test_prune_invalid(void) {
    odin3_pw empty;
    memset(&empty, 0, sizeof empty);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pw_prune(NULL, &empty));
    TEST_ASSERT_TRUE(logged("proc: internal: prune without a process"));
    fixture fx;
    fx_open(&fx, HEAD "    assign \\t0 \\a\n" TAIL);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pw_prune(&fx.mctx, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_pw_prune(&fx.mctx, &empty));
    fx_close(&fx);
}

/* --- out of memory ---------------------------------------------------------------------------- */

/* prune_oom.il: a partial promotion, a covered action, a whole promotion, a full switch with two
 * cases whose first holds a full switch with one case, and an overwritten action. */
static const char *const k_oom_text = "module \\m\n  wire \\a\n  wire \\b\n  wire \\c\n  wire \\d\n"
                                      "  wire \\e\n  wire \\f\n  wire \\q\n  wire width 2 \\t0\n"
                                      "  wire \\t1\n  wire \\t2\n  process $p\n"
                                      "    assign { \\t0 [1] \\t0 [0] } { \\a \\b }\n"
                                      "    assign \\t1 \\q\n"
                                      "    assign \\t2 \\c\n"
                                      "    switch \\e\n"
                                      "      case 1'1\n"
                                      "        assign \\t0 [0] \\c\n"
                                      "        assign \\t1 \\a\n"
                                      "        attribute \\full_case 1\n"
                                      "        switch \\f\n"
                                      "          case 1'1\n            assign \\t1 \\b\n"
                                      "        end\n"
                                      "      case\n"
                                      "        assign \\t0 [0] \\d\n"
                                      "        assign \\t1 \\c\n"
                                      "    end\n" TAIL;

static const char *const k_oom_body =
    "  attribute \\full_case 1\n"
    "  switch \\e\n"
    "    case 1'1\n"
    "      assign \\t0 [0] \\c\n"
    "      attribute \\full_case 1\n"
    "      switch \\f\n        case 1'1\n          assign \\t1 \\b\n"
    "      end\n"
    "    case\n      assign \\t0 [0] \\d\n      assign \\t1 \\c\n"
    "  end\n"
    "end\n";

static const char *const k_oom_promoted = "t2 <- c; t0[1] <- a";

/* One injection: NO_MEMORY with a valid tree and a clear scratch index, or the full result. */
static bool oom_step(long fail) {
    fixture fx;
    fx_open(&fx, k_oom_text);
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_pw_prune(&fx.mctx, &fx.pw);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, "status");
    TEST_ASSERT_EQUAL_size_t(0, fx.mctx.touched.len);
    for (size_t i = 0; i < fx.mctx.scratch.len; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, *(const uint32_t *)odin3_vec_cat(&fx.mctx.scratch, i));
    }
    if (st == ODIN3_OK) {
        fx_expect(&fx, k_oom_body, k_oom_promoted);
    } else {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_encode(&fx.mctx, &fx.pw));
        check_full(&fx.res);
    }
    fx_close(&fx);
    return st == ODIN3_OK;
}

static void test_prune_oom(void) {
    prune_run(k_oom_text, (want){k_oom_body, k_oom_promoted});
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
    RUN_TEST(test_prune_promote_root);
    RUN_TEST(test_prune_no_promote_later_switch);
    RUN_TEST(test_prune_switch_default_covers);
    RUN_TEST(test_prune_default_without_full);
    RUN_TEST(test_prune_full_case_covers);
    RUN_TEST(test_prune_partial_bits);
    RUN_TEST(test_prune_narrow);
    RUN_TEST(test_prune_nested_not_every_path);
    RUN_TEST(test_prune_two_branches_order);
    RUN_TEST(test_prune_nested_full_propagates);
    RUN_TEST(test_prune_count_cases_not_bits);
    RUN_TEST(test_prune_counts_reset);
    RUN_TEST(test_prune_single_case);
    RUN_TEST(test_prune_reverse_switches);
    RUN_TEST(test_prune_promote_order);
    RUN_TEST(test_prune_stop_after);
    RUN_TEST(test_prune_invalid);
    RUN_TEST(test_prune_oom);
    return UNITY_END();
}
