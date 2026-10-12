/*
 * test_proc_arst.c — proc's arst phase, the clock pre-check and E4 (2E spec §5.4, §5.5, §11;
 * PR-10, PR-11, PR-17; plan Task 6, point (n)): every reset form of §5.4's table (Review Focus 1),
 * a reset tested through a renaming wire and through a merge the pre-proc opt_expr makes (internal
 * risk §21 #7), two nested resets, a VHDL-style edge test, a register the reset does not touch,
 * apply_reset with a promoted register (internal risk §21 #3, E2), the round and walk caps (E3),
 * apply_const's known and unknown descents, the sync order, the clock errors (E1a, E1b, E5), E4,
 * located errors, --stop-after arst, and out of memory.
 *
 * Yosys re-derivation: each test's RTLIL text, saved as <test>.il (in a test that builds several,
 * the variant's name), gives its expectation with
 *   tools/proc-check/yosys-phase --yosys $VTR_ROOT/build/bin/yosys --upto arst <test>.il
 * (Yosys 0.55, git sha1 60f126cd0; run 2026-10-12), which runs proc_clean, proc_rmdead,
 * proc_prune and proc_init first, as the fixture does (proc_init has nothing to do). The clock
 * errors come from `--upto dff` (Yosys: `Multiple edge sensitive events found for this signal!`,
 * `Missing edge-sensitive event for this signal!`); E2 is Yosys's `Async reset \rst yields
 * non-constant value 1'm for signal \dataout.`; E3 (100 rounds), E4 (2D has no $memwr) and the
 * walk cap (Yosys recurses until its stack overflows) have no Yosys run, and memwr_e4's $mem has
 * only the parameters the test needs. The expected texts are Yosys's process bodies as pb_print
 * indents them, and the diagnostics §15's texts.
 */
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/value.h"
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

enum {
    LOG_LINES = 16,
    LOG_TEXT = 512,
    OOM_LIMIT = 6000,
    WALK_CAP = 8,
    ONE_ROUND = 1,
    TEXT_MAX = 4096,
};

static odin3_log_level levels[LOG_LINES];
static char lines[LOG_LINES][LOG_TEXT];
static size_t n_lines;

static void line_sink(odin3_log_level level, const char *msg, void *user) {
    (void)user;
    if (n_lines < LOG_LINES) {
        levels[n_lines] = level;
        int len = snprintf(lines[n_lines], LOG_TEXT, "%s", msg);
        TEST_ASSERT_TRUE(len >= 0 && len < LOG_TEXT);
    }
    n_lines++;
}

void setUp(void) {
    n_lines = 0;
    memset(lines, 0, sizeof lines);
    odin3_log_set_sink(line_sink, NULL);
    odin3_proc_test_set_hooks(NULL);
    odin3_proc_test_set_limits(NULL);
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_proc_test_set_limits(NULL);
    odin3_log_set_sink(NULL, NULL);
}

/* The error lines as the default sink prints them ("odin3: error: <msg>"), in order. */
static size_t errors_seen(char out[][LOG_TEXT], size_t max) {
    size_t count = 0;
    for (size_t i = 0; i < n_lines && i < LOG_LINES; i++) {
        if (levels[i] != ODIN3_LOG_ERROR) {
            continue;
        }
        if (count < max) {
            int len = snprintf(out[count], LOG_TEXT, "odin3: error: %s", lines[i]);
            TEST_ASSERT_TRUE(len > 0 && len < LOG_TEXT);
        }
        count++;
    }
    return count;
}

/* Exactly one error was logged, and it reads want. */
static void expect_one_error(const char *want) {
    char errs[2][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(1, errors_seen(errs, 2));
    TEST_ASSERT_EQUAL_STRING(want, errs[0]);
}

/* A failed proc run logged one error of proc's, want, then the pass manager's line. */
static void expect_pass_error(const char *want) {
    static const char k_failed[] = "odin3: error: pass proc: failed: ";
    char errs[3][LOG_TEXT];
    TEST_ASSERT_EQUAL_size_t(2, errors_seen(errs, 3));
    TEST_ASSERT_EQUAL_STRING(want, errs[0]);
    TEST_ASSERT_EQUAL_INT(0, strncmp(errs[1], k_failed, sizeof k_failed - 1));
}

/* --- fixture: build, decode, clean, rmdead, prune; arst; encode, print ------------------------ */

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

/* Decodes module m's process (built and checked by the caller), then clean, rmdead and prune, as
 * yosys-phase --upto arst does before proc_arst. */
static void fx_decode(fixture *fx) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_proc_mctx_init(&fx->mctx, &fx->res.ctx, fx->res.module));
    memset(&fx->stats, 0, sizeof fx->stats);
    fx->mctx.stats = &fx->stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_decode(&fx->mctx, pb_proc(&fx->res, "m", 0), &fx->pw));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_clean(&fx->pw, 0, ODIN3_PW_CLEAN_WHOLE_TREE));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_rmdead(&fx->mctx, &fx->pw));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_prune(&fx->mctx, &fx->pw));
}

static void fx_open(fixture *fx, const char *text) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(text, &fx->res));
    check_full(&fx->res);
    fx_decode(fx);
    n_lines = 0;
}

/* The scratch index is all zero (every phase resets it, also on failure). */
static void expect_scratch_clear(const fixture *fx) {
    TEST_ASSERT_EQUAL_size_t(0, fx->mctx.touched.len);
    for (size_t i = 0; i < fx->mctx.scratch.len; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, *(const uint32_t *)odin3_vec_cat(&fx->mctx.scratch, i));
    }
}

/* The re-encoded body must print as body. */
static void fx_expect(fixture *fx, const char *body) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_encode(&fx->mctx, &fx->pw));
    check_full(&fx->res);
    odin3_strbuf got;
    odin3_strbuf_init(&got);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_print(fx->res.module, fx->pw.node, &got));
    TEST_ASSERT_EQUAL_STRING(body, got.data);
    odin3_strbuf_free(&got);
}

static void fx_close(fixture *fx) {
    odin3_pw_free(&fx->pw);
    odin3_proc_mctx_free(&fx->mctx);
    pb_free(&fx->res);
}

/* arst, then the clock pre-check (E4, then the clocks), as the CLOCKS phase runs them. */
static odin3_status fx_arst_clocks(fixture *fx) {
    odin3_status st = odin3_pw_arst(&fx->mctx, &fx->pw);
    st = st == ODIN3_OK ? odin3_pw_check_memwr(&fx->mctx, &fx->pw) : st;
    st = st == ODIN3_OK ? odin3_pw_check_clocks(&fx->mctx, &fx->pw) : st;
    expect_scratch_clear(fx);
    return st;
}

/* A test's text and what it must give: a body, or an error line. */
typedef struct io {
    const char *text;
    const char *want;
} io;

/* arst and the clock pre-check on the text's process succeed silently, then its body prints as
 * want. */
static void arst_run(io test) {
    fixture fx;
    fx_open(&fx, test.text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_arst_clocks(&fx));
    TEST_ASSERT_EQUAL_size_t(0, n_lines);
    fx_expect(&fx, test.want);
    fx_close(&fx);
}

/* arst and the clock pre-check on the text's process fail with exactly the error want. */
static void arst_fail(io test) {
    fixture fx;
    fx_open(&fx, test.text);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, fx_arst_clocks(&fx));
    expect_one_error(test.want);
    TEST_ASSERT_EQUAL_UINT32(1, fx.stats.errors);
    fx_close(&fx);
}

#define PORTS_RST                                                                                  \
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\rst\n  wire input 3 \\d\n"                 \
    "  wire output 4 \\q\n  wire \\t0\n"
#define RST_SWITCH(sig)                                                                            \
    "    switch \\" sig "\n"                                                                       \
    "      case 1'1\n        assign \\t0 1'0\n"                                                    \
    "      case\n        assign \\t0 \\d\n"                                                        \
    "    end\n"
#define SYNC(kind, sig, lhs, rhs) "    sync " kind " " sig "\n      update " lhs " " rhs "\n"
#define TAIL "  end\nend\n"
#define UNARY_PARAMS                                                                               \
    "    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 1\n    parameter \\Y_WIDTH 1\n"
#define BINARY_PARAMS                                                                              \
    "    parameter \\A_SIGNED 0\n    parameter \\B_SIGNED 0\n    parameter \\A_WIDTH 1\n"          \
    "    parameter \\B_WIDTH 1\n    parameter \\Y_WIDTH 1\n"

/* arst_posedge.il: `@(posedge clk or posedge rst) if (rst) q <= 0; else q <= d;`. */
static const char *const k_posedge_text = PORTS_RST "  process $p\n" RST_SWITCH("rst")
    SYNC("posedge", "\\clk", "\\q", "\\t0") SYNC("posedge", "\\rst", "\\q", "\\t0") TAIL;

static const char *const k_posedge_body = "  assign \\t0 \\d\n"
                                          "  sync high \\rst\n    update \\q 1'0\n"
                                          "  sync posedge \\clk\n    update \\q \\t0\n"
                                          "end\n";

/* --- the reset forms (Review Focus 1) -------------------------------------------------------- */

/* arst_posedge.il: the reset sync becomes `sync high` with the reset value, the reset branch goes,
 * the other case is inlined into the root, and the level sync comes first. */
static void test_arst_posedge(void) {
    arst_run((io){k_posedge_text, k_posedge_body});
}

/* A form: switch e driven by `cell` (on rst_n for the active-low forms), the clock's edge and the
 * reset's edge. */
typedef struct form {
    const char *cell;
    const char *rst;      /* the reset's wire */
    const char *clk_edge; /* posedge | negedge */
    const char *rst_edge;
    const char *level; /* the reset sync after arst: high | low */
} form;

/* The form's text and the body arst gives (the same root as arst_posedge). */
static void form_texts(const form *spec, char *text, char *body) {
    int len =
        snprintf(text, TEXT_MAX,
                 "module \\m\n  wire input 1 \\clk\n  wire input 2 \\%s\n"
                 "  wire input 3 \\d\n  wire output 4 \\q\n  wire \\t0\n  wire \\e\n%s"
                 "  process $p\n" RST_SWITCH("e") "    sync %s \\clk\n      update \\q \\t0\n"
                                                  "    sync %s \\%s\n      update \\q \\t0\n" TAIL,
                 spec->rst, spec->cell, spec->clk_edge, spec->rst_edge, spec->rst);
    TEST_ASSERT_TRUE(len > 0 && len < TEXT_MAX);
    len = snprintf(body, TEXT_MAX,
                   "  assign \\t0 \\d\n  sync %s \\%s\n    update \\q 1'0\n"
                   "  sync %s \\clk\n    update \\q \\t0\nend\n",
                   spec->level, spec->rst, spec->clk_edge);
    TEST_ASSERT_TRUE(len > 0 && len < TEXT_MAX);
}

/*
 * forms_logic_not.il, forms_not.il, forms_eq_b.il, forms_eq_a.il, forms_ne.il,
 * forms_reduce_bool.il: `if (!rst_n)`, `if (~rst_n)`, `if (rst == 1'b1)`, `if (1'b0 == rst_n)`,
 * `if (rst_n != 1'b1)`, `if (|rst)`; each driver passes check_signal with its polarity, so the
 * reset sync is `low` for the active-low forms and `high` for the others.
 */
static void test_arst_forms(void) {
    static const form k_forms[] = {
        {"  cell $logic_not $c1\n" UNARY_PARAMS
         "    connect \\A \\rst_n\n    connect \\Y \\e\n  end\n",
         "rst_n", "negedge", "negedge", "low"},
        {"  cell $not $c1\n" UNARY_PARAMS "    connect \\A \\rst_n\n    connect \\Y \\e\n  end\n",
         "rst_n", "negedge", "negedge", "low"},
        {"  cell $eq $c1\n" BINARY_PARAMS
         "    connect \\A \\rst\n    connect \\B 1'1\n    connect \\Y \\e\n"
         "  end\n",
         "rst", "posedge", "posedge", "high"},
        {"  cell $eq $c1\n" BINARY_PARAMS
         "    connect \\A 1'0\n    connect \\B \\rst_n\n    connect \\Y \\e\n"
         "  end\n",
         "rst_n", "posedge", "negedge", "low"},
        {"  cell $ne $c1\n" BINARY_PARAMS
         "    connect \\A \\rst_n\n    connect \\B 1'1\n    connect \\Y \\e\n"
         "  end\n",
         "rst_n", "posedge", "negedge", "low"},
        {"  cell $reduce_bool $c1\n" UNARY_PARAMS
         "    connect \\A \\rst\n    connect \\Y \\e\n  end\n",
         "rst", "posedge", "posedge", "high"},
    };
    static char text[TEXT_MAX];
    static char body[TEXT_MAX];
    for (size_t i = 0; i < sizeof k_forms / sizeof k_forms[0]; i++) {
        form_texts(&k_forms[i], text, body);
        arst_run((io){text, body});
    }
}

/* The nets of test_check_signal's text, by wire. */
enum {
    SIG_R,
    SIG_E1,
    SIG_E2,
    SIG_E3,
    SIG_E4,
    SIG_E5,
    SIG_E6,
    SIG_E7,
    SIG_CLK,
    SIG_ONE,
    SIG_ZERO,
    N_SIG_NETS
};

/* One odin3_proc_check_signal call from polarity pol_in: its result, the polarity after it (left
 * alone by a failed walk) and the hops walked. */
typedef struct sig_case {
    uint32_t signal, ref;
    bool pol_in, found, pol_out;
    uint32_t hops;
} sig_case;

static void expect_check(fixture *fx, const odin3_net_id *nets, sig_case test) {
    bool pol = test.pol_in;
    odin3_proc_sigref sr = {nets[test.signal], nets[test.ref]};
    TEST_ASSERT_EQUAL(test.found, odin3_proc_check_signal(&fx->mctx, sr, &pol));
    TEST_ASSERT_EQUAL(test.pol_out, pol);
    TEST_ASSERT_EQUAL_UINT32(test.hops, odin3_proc_test_signal_hops());
}

/*
 * check_signal on its own (odin3_proc_check_signal): the polarity flips once per inversion and is
 * left alone when the walk fails; a $eq with two constant sides goes on to B (a constant, which
 * matches only the same constant), as Yosys tests A first; a wide A, a driver outside the table,
 * a $eq without a constant side and a bit of a two-bit Y all fail.
 */
static void test_check_signal(void) {
    fixture fx;
    fx_open(&fx,
            "module \\m\n  wire \\clk\n  wire \\r\n  wire width 2 \\w\n  wire \\e1\n"
            "  wire \\e2\n  wire \\e3\n  wire \\e4\n  wire \\e5\n  wire \\q\n"
            "  cell $logic_not $c1\n" UNARY_PARAMS
            "    connect \\A \\r\n    connect \\Y \\e1\n  end\n"
            "  cell $not $c2\n" UNARY_PARAMS "    connect \\A \\e1\n    connect \\Y \\e2\n  end\n"
            "  cell $reduce_or $c3\n    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 2\n"
            "    parameter \\Y_WIDTH 1\n    connect \\A \\w\n"
            "    connect \\Y \\e3\n  end\n"
            "  cell $and $c4\n" BINARY_PARAMS "    connect \\A \\r\n    connect \\B \\clk\n"
            "    connect \\Y \\e4\n  end\n"
            "  cell $eq $c5\n" BINARY_PARAMS "    connect \\A \\r\n    connect \\B \\clk\n"
            "    connect \\Y \\e5\n  end\n"
            "  wire \\e6\n  wire width 2 \\e7\n  wire \\one\n  wire \\zero\n"
            "  connect \\one 1'1\n  connect \\zero 1'0\n"
            "  cell $eq $c6\n" BINARY_PARAMS "    connect \\A 1'0\n    connect \\B 1'1\n"
            "    connect \\Y \\e6\n  end\n"
            "  cell $logic_not $c7\n    parameter \\A_SIGNED 0\n    parameter \\A_WIDTH 1\n"
            "    parameter \\Y_WIDTH 2\n    connect \\A \\r\n    connect \\Y \\e7\n  end\n"
            "  process $p\n    sync posedge \\clk\n      update \\q \\e2\n" TAIL);
    odin3_module *mod = fx.res.module;
    const char *names[N_SIG_NETS] = {"r",  "e1", "e2",  "e3",  "e4",  "e5",
                                     "e6", "e7", "clk", "one", "zero"};
    odin3_net_id nets[N_SIG_NETS];
    for (size_t i = 0; i < N_SIG_NETS; i++) {
        uint32_t str = 0;
        TEST_ASSERT_TRUE(odin3_strtab_find(odin3_design_strtab(fx.res.design),
                                           odin3_bytes_cstr(names[i]), &str));
        nets[i] = odin3_wire_net(mod, odin3_module_find_wire(mod, str), 0);
    }
    static const sig_case k_cases[] = {
        {SIG_E2, SIG_R, true, true, true, 2},      /* two inversions */
        {SIG_E1, SIG_R, true, true, false, 1},     /* one */
        {SIG_R, SIG_R, true, true, true, 0},       /* the ref itself */
        {SIG_E3, SIG_R, true, false, true, 0},     /* $reduce_or of a two-bit A */
        {SIG_E4, SIG_R, true, false, true, 0},     /* $and */
        {SIG_E5, SIG_R, true, false, true, 0},     /* $eq without a constant side */
        {SIG_E2, SIG_CLK, false, false, false, 2}, /* two inversions, then no driver */
        {SIG_E1, SIG_CLK, false, false, false, 1}, /* one inversion, left alone */
        {SIG_E6, SIG_ONE, true, true, false, 1},   /* both sides constant: on to B, as Yosys */
        {SIG_E6, SIG_ZERO, true, false, true, 1},  /* (B is 1'1, not 1'0) */
        {SIG_E7, SIG_R, true, false, true, 0},     /* bit 0 of a two-bit Y */
    };
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        expect_check(&fx, nets, k_cases[i]);
    }
    bool pol = true;
    TEST_ASSERT_FALSE(
        odin3_proc_check_signal(&fx.mctx, (odin3_proc_sigref){(odin3_net_id){0}, nets[0]}, &pol));
    fx_close(&fx);
}

/*
 * arst_rename_wire.il (Review Focus 1, `wire r = rst; ... if (r)`): r and rst are one net, so the
 * switch on r tests the reset. pb_build's `connect a b` keeps a's net: `connect \\rst \\r` keeps
 * the port's net, as 2D's merges do, and the body prints rst; the brief's `connect \\r \\rst` keeps
 * r's, and the same body prints that net as r. Yosys 0.55 compares names literally and fails with
 * `Multiple edge sensitive events found for this signal!` on arst_rename_wire_literal.il (§4,
 * §20 #1); its expectation is that of the text with one name per net, as write_rtlil prints it,
 * which is arst_posedge.il (and, with r for rst, arst_rename_wire_r.il).
 */
static void test_arst_rename_wire(void) {
    const char *const body_r = "  assign \\t0 \\d\n"
                               "  sync high \\r\n    update \\q 1'0\n"
                               "  sync posedge \\clk\n    update \\q \\t0\n"
                               "end\n";
    arst_run((io){PORTS_RST "  wire \\r\n  connect \\rst \\r\n"
                            "  process $p\n" RST_SWITCH("r") SYNC("posedge", "\\clk", "\\q", "\\t0")
                                SYNC("posedge", "\\rst", "\\q", "\\t0") TAIL,
                  k_posedge_body});
    arst_run((io){PORTS_RST "  wire \\r\n  connect \\r \\rst\n"
                            "  process $p\n" RST_SWITCH("r") SYNC("posedge", "\\clk", "\\q", "\\t0")
                                SYNC("posedge", "\\rst", "\\q", "\\t0") TAIL,
                  body_r});
}

/*
 * arst_after_trio_merge (internal risk §21 #7): the pre-proc opt_expr replaces `$eq(rst, 1'1)` by a
 * connection, which in Odin III's mode P is a merge of its Y into rst (the cell deleted first), so
 * the switch's net is rst and the reset is found. Yosys after Parmys's opt_expr fails (§4); its
 * expectation is arst_posedge.il's, the text with the merge made.
 */
static void test_arst_after_trio_merge(void) {
    fixture fx;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK,
        pb_build(PORTS_RST "  wire \\e\n"
                           "  cell $eq $c1\n" BINARY_PARAMS "    connect \\A \\rst\n"
                           "    connect \\B 1'1\n    connect \\Y \\e\n  end\n"
                           "  process $p\n" RST_SWITCH("e") SYNC("posedge", "\\clk", "\\q", "\\t0")
                               SYNC("posedge", "\\rst", "\\q", "\\t0") TAIL,
                 &fx.res));
    odin3_module *mod = fx.res.module;
    uint32_t str = 0;
    TEST_ASSERT_TRUE(
        odin3_strtab_find(odin3_design_strtab(fx.res.design), odin3_bytes_cstr("$c1"), &str));
    odin3_node_id cell = odin3_module_find_node(mod, str);
    odin3_pinslice port_y = odin3_node_port(mod, cell, 2);
    odin3_net_id net_e = odin3_pin_net(mod, port_y.first);
    odin3_net_id net_rst = odin3_pin_net(mod, odin3_node_port(mod, cell, 0).first);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_delete(mod, cell));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_net_merge(mod, (odin3_net_pair){net_rst, net_e}));
    check_full(&fx.res);
    fx_decode(&fx);
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_arst_clocks(&fx));
    TEST_ASSERT_EQUAL_size_t(0, n_lines);
    fx_expect(&fx, k_posedge_body);
    fx_close(&fx);
}

/*
 * arst_two_resets.il: `if (r1) q <= 0; else if (!r2) q <= 1; else q <= d;` with three events. r1
 * is found on the first turn; its branch goes and the inlined else holds the switch on nr2, which
 * the second turn finds as the negedge reset r2. Level syncs in the order found, then the clock.
 */
static void test_arst_two_resets(void) {
    arst_run((io){"module \\m\n  wire input 1 \\clk\n  wire input 2 \\r1\n  wire input 3 \\r2\n"
                  "  wire input 4 \\d\n  wire output 5 \\q\n  wire \\t0\n  wire \\nr2\n"
                  "  cell $logic_not $c1\n" UNARY_PARAMS
                  "    connect \\A \\r2\n    connect \\Y \\nr2\n  end\n"
                  "  process $p\n"
                  "    switch \\r1\n"
                  "      case 1'1\n        assign \\t0 1'0\n"
                  "      case\n"
                  "        switch \\nr2\n"
                  "          case 1'1\n            assign \\t0 1'1\n"
                  "          case\n            assign \\t0 \\d\n"
                  "        end\n"
                  "    end\n" SYNC("posedge", "\\clk", "\\q", "\\t0") SYNC(
                      "posedge", "\\r1", "\\q", "\\t0") SYNC("negedge", "\\r2", "\\q", "\\t0") TAIL,
                  "  assign \\t0 \\d\n"
                  "  sync high \\r1\n    update \\q 1'0\n"
                  "  sync low \\r2\n    update \\q 1'1\n"
                  "  sync posedge \\clk\n    update \\q \\t0\n"
                  "end\n"});
}

/* arst_vhdl_style.il: `always @(posedge c) if (c) q <= d; else q <= q;`: one edge sync, so the
 * branch for c = 0 goes (eliminate_const with the inverted polarity) and the sync stays an edge. */
static void test_arst_vhdl_style(void) {
    arst_run((io){"module \\m\n  wire input 1 \\c\n  wire input 2 \\d\n  wire output 3 \\q\n"
                  "  wire \\t0\n  process $p\n"
                  "    switch \\c\n"
                  "      case 1'1\n        assign \\t0 \\d\n"
                  "      case\n        assign \\t0 \\q\n"
                  "    end\n" SYNC("posedge", "\\c", "\\q", "\\t0") TAIL,
                  "  assign \\t0 \\d\n  sync posedge \\c\n    update \\q \\t0\nend\n"});
}

/*
 * arst_self_value.il: 2D's default-mode body for u, which the reset branch does not assign: the
 * root default u0 <- u survives prune (the else branch assigns u0), so apply_reset gives u its own
 * value (u <- u, a non-constant value without SM, left to the flip-flop step), and the inlined else
 * follows the root default.
 */
static void test_arst_self_value(void) {
    arst_run((io){"module \\m\n  wire input 1 \\clk\n  wire input 2 \\rst\n  wire input 3 \\d\n"
                  "  wire input 4 \\e\n  wire output 5 \\q\n  wire output 6 \\u\n  wire \\t0\n"
                  "  wire \\u0\n  process $p\n"
                  "    assign \\u0 \\u\n"
                  "    switch \\rst\n"
                  "      case 1'1\n        assign \\t0 1'0\n"
                  "      case\n        assign \\t0 \\d\n        assign \\u0 \\e\n"
                  "    end\n"
                  "    sync posedge \\clk\n      update \\q \\t0\n      update \\u \\u0\n"
                  "    sync posedge \\rst\n      update \\q \\t0\n      update \\u \\u0\n" TAIL,
                  "  assign \\u0 \\u\n  assign \\t0 \\d\n  assign \\u0 \\e\n"
                  "  sync high \\rst\n    update \\q 1'0\n    update \\u \\u\n"
                  "  sync posedge \\clk\n    update \\q \\t0\n    update \\u \\u0\n"
                  "end\n"});
}

/* arst_known_inside_unknown.il: the reset branch's switch on x is unknown (its 1'0 case writes
 * SM), but in its default a nested switch on rst passes check_signal and descends as known, so its
 * 1'1 case gives q's reset value 1'1. */
static void test_arst_known_inside_unknown(void) {
    arst_run((io){PORTS_RST "  wire input 5 \\x\n  process $p\n"
                            "    switch \\rst\n"
                            "      case 1'1\n"
                            "        switch \\x\n"
                            "          case 1'0\n            assign \\t0 1'0\n"
                            "          case\n"
                            "            switch \\rst\n"
                            "              case 1'1\n                assign \\t0 1'1\n"
                            "              case\n                assign \\t0 \\d\n"
                            "            end\n"
                            "        end\n"
                            "      case\n        assign \\t0 \\d\n"
                            "    end\n" SYNC("posedge", "\\clk", "\\q", "\\t0")
                                SYNC("posedge", "\\rst", "\\q", "\\t0") TAIL,
                  "  assign \\t0 \\d\n"
                  "  sync high \\rst\n    update \\q 1'1\n"
                  "  sync posedge \\clk\n    update \\q \\t0\n"
                  "end\n"});
}

/* arst_default_is_reset.il: `if (rst == 0) q <= d; else q <= 0;` as a switch whose default is the
 * reset branch: apply_const takes the default (no compare is 1'1) and eliminate_const keeps the
 * 1'0 case. */
static void test_arst_default_is_reset(void) {
    arst_run((io){PORTS_RST "  process $p\n"
                            "    switch \\rst\n"
                            "      case 1'0\n        assign \\t0 \\d\n"
                            "      case\n        assign \\t0 1'0\n"
                            "    end\n" SYNC("posedge", "\\clk", "\\q", "\\t0")
                                SYNC("posedge", "\\rst", "\\q", "\\t0") TAIL,
                  k_posedge_body});
}

/* arst_switch_order.il: in the reset branch an unknown switch on x writes SM, then a known switch
 * on rst writes 1'1 (without a default, so prune keeps the first write); apply_const visits them in
 * order, so the value is 1'1. */
static void test_arst_switch_order(void) {
    arst_run((io){PORTS_RST "  wire input 5 \\x\n  wire input 6 \\a\n  process $p\n"
                            "    switch \\rst\n"
                            "      case 1'1\n"
                            "        switch \\x\n          case 1'1\n            assign \\t0 \\a\n"
                            "        end\n"
                            "        switch \\rst\n"
                            "          case 1'1\n            assign \\t0 1'1\n"
                            "        end\n"
                            "      case\n        assign \\t0 \\d\n"
                            "    end\n" SYNC("posedge", "\\clk", "\\q", "\\t0")
                                SYNC("posedge", "\\rst", "\\q", "\\t0") TAIL,
                  "  assign \\t0 \\d\n"
                  "  sync high \\rst\n    update \\q 1'1\n"
                  "  sync posedge \\clk\n    update \\q \\t0\n"
                  "end\n"});
}

/* arst_later_cases.il: the reset switch keeps a case with a non-constant compare (rmdead never
 * tests it); eliminate_const keeps that first other case and removes the default after it. */
static void test_arst_later_cases(void) {
    arst_run((io){PORTS_RST "  wire input 5 \\x\n  wire input 6 \\a\n  wire input 7 \\b\n"
                            "  process $p\n"
                            "    switch \\rst\n"
                            "      case 1'1\n        assign \\t0 1'0\n"
                            "      case \\x\n        assign \\t0 \\a\n"
                            "      case\n        assign \\t0 \\b\n"
                            "    end\n" SYNC("posedge", "\\clk", "\\q", "\\t0")
                                SYNC("posedge", "\\rst", "\\q", "\\t0") TAIL,
                  "  assign \\t0 \\a\n"
                  "  sync high \\rst\n    update \\q 1'0\n"
                  "  sync posedge \\clk\n    update \\q \\t0\n"
                  "end\n"});
}

/*
 * arst_assign_view.il (internal risk §21 #3): prune promotes t1 <- w and t2 <- 1'1. o1's reset
 * value reads t1 as w, which the reset branch sets to 1'0; o2's reads t2 as the constant 1'1 at
 * once; o3's is t2 after a round, mapped to 1'1. Yosys's connect lines are the promotions.
 */
static void test_arst_assign_view(void) {
    arst_run((io){PORTS_RST
                  "  wire input 5 \\a\n  wire output 6 \\o1\n  wire output 7 \\o2\n"
                  "  wire output 8 \\o3\n  wire \\t1\n  wire \\t2\n  wire \\t3\n  wire \\w\n"
                  "  process $p\n"
                  "    assign \\t1 \\w\n    assign \\t2 1'1\n"
                  "    switch \\rst\n"
                  "      case 1'1\n        assign \\t0 1'0\n        assign \\w 1'0\n"
                  "        assign \\t3 \\t2\n"
                  "      case\n        assign \\t0 \\d\n        assign \\w \\a\n"
                  "        assign \\t3 \\d\n"
                  "    end\n"
                  "    sync posedge \\clk\n      update \\q \\t0\n      update \\o1 \\t1\n"
                  "      update \\o2 \\t2\n      update \\o3 \\t3\n"
                  "    sync posedge \\rst\n      update \\q \\t0\n      update \\o1 \\t1\n"
                  "      update \\o2 \\t2\n      update \\o3 \\t3\n" TAIL,
                  "  assign \\t0 \\d\n  assign \\w \\a\n  assign \\t3 \\d\n"
                  "  sync high \\rst\n    update \\q 1'0\n    update \\o1 1'0\n"
                  "    update \\o2 1'1\n    update \\o3 1'1\n"
                  "  sync posedge \\clk\n    update \\q \\t0\n    update \\o1 \\t1\n"
                  "    update \\o2 \\t2\n    update \\o3 \\t3\n"
                  "end\n"});
}

/* arst_reorder.il: a level sync and an always sync arrive in the body; after arst the syncs are
 * the level ones (the old first, then the reset found), the edge, then the always sync. */
static void test_arst_reorder(void) {
    fixture fx;
    fx_open(&fx,
            PORTS_RST "  wire input 5 \\en\n  wire output 6 \\r\n  process $p\n" RST_SWITCH("rst")
                SYNC("posedge", "\\clk", "\\q", "\\t0") SYNC("always", "", "\\r", "\\d")
                    SYNC("high", "\\en", "\\r", "\\d") SYNC("posedge", "\\rst", "\\q", "\\t0")
                        TAIL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_arst(&fx.mctx, &fx.pw));
    TEST_ASSERT_EQUAL_size_t(0, n_lines);
    fx_expect(&fx, "  assign \\t0 \\d\n"
                   "  sync high \\en\n    update \\r \\d\n"
                   "  sync high \\rst\n    update \\q 1'0\n"
                   "  sync posedge \\clk\n    update \\q \\t0\n"
                   "  sync always\n    update \\r \\d\n"
                   "end\n");
    fx_close(&fx);
}

/* --- E2, E3 and the caps --------------------------------------------------------------------- */

/* arst_apply_promoted_e2.il (paj_top's shape, internal risk §21 #3). */
static const char *const k_promoted_text =
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\rst\n  wire input 3 \\d\n"
    "  wire input 4 \\data\n  wire output 5 \\q\n  wire output 6 \\dataout\n  wire \\t0\n"
    "  wire \\t1\n  process $p\n"
    "    assign \\t1 \\data\n" RST_SWITCH(
        "rst") "    sync posedge \\clk\n      update \\q \\t0\n      update \\dataout \\t1\n"
               "    sync posedge \\rst\n      update \\q \\t0\n      update \\dataout \\t1\n" TAIL;

static const char *const k_promoted_e2 = "odin3: error: proc: module 'm': asynchronous reset 'rst' "
                                         "does not give 'dataout' a constant value";

/*
 * arst_apply_promoted_e2.il: prune promotes t1 <- data, so the assign view reads dataout's t1 as
 * data, which no case assigns: its value stays SM (E2), while q, read through t0, is 1'0. Through
 * the pass the module keeps its $proc node and BODY (step A changes nothing).
 */
static void test_arst_apply_promoted_e2(void) {
    fixture fx;
    fx_open(&fx, k_promoted_text);
    TEST_ASSERT_EQUAL_size_t(1, fx.pw.promoted.len);
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE, odin3_pw_arst(&fx.mctx, &fx.pw));
    expect_one_error(k_promoted_e2);
    expect_scratch_clear(&fx);
    fx_close(&fx);
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(k_promoted_text, &res));
    odin3_node_id node = pb_proc(&res, "m", 0);
    odin3_value body = *odin3_node_param(res.module, node, ODIN3_PROC_P_BODY);
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                          odin3_pass_run(res.design, "proc", odin3_bytes_cstr("--noopt")));
    expect_pass_error(k_promoted_e2);
    TEST_ASSERT_TRUE(odin3_node_live(res.module, node));
    TEST_ASSERT_EQUAL_UINT32(node.v, pb_proc(&res, "m", 0).v);
    TEST_ASSERT_TRUE(
        odin3_value_equal(&body, odin3_node_param(res.module, node, ODIN3_PROC_P_BODY)));
    check_full(&res);
    pb_free(&res);
}

/* arst_nested_unknown.il: a switch on x inside the reset branch is unknown, so its assignment
 * makes q's value SM: E2 (Yosys: `Async reset \rst yields non-constant value 1'm for signal \q.`).
 */
static void test_arst_nested_unknown(void) {
    arst_fail((io){PORTS_RST "  wire input 5 \\x\n  wire input 6 \\a\n  process $p\n"
                             "    switch \\rst\n"
                             "      case 1'1\n        assign \\t0 1'0\n"
                             "        switch \\x\n"
                             "          case 1'1\n            assign \\t0 \\a\n"
                             "        end\n"
                             "      case\n        assign \\t0 \\d\n"
                             "    end\n" SYNC("posedge", "\\clk", "\\q", "\\t0")
                                 SYNC("posedge", "\\rst", "\\q", "\\t0") TAIL,
                   "odin3: error: proc: module 'm': asynchronous reset 'rst' does not give 'q' a "
                   "constant value"});
}

/* arst_round_cap.il: the reset value of q is t0, then w, then 1'0: two rounds. */
static const char *const k_rounds_text =
    PORTS_RST "  wire \\w\n  process $p\n"
              "    switch \\rst\n"
              "      case 1'1\n        assign \\t0 \\w\n        assign \\w 1'0\n"
              "      case\n        assign \\t0 \\d\n"
              "    end\n" SYNC("posedge", "\\clk", "\\q", "\\t0")
                  SYNC("posedge", "\\rst", "\\q", "\\t0") TAIL;

/* arst_round_cap.il: under a cap of one round, E3; with the default cap, q <- 1'0 as in Yosys. */
static void test_arst_round_cap(void) {
    odin3_proc_limits limits = odin3_proc_limits_now();
    limits.max_arst_rounds = ONE_ROUND;
    odin3_proc_test_set_limits(&limits);
    arst_fail((io){k_rounds_text,
                   "odin3: error: proc: module 'm': asynchronous reset 'rst' does not "
                   "settle the value of 'q' after 1 substitutions"});
    odin3_proc_test_set_limits(NULL);
    arst_run((io){k_rounds_text, k_posedge_body});
}

/*
 * arst_walk_cap: n1 = $not(n2), n2 = $not(n1), a switch on n1. With a walk cap of 8, check_signal
 * gives up after 8 hops (Yosys recurses until its stack overflows), so no reset is found and the
 * clock pre-check reports E1a.
 */
static void test_arst_walk_cap(void) {
    odin3_proc_limits limits = odin3_proc_limits_now();
    limits.max_signal_walk = WALK_CAP;
    odin3_proc_test_set_limits(&limits);
    const char *text = PORTS_RST
        "  wire \\n1\n  wire \\n2\n"
        "  cell $not $c1\n" UNARY_PARAMS "    connect \\A \\n2\n    connect \\Y \\n1\n  end\n"
        "  cell $not $c2\n" UNARY_PARAMS "    connect \\A \\n1\n    connect \\Y \\n2\n  end\n"
        "  process $p\n" RST_SWITCH("n1") SYNC("posedge", "\\clk", "\\q", "\\t0")
            SYNC("posedge", "\\rst", "\\q", "\\t0") TAIL;
    fixture fx;
    fx_open(&fx, text);
    const odin3_pw_switch *sw = odin3_vec_cat(&fx.pw.switches, 0);
    odin3_net_id n1 = {odin3_pw_sig_bits(&fx.pw, sw->signal)[0]};
    const odin3_pw_sync *rst = odin3_vec_cat(&fx.pw.syncs, 1);
    bool pol = true;
    TEST_ASSERT_FALSE(
        odin3_proc_check_signal(&fx.mctx, (odin3_proc_sigref){n1, {rst->signal}}, &pol));
    TEST_ASSERT_EQUAL_UINT32(WALK_CAP, odin3_proc_test_signal_hops());
    TEST_ASSERT_TRUE(pol);
    fx_close(&fx);
    arst_fail((io){text,
                   "odin3: error: proc: module 'm': 'q' is clocked by both 'clk' and 'rst'; a "
                   "second edge event must be an asynchronous reset tested by the block's only "
                   "top-level 'if'"});
}

/* --- the clock pre-check (§5.5) ------------------------------------------------------------- */

static const char *const k_e1a_text =
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\rst\n  wire input 3 \\a\n"
    "  wire input 4 \\b\n  wire input 5 \\d\n  wire output 6 \\q\n  wire \\t0\n  process $p\n"
    "    switch \\a\n      case 1'1\n        assign \\t0 \\d\n    end\n"
    "    switch \\b\n      case 1'1\n        assign \\t0 1'0\n    end\n" SYNC(
        "posedge", "\\clk", "\\q", "\\t0") SYNC("posedge", "\\rst", "\\q", "\\t0") TAIL;

static const char *const k_e1a_msg =
    "'q' is clocked by both 'clk' and 'rst'; a second edge event must be an asynchronous reset "
    "tested by the block's only top-level 'if'";

static const char *const k_e1b_text =
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\d\n  wire output 3 \\q\n  wire \\t0\n"
    "  process $p\n    assign \\t0 \\d\n" SYNC("posedge", "\\clk", "\\q", "\\t0")
        SYNC("negedge", "\\clk", "\\q", "\\t0") TAIL;

static const char *const k_e1b_msg =
    "'q' is clocked by both edges of 'clk'; dual-edge registers are not supported";

/* clocks_e1a.il: two root switches, so arst finds nothing and q keeps two edge events. */
static void test_clocks_e1a(void) {
    arst_fail((io){k_e1a_text,
                   "odin3: error: proc: module 'm': 'q' is clocked by both 'clk' and 'rst'; "
                   "a second edge event must be an asynchronous reset tested by the "
                   "block's only top-level 'if'"});
}

/* clocks_e1b.il: `always @(posedge clk or negedge clk)`. */
static void test_clocks_e1b(void) {
    arst_fail((io){k_e1b_text,
                   "odin3: error: proc: module 'm': 'q' is clocked by both edges of 'clk'; "
                   "dual-edge registers are not supported"});
}

/* clocks_e5.il: a body that arrives with a level sync only. */
static void test_clocks_e5(void) {
    arst_fail((io){"module \\m\n  wire input 1 \\en\n  wire input 2 \\d\n  wire output 3 \\q\n"
                   "  process $p\n" SYNC("high", "\\en", "\\q", "\\d") TAIL,
                   "odin3: error: proc: module 'm': 'q' is assigned only under level events; a "
                   "register needs a clock edge"});
}

/* clocks_ok_self.il (`always @(posedge a) a <= d;`) and clocks_ok_const.il (`@(posedge 1'b1)`):
 * one edge each, no error (prune promotes t0 <- d); Yosys's proc_dff builds a $dff for both. */
static void test_clocks_ok_self_and_const(void) {
    arst_run((io){"module \\m\n  wire input 1 \\d\n  wire output 2 \\a\n  wire \\t0\n  process $p\n"
                  "    assign \\t0 \\d\n" SYNC("posedge", "\\a", "\\a", "\\t0") TAIL,
                  "  sync posedge \\a\n    update \\a \\t0\nend\n"});
    arst_run((io){"module \\m\n  wire input 1 \\d\n  wire output 2 \\q\n  wire \\t0\n  process $p\n"
                  "    assign \\t0 \\d\n" SYNC("posedge", "1'1", "\\q", "\\t0") TAIL,
                  "  sync posedge 1'1\n    update \\q \\t0\nend\n"});
}

/* clocks_ok_twice.il: one edge sync updates q twice: one edge event, no error (Yosys: $dff).
 * clocks_same_edge_twice.il: two syncs on the same edge of clk are two edge events (Yosys:
 * `Multiple edge sensitive events found for this signal!`); not both edges, so E1a. */
static void test_clocks_twice(void) {
    arst_run((io){PORTS_RST
                  "  wire \\t1\n  process $p\n    assign \\t0 \\d\n    assign \\t1 \\d\n"
                  "    sync posedge \\clk\n      update \\q \\t0\n      update \\q \\t1\n" TAIL,
                  "  sync posedge \\clk\n    update \\q \\t0\n    update \\q \\t1\nend\n"});
    arst_fail(
        (io){PORTS_RST "  process $p\n    assign \\t0 \\d\n" SYNC("posedge", "\\clk", "\\q", "\\t0")
                 SYNC("posedge", "\\clk", "\\q", "\\t0") TAIL,
             "odin3: error: proc: module 'm': 'q' is clocked by both 'clk' and 'clk'; a second "
             "edge event must be an asynchronous reset tested by the block's only top-level "
             "'if'"});
}

/*
 * clocks_name_run.il: q[1:0] has two edge events, q[3:2] one: the error names the run q[1:0], the
 * first bits in key order with that error (Yosys's group is the same register chunk).
 * clocks_name_wire.il: all of q, then r[2] with the same error: the run stops at q's last bit, so
 * the name is the whole wire 'q'.
 */
static void test_clocks_name_run(void) {
    arst_fail((io){"module \\m\n  wire input 1 \\clk\n  wire input 2 \\rst\n"
                   "  wire width 4 input 3 \\d\n  wire width 4 output 4 \\q\n  wire width 4 \\t0\n"
                   "  process $p\n    assign \\t0 \\d\n" SYNC("posedge", "\\clk", "\\q", "\\t0")
                       SYNC("posedge", "\\rst", "\\q [1:0]", "\\t0 [1:0]") TAIL,
                   "odin3: error: proc: module 'm': 'q[1:0]' is clocked by both 'clk' and 'rst'; a "
                   "second edge event must be an asynchronous reset tested by the block's only "
                   "top-level 'if'"});
    arst_fail((io){"module \\m\n  wire input 1 \\clk\n  wire input 2 \\rst\n  wire input 3 \\d\n"
                   "  wire width 2 output 4 \\q\n  wire width 3 output 5 \\r\n"
                   "  wire width 2 \\t0\n  process $p\n    assign \\t0 { \\d \\d }\n"
                   "    sync posedge \\clk\n      update \\q \\t0\n      update \\r [2] \\d\n"
                   "    sync posedge \\rst\n      update \\q \\t0\n      update \\r [2] \\d\n" TAIL,
                   "odin3: error: proc: module 'm': 'q' is clocked by both 'clk' and 'rst'; a "
                   "second edge event must be an asynchronous reset tested by the block's only "
                   "top-level 'if'"});
}

/* --- E4 (§11, point (n)) ---------------------------------------------------------------------- */

/* memwr_e4: arst_posedge with an O temporary wa on pin `port` of a $mem; more_syncs come after the
 * clock's. */
#define MEMWR_TEXT(port, more_syncs)                                                               \
    PORTS_RST "  wire input 5 \\a\n  wire \\wa\n"                                                  \
              "  cell $mem $m1\n    parameter \\MEMID \"m\"\n    connect \\" port " \\wa\n  end\n" \
              "  process $p\n"                                                                     \
              "    switch \\rst\n"                                                                 \
              "      case 1'1\n        assign \\t0 1'0\n        assign \\wa 1'0\n"                 \
              "      case\n        assign \\t0 \\d\n        assign \\wa \\a\n"                     \
              "    end\n" SYNC("posedge", "\\clk", "\\q", "\\t0") more_syncs TAIL

#define MEMWR_RST SYNC("posedge", "\\rst", "\\q", "\\t0")

/* After the reset is found, E4 names the memory and the reset (WR_ADDR, WR_DATA and WR_EN alike).
 * None without a reset (no level sync: the write is an ordinary combinational temporary), on a
 * read pin, or when a sync updates wa (it is then a register, not a write temporary). */
static void test_memwr_e4(void) {
    static const char k_e4[] = "odin3: error: proc: module 'm': memory 'm' is written in an always "
                               "block with asynchronous reset 'rst'";
    arst_fail((io){MEMWR_TEXT("WR_ADDR", MEMWR_RST), k_e4});
    arst_fail((io){MEMWR_TEXT("WR_DATA", MEMWR_RST), k_e4});
    arst_fail((io){MEMWR_TEXT("WR_EN", MEMWR_RST), k_e4});
    const char *const quiet[] = {
        MEMWR_TEXT("WR_ADDR", ""),
        MEMWR_TEXT("RD_ADDR", MEMWR_RST),
        MEMWR_TEXT("WR_ADDR", SYNC("posedge", "\\clk", "\\wa", "\\a") MEMWR_RST),
    };
    for (size_t i = 0; i < sizeof quiet / sizeof quiet[0]; i++) {
        fixture fx;
        fx_open(&fx, quiet[i]);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, fx_arst_clocks(&fx));
        TEST_ASSERT_EQUAL_size_t(0, n_lines);
        fx_close(&fx);
    }
}

/* --- through the pass ------------------------------------------------------------------------ */

/* Text with `attribute \src "t.v:4.3"` before its process line. */
static void with_src(const char *text, char *out) {
    const char *at = strstr(text, "  process $p\n");
    TEST_ASSERT_NOT_NULL(at);
    int len = snprintf(out, TEXT_MAX, "%.*s  attribute \\src \"t.v:4.3\"\n%s", (int)(at - text),
                       text, at);
    TEST_ASSERT_TRUE(len > 0 && len < TEXT_MAX);
}

/* proc on the text fails with one error, want; the module keeps its process. */
static void pass_fails(io test) {
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(test.text, &res));
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_PARSE,
                          odin3_pass_run(res.design, "proc", odin3_bytes_cstr("--noopt")));
    expect_pass_error(test.want);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_module_proc_count(res.module));
    check_full(&res);
    pb_free(&res);
}

/* located: E1a, E1b and E2 at the always (`attribute \src` on the process, point (k)). */
static void test_located(void) {
    static char text[TEXT_MAX];
    static char want[TEXT_MAX];
    const char *const texts[] = {k_e1a_text, k_e1b_text, k_promoted_text};
    const char *const msgs[] = {
        k_e1a_msg, k_e1b_msg, "asynchronous reset 'rst' does not give 'dataout' a constant value"};
    for (size_t i = 0; i < sizeof texts / sizeof texts[0]; i++) {
        with_src(texts[i], text);
        int len = snprintf(want, TEXT_MAX, "odin3: error: t.v:4:3: error: %s", msgs[i]);
        TEST_ASSERT_TRUE(len > 0 && len < TEXT_MAX);
        pass_fails((io){text, want});
    }
}

/* arst_stop_after: proc --stop-after arst re-encodes arst_posedge's process (level sync and all).
 */
static void test_arst_stop_after(void) {
    pb_result res;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build(k_posedge_text, &res));
    n_lines = 0;
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_pass_run(res.design, "proc", odin3_bytes_cstr("--stop-after arst")));
    bool info = false;
    for (size_t i = 0; i < n_lines && i < LOG_LINES; i++) {
        TEST_ASSERT_TRUE(levels[i] != ODIN3_LOG_ERROR && levels[i] != ODIN3_LOG_WARN);
        info = info || strcmp(lines[i], "proc: stopped after arst; 1 processes kept") == 0;
    }
    TEST_ASSERT_TRUE(info);
    check_full(&res);
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_print(res.module, pb_proc(&res, "m", 0), &text));
    TEST_ASSERT_EQUAL_STRING(k_posedge_body, text.data);
    odin3_strbuf_free(&text);
    pb_free(&res);
}

/* --- out of memory --------------------------------------------------------------------------- */

/* arst_oom.il: two nested resets, a promotion through the assign view, a register the reset does
 * not touch, a known switch inside an unknown one. */
static const char *const k_oom_text =
    "module \\m\n  wire input 1 \\clk\n  wire input 2 \\r1\n  wire input 3 \\r2\n"
    "  wire input 4 \\d\n  wire input 5 \\x\n  wire input 6 \\c\n  wire output 7 \\q\n"
    "  wire output 8 \\u\n  wire output 9 \\v\n  wire \\t0\n  wire \\u0\n  wire \\v0\n"
    "  wire \\nr2\n"
    "  cell $logic_not $c1\n" UNARY_PARAMS "    connect \\A \\r2\n    connect \\Y \\nr2\n  end\n"
    "  process $p\n"
    "    assign \\u0 \\u\n"
    "    assign \\v0 \\c\n"
    "    switch \\r1\n"
    "      case 1'1\n"
    "        assign \\t0 1'0\n"
    "        switch \\x\n"
    "          case 1'0\n            assign \\t0 1'1\n"
    "          case\n"
    "            switch \\r1\n              case 1'1\n                assign \\t0 1'0\n"
    "              case\n                assign \\t0 \\d\n            end\n"
    "        end\n"
    "      case\n"
    "        switch \\nr2\n"
    "          case 1'1\n            assign \\t0 1'1\n"
    "          case\n            assign \\t0 \\d\n            assign \\u0 \\d\n"
    "        end\n"
    "    end\n"
    "    sync posedge \\clk\n      update \\q \\t0\n      update \\u \\u0\n"
    "      update \\v \\v0\n"
    "    sync posedge \\r1\n      update \\q \\t0\n      update \\u \\u0\n"
    "    sync negedge \\r2\n      update \\q \\t0\n      update \\u \\u0\n" TAIL;

static const char *const k_oom_body = "  assign \\u0 \\u\n"
                                      "  assign \\t0 \\d\n"
                                      "  assign \\u0 \\d\n"
                                      "  sync high \\r1\n    update \\q 1'0\n    update \\u \\u\n"
                                      "  sync low \\r2\n    update \\q 1'1\n    update \\u \\u\n"
                                      "  sync posedge \\clk\n    update \\q \\t0\n"
                                      "    update \\u \\u0\n    update \\v \\v0\n"
                                      "end\n";

/* One injection: NO_MEMORY with a valid tree and a clear scratch index, or the full result. */
static bool oom_step(long fail) {
    fixture fx;
    fx_open(&fx, k_oom_text);
    odin3_util_set_alloc_fail_after(fail);
    odin3_status st = odin3_pw_arst(&fx.mctx, &fx.pw);
    odin3_util_set_alloc_fail_after(-1);
    TEST_ASSERT_TRUE_MESSAGE(st == ODIN3_OK || st == ODIN3_ERR_NO_MEMORY, "status");
    expect_scratch_clear(&fx);
    TEST_ASSERT_EQUAL_size_t(0, n_lines);
    if (st == ODIN3_OK) {
        fx_expect(&fx, k_oom_body);
    } else {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_pw_encode(&fx.mctx, &fx.pw));
        check_full(&fx.res);
    }
    fx_close(&fx);
    return st == ODIN3_OK;
}

static void test_arst_oom(void) {
    arst_run((io){k_oom_text, k_oom_body});
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
    RUN_TEST(test_arst_posedge);
    RUN_TEST(test_arst_forms);
    RUN_TEST(test_check_signal);
    RUN_TEST(test_arst_rename_wire);
    RUN_TEST(test_arst_after_trio_merge);
    RUN_TEST(test_arst_two_resets);
    RUN_TEST(test_arst_vhdl_style);
    RUN_TEST(test_arst_self_value);
    RUN_TEST(test_arst_known_inside_unknown);
    RUN_TEST(test_arst_default_is_reset);
    RUN_TEST(test_arst_switch_order);
    RUN_TEST(test_arst_later_cases);
    RUN_TEST(test_arst_assign_view);
    RUN_TEST(test_arst_reorder);
    RUN_TEST(test_arst_apply_promoted_e2);
    RUN_TEST(test_arst_nested_unknown);
    RUN_TEST(test_arst_round_cap);
    RUN_TEST(test_arst_walk_cap);
    RUN_TEST(test_clocks_e1a);
    RUN_TEST(test_clocks_e1b);
    RUN_TEST(test_clocks_e5);
    RUN_TEST(test_clocks_ok_self_and_const);
    RUN_TEST(test_clocks_twice);
    RUN_TEST(test_clocks_name_run);
    RUN_TEST(test_memwr_e4);
    RUN_TEST(test_located);
    RUN_TEST(test_arst_stop_after);
    RUN_TEST(test_arst_oom);
    return UNITY_END();
}
