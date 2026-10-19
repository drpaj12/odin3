/*
 * test_lpmmap_ram.c — lpmmap, plan Task 8: an inferred `$mem` to `altsyncram` (5.1 spec R-1..R-9,
 * F-1, H-6, H-8). The `$mem` is built as 2D EL-39 emits it (asynchronous read, raw addresses, one
 * write enable replicated over the data bits), with the flip-flops R-1 merges around it, from flat
 * RTLIL with lib/lpm.o3lib read; lm_run checks the IR (FULL) before and after.
 */
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "lpmmap_fixture.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/attr.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { LOG_TEXT = 16384, TEXT_MAX = 8192, OOM_LIMIT = 8000 };

static char log_text[LOG_TEXT];

static void text_sink(odin3_log_level level, const char *msg, void *user) {
    (void)level;
    (void)user;
    size_t used = strlen(log_text);
    if (used + strlen(msg) + 2 < sizeof log_text) {
        (void)snprintf(log_text + used, sizeof log_text - used, "%s\n", msg);
    }
}

void setUp(void) {
    log_text[0] = '\0';
    odin3_log_set_sink(text_sink, NULL);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_log_set_level(ODIN3_LOG_INFO));
}

void tearDown(void) {
    odin3_util_set_alloc_fail_after(-1);
    odin3_log_set_sink(NULL, NULL);
}

/* How the read port is surrounded. */
typedef enum rmode {
    R_SYNC,    /* a synchronous read port: RD_CLK_ENABLE 1, RD_DATA is the output */
    R_ASYNC,   /* asynchronous, RD_DATA is the output (stays soft) */
    R_DFF,     /* asynchronous, RD_DATA feeds a `$dff` on the clock (R-1 data merge) */
    R_DFFE_VAR /* ... a `$dffe` whose enable is the input wire ren (stays soft: RD_EN) */
} rmode;

typedef struct memcase {
    uint32_t abits, width;
    uint32_t size; /* 0: 2^abits */
    int32_t offset;
    uint32_t n_rd, n_wr; /* 1 or 0 / 2 for the unmapped patterns */
    rmode mode;
    bool transparent;
    bool same_addr; /* RD_ADDR is the write address */
    bool rd_neg, wr_neg;
    bool rd_clk2; /* the read port is on clk2 */
    bool init;
    bool split_en;
    bool en_var;  /* R_SYNC: RD_EN is the input wire ren */
    bool rd_init; /* the output wire q carries init 8'hAA (a read register with an init value) */
} memcase;

static void put(odin3_strbuf *out, const char *fmt, ...) ODIN3_PRINTF(2, 3);

static void put(odin3_strbuf *out, const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(buf)));
}

typedef struct catref {
    const char *pin;  /* the formal */
    const char *wire; /* the wire repeated over the ports */
} catref;

static void put_cat(odin3_strbuf *out, catref ref, uint32_t count) {
    put(out, "    connect \\%s {", ref.pin);
    for (uint32_t i = 0; i < count; i++) {
        put(out, " \\%s", ref.wire);
    }
    put(out, " }\n");
}

static void decl(odin3_strbuf *out, const memcase *mc) {
    uint32_t port = 1;
    put(out, "module \\m\n  wire input %" PRIu32 " \\clk\n  wire input %" PRIu32 " \\clk2\n", port,
        port + 1);
    port += 2;
    if (mc->abits > 0) {
        put(out, "  wire width %" PRIu32 " input %" PRIu32 " \\ra\n", mc->abits, port++);
        put(out, "  wire width %" PRIu32 " input %" PRIu32 " \\wa\n", mc->abits, port++);
    }
    put(out, "  wire input %" PRIu32 " \\ren\n  wire width %" PRIu32 " input %" PRIu32 " \\wd\n",
        port, mc->width, port + 1);
    port += 2;
    put(out, "  wire input %" PRIu32 " \\we\n  wire input %" PRIu32 " \\we2\n", port, port + 1);
    port += 2;
    if (mc->rd_init) {
        put(out, "  attribute \\init %" PRIu32 "'10101010\n", mc->width);
    }
    put(out, "  wire width %" PRIu32 " output %" PRIu32 " \\q\n", mc->width, port);
    if (mc->mode == R_DFF || mc->mode == R_DFFE_VAR) {
        put(out, "  wire width %" PRIu32 " \\rd\n", mc->width);
    }
}

typedef struct bitspec {
    uint32_t count;
    uint32_t mask;
} bitspec;

static void bits(odin3_strbuf *out, const char *name, bitspec spec) {
    put(out, "    parameter \\%s %" PRIu32 "'", name, spec.count);
    for (uint32_t i = spec.count; i > 0; i--) {
        put(out, "%c", ((spec.mask >> (i - 1)) & 1U) != 0 ? '1' : '0');
    }
    put(out, "\n");
}

static uint32_t size_of(const memcase *mc) {
    return mc->size != 0 ? mc->size : (uint32_t)1 << mc->abits;
}

static void params(odin3_strbuf *out, const memcase *mc) {
    put(out,
        "  cell $mem \\u\n    parameter \\MEMID \"\\\\mem\"\n    parameter \\SIZE %" PRIu32
        "\n    parameter \\OFFSET %" PRId32 "\n    parameter \\ABITS %" PRIu32
        "\n    parameter \\WIDTH %" PRIu32 "\n    parameter \\RD_PORTS %" PRIu32
        "\n    parameter \\WR_PORTS %" PRIu32 "\n",
        size_of(mc), mc->offset, mc->abits, mc->width, mc->n_rd, mc->n_wr);
    uint32_t rmask = (1U << mc->n_rd) - 1U;
    bits(out, "RD_CLK_ENABLE", (bitspec){mc->n_rd, mc->mode == R_SYNC ? rmask : 0});
    bits(out, "RD_CLK_POLARITY", (bitspec){mc->n_rd, mc->rd_neg ? 0 : rmask});
    bits(out, "RD_TRANSPARENT", (bitspec){mc->n_rd, mc->transparent ? rmask : 0});
    uint32_t wmask = (1U << mc->n_wr) - 1U;
    bits(out, "WR_CLK_ENABLE", (bitspec){mc->n_wr, wmask});
    bits(out, "WR_CLK_POLARITY", (bitspec){mc->n_wr, mc->wr_neg ? 0 : wmask});
    if (mc->init) {
        uint32_t total = size_of(mc) * mc->width;
        put(out, "    parameter \\INIT %" PRIu32 "'1", total);
        for (uint32_t i = 1; i < total; i++) {
            put(out, "x");
        }
        put(out, "\n");
    } else {
        put(out, "    parameter \\INIT 0'\n");
    }
}

static void connect_read_ctl(odin3_strbuf *out, const memcase *mc) {
    const char *rclk = mc->rd_clk2 ? "clk2" : "clk";
    if (mc->mode == R_SYNC) {
        put_cat(out, (catref){"RD_CLK", rclk}, mc->n_rd);
    } else {
        put(out, "    connect \\RD_CLK {");
        for (uint32_t i = 0; i < mc->n_rd; i++) {
            put(out, " 1'x");
        }
        put(out, " }\n");
    }
    put(out, "    connect \\RD_EN {");
    for (uint32_t i = 0; i < mc->n_rd; i++) {
        put(out, mc->mode == R_SYNC && mc->en_var ? " \\ren" : " 1'1");
    }
    put(out, " }\n");
}

static void connect_read(odin3_strbuf *out, const memcase *mc) {
    const char *raddr = mc->same_addr ? "wa" : "ra";
    connect_read_ctl(out, mc);
    put_cat(out, (catref){"RD_ADDR", raddr}, mc->abits > 0 ? mc->n_rd : 0);
    if (mc->n_rd == 0) {
        put(out, "    connect \\RD_DATA { }\n");
    } else {
        const char *wire = mc->mode == R_DFF || mc->mode == R_DFFE_VAR ? "\\rd\n" : "\\q\n";
        put(out, "    connect \\RD_DATA %s", wire);
    }
}

static void connects(odin3_strbuf *out, const memcase *mc) {
    connect_read(out, mc);
    put_cat(out, (catref){"WR_ADDR", "wa"}, mc->abits > 0 ? mc->n_wr : 0);
    put_cat(out, (catref){"WR_CLK", "clk"}, mc->n_wr);
    put(out, "    connect \\WR_EN {");
    for (uint32_t i = 0; i < mc->n_wr * mc->width; i++) {
        put(out, mc->split_en && i == 0 ? " \\we2" : " \\we");
    }
    put(out, " }\n");
    put_cat(out, (catref){"WR_DATA", "wd"}, mc->n_wr);
    put(out, "  end\n");
}

static void ff(odin3_strbuf *out, const memcase *mc) {
    if (mc->mode != R_DFF && mc->mode != R_DFFE_VAR) {
        return;
    }
    bool en = mc->mode == R_DFFE_VAR;
    put(out, "  cell %s \\ff\n    parameter \\WIDTH %" PRIu32 "\n    parameter \\CLK_POLARITY 1\n",
        en ? "$dffe" : "$dff", mc->width);
    if (en) {
        put(out, "    parameter \\EN_POLARITY 1\n    connect \\EN \\ren\n");
    }
    put(out, "    connect \\CLK \\clk\n    connect \\D \\rd\n    connect \\Q \\q\n  end\n");
}

static void build(memcase mc, pb_result *res) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    decl(&text, &mc);
    params(&text, &mc);
    connects(&text, &mc);
    ff(&text, &mc);
    put(&text, "end\n");
    lm_build_ok(text.data, res);
    odin3_strbuf_free(&text);
}

static const memcase k_sp = {
    .abits = 4, .width = 8, .n_rd = 1, .n_wr = 1, .mode = R_SYNC, .same_addr = true};
static const memcase k_sdp = {.abits = 4, .width = 8, .n_rd = 1, .n_wr = 1, .mode = R_SYNC};

typedef struct snap {
    odin3_net_id wd[16], wa[8], ra[8], q[16], we, clk;
} snap;

typedef struct pinref {
    uint32_t port;
    uint32_t bit;
} pinref;

static odin3_net_id mem_pin(const odin3_module *mod, odin3_node_id node, pinref ref) {
    odin3_pinslice pins = odin3_node_port(mod, node, ref.port);
    return ref.bit < pins.count ? odin3_pin_net(mod, (odin3_pin_id){pins.first.v + ref.bit})
                                : (odin3_net_id){0};
}

/* The nets of the `$mem`'s data pins before the run. */
enum { MP_RD_CLK, MP_RD_EN, MP_RD_ADDR, MP_RD_DATA, MP_WR_CLK, MP_WR_EN, MP_WR_ADDR, MP_WR_DATA };

static void snapshot(const pb_result *res, const memcase *mc, snap *sn) {
    memset(sn, 0, sizeof *sn);
    odin3_node_id mem = lm_nth(res->module, "$mem", 0);
    for (uint32_t i = 0; i < mc->width; i++) {
        sn->wd[i] = mem_pin(res->module, mem, (pinref){MP_WR_DATA, i});
        sn->q[i] = mem_pin(res->module, mem, (pinref){MP_RD_DATA, i});
    }
    for (uint32_t i = 0; i < mc->abits; i++) {
        sn->wa[i] = mem_pin(res->module, mem, (pinref){MP_WR_ADDR, i});
        sn->ra[i] = mem_pin(res->module, mem, (pinref){MP_RD_ADDR, i});
    }
    sn->we = mem_pin(res->module, mem, (pinref){MP_WR_EN, 0});
    sn->clk = mem_pin(res->module, mem, (pinref){MP_WR_CLK, 0});
}

static bool is_const(const odin3_module *mod, odin3_net_id net, odin3_const value) {
    return odin3_net_valid(net) && odin3_net_const_value(mod, net) == value;
}

static void expect_soft(memcase mc, const char *needle) {
    pb_result res;
    build(mc, &res);
    odin3_lpmmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, &stats));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, lm_count(res.module, "$mem"), needle);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, lm_count(res.module, "altsyncram"), needle);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(1, stats.soft, needle);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(log_text, needle), log_text);
    pb_free(&res);
}

/* --- R-2, R-3: single port ------------------------------------------------------------------ */

static void test_r2_single_port(void) {
    pb_result res;
    build(k_sp, &res);
    snap sn;
    snapshot(&res, &k_sp, &sn);
    odin3_lpmmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, &stats));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "altsyncram"));
    TEST_ASSERT_EQUAL_UINT32(0, lm_count(res.module, "$mem"));
    TEST_ASSERT_EQUAL_UINT64(1, stats.ram);
    odin3_node_id blk = lm_nth(res.module, "altsyncram", 0);
    TEST_ASSERT_EQUAL_STRING("SINGLE_PORT", lm_param_str(res.module, blk, "operation_mode"));
    for (uint32_t i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT32(sn.wd[i].v, lm_pin_net(res.module, blk, "data_a", i).v);
        TEST_ASSERT_EQUAL_UINT32(sn.q[i].v, lm_pin_net(res.module, blk, "q_a", i).v);
    }
    for (uint32_t i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT32(sn.wa[i].v, lm_pin_net(res.module, blk, "address_a", i).v);
    }
    TEST_ASSERT_EQUAL_UINT32(sn.we.v, lm_pin_net(res.module, blk, "wren_a", 0).v);
    TEST_ASSERT_EQUAL_UINT32(sn.clk.v, lm_pin_net(res.module, blk, "clock0", 0).v);
    TEST_ASSERT_TRUE(lm_port_open(res.module, blk, "q_b"));
    pb_free(&res);
}

/* Every bit of each listed port of blk is the constant `value`. */
typedef struct portset {
    const char *const *names;
    size_t count;
} portset;

static void expect_ports_const(const pb_result *res, odin3_node_id blk, portset set,
                               odin3_const value) {
    for (size_t i = 0; i < set.count; i++) {
        uint32_t width = lm_port_width(res->module, blk, set.names[i]);
        for (uint32_t bit = 0; bit < width; bit++) {
            TEST_ASSERT_TRUE_MESSAGE(
                is_const(res->module, lm_pin_net(res->module, blk, set.names[i], bit), value),
                set.names[i]);
        }
    }
}

static void test_r3_tie_offs(void) {
    pb_result res;
    build(k_sp, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "altsyncram", 0);
    static const char *const ones[] = {"rden_a",   "clock1",   "clocken0",
                                       "clocken1", "clocken2", "clocken3"};
    static const char *const zeros[] = {"aclr0",  "aclr1",  "addressstall_a", "addressstall_b",
                                        "wren_b", "rden_b", "data_b",         "address_b"};
    expect_ports_const(&res, blk, (portset){ones, sizeof ones / sizeof ones[0]}, ODIN3_CONST_1);
    expect_ports_const(&res, blk, (portset){zeros, sizeof zeros / sizeof zeros[0]}, ODIN3_CONST_0);
    pb_free(&res);
}

static void test_r3_params(void) {
    memcase mc = k_sp;
    mc.size = 12; /* SIZE below 2^ABITS (agent default (f)) */
    pb_result res;
    build(mc, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "altsyncram", 0);
    TEST_ASSERT_EQUAL_INT64(8, lm_param_int(res.module, blk, "width_a"));
    TEST_ASSERT_EQUAL_INT64(8, lm_param_int(res.module, blk, "width_b"));
    TEST_ASSERT_EQUAL_INT64(4, lm_param_int(res.module, blk, "widthad_a"));
    TEST_ASSERT_EQUAL_INT64(4, lm_param_int(res.module, blk, "widthad_b"));
    TEST_ASSERT_EQUAL_INT64(12, lm_param_int(res.module, blk, "numwords_a"));
    TEST_ASSERT_EQUAL_INT64(12, lm_param_int(res.module, blk, "numwords_b"));
    TEST_ASSERT_EQUAL_STRING("altsyncram", lm_param_str(res.module, blk, "lpm_type"));
    pb_free(&res);
}

/* --- R-4: simple dual port ------------------------------------------------------------------ */

/* The first `count` bits of port `port` of blk are the nets src[0..count). */
static void expect_nets(const pb_result *res, odin3_node_id blk, const char *port,
                        const odin3_net_id *src, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(src[i].v, lm_pin_net(res->module, blk, port, i).v, port);
    }
}

static void test_r2_simple_dual(void) {
    pb_result res;
    build(k_sdp, &res);
    snap sn;
    snapshot(&res, &k_sdp, &sn);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "altsyncram", 0);
    TEST_ASSERT_EQUAL_STRING("DUAL_PORT", lm_param_str(res.module, blk, "operation_mode"));
    TEST_ASSERT_EQUAL_STRING("CLOCK0", lm_param_str(res.module, blk, "address_reg_b"));
    TEST_ASSERT_EQUAL_STRING("UNREGISTERED", lm_param_str(res.module, blk, "outdata_reg_b"));
    expect_nets(&res, blk, "data_a", sn.wd, 8);
    expect_nets(&res, blk, "q_b", sn.q, 8);
    expect_nets(&res, blk, "address_a", sn.wa, 4);
    expect_nets(&res, blk, "address_b", sn.ra, 4);
    static const char *const zeros[] = {"data_b", "wren_b"};
    expect_ports_const(&res, blk, (portset){zeros, 2}, ODIN3_CONST_0);
    TEST_ASSERT_TRUE(lm_port_open(res.module, blk, "q_a"));
    TEST_ASSERT_TRUE(is_const(res.module, lm_pin_net(res.module, blk, "rden_b", 0), ODIN3_CONST_1));
    pb_free(&res);
}

/* --- R-5 ------------------------------------------------------------------------------------ */

static void test_r5_read_during_write(void) {
    pb_result res;
    memcase mc = k_sp;
    build(mc, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("OLD_DATA",
                             lm_param_str(res.module, lm_nth(res.module, "altsyncram", 0),
                                          "read_during_write_mode_port_a"));
    pb_free(&res);
    mc.transparent = true;
    build(mc, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("NEW_DATA_NO_NBE_READ",
                             lm_param_str(res.module, lm_nth(res.module, "altsyncram", 0),
                                          "read_during_write_mode_port_a"));
    pb_free(&res);
    mc = k_sdp;
    build(mc, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("OLD_DATA",
                             lm_param_str(res.module, lm_nth(res.module, "altsyncram", 0),
                                          "read_during_write_mode_mixed_ports"));
    pb_free(&res);
    mc.transparent = true;
    expect_soft(mc, "lpmmap: memory 'mem' has a transparent read across two ports; left soft");
}

/* --- R-1 ------------------------------------------------------------------------------------ */

static void test_r1_data_merge(void) {
    memcase mc = k_sdp;
    mc.mode = R_DFF;
    pb_result res;
    build(mc, &res);
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "$dff"));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "altsyncram"));
    TEST_ASSERT_EQUAL_UINT32(0, lm_count(res.module, "$mem"));
    TEST_ASSERT_EQUAL_UINT32(0, lm_count(res.module, "$dff"));
    pb_free(&res);
}

static void test_r1_soft_keeps_ffs(void) {
    memcase mc = k_sdp;
    mc.mode = R_DFF;
    mc.offset = 3;
    pb_result res;
    build(mc, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "$mem"));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "$dff"));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "lpmmap: memory 'mem' has OFFSET 3; left soft"));
    pb_free(&res);
}

/* --- R-6, R-2 soft cases --------------------------------------------------------------------- */

static void test_r6_soft_cases(void) {
    memcase mc = k_sp;
    mc.rd_neg = true;
    mc.wr_neg = true;
    expect_soft(mc, "lpmmap: memory 'mem' has a port on the falling clock edge; left soft");
    mc = k_sp;
    mc.rd_clk2 = true;
    expect_soft(mc, "lpmmap: memory 'mem' has read and write ports on different clocks; left soft");
    mc = k_sp;
    mc.init = true;
    expect_soft(mc, "lpmmap: memory 'mem' has initial contents; left soft");
    mc = k_sp;
    mc.split_en = true;
    expect_soft(mc, "lpmmap: memory 'mem' has a write enable that is not one net; left soft");
    mc = k_sp;
    mc.mode = R_ASYNC;
    expect_soft(mc, "lpmmap: memory 'mem' has an asynchronous read port; left soft");
    mc = k_sp;
    mc.en_var = true;
    expect_soft(mc, "lpmmap: memory 'mem' has a read enable that is not constant 1; left soft");
    mc = k_sp;
    mc.mode = R_DFFE_VAR;
    expect_soft(mc, "lpmmap: memory 'mem' has a read enable that is not constant 1; left soft");
}

static void test_h1_read_register_init_stays_soft(void) {
    memcase mc = k_sdp;
    mc.mode = R_DFF;
    mc.rd_init = true;
    pb_result res;
    build(mc, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "$mem"));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "$dff"));
    TEST_ASSERT_EQUAL_UINT32(0, lm_count(res.module, "altsyncram"));
    TEST_ASSERT_NOT_NULL(strstr(
        log_text, "lpmmap: memory 'mem' has a read register with an initial value; left soft"));
    pb_free(&res);
    /* the same memory without the init maps */
    mc.rd_init = false;
    build(mc, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "altsyncram"));
    pb_free(&res);
}

static void test_r2_unmapped_patterns(void) {
    memcase mc = k_sp;
    mc.n_wr = 0;
    expect_soft(mc, "lpmmap: memory 'mem' (1 read, 0 write ports) has no 5.1 mapping; left soft");
    mc = k_sp;
    mc.n_rd = 0;
    expect_soft(mc, "lpmmap: memory 'mem' (0 read, 1 write ports) has no 5.1 mapping; left soft");
}

/* --- R-7 ------------------------------------------------------------------------------------ */

static void test_r7_widths(void) {
    memcase mc = k_sp;
    mc.abits = 13;
    mc.width = 12;
    pb_result res;
    build(mc, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "altsyncram"));
    odin3_node_id blk = lm_nth(res.module, "altsyncram", 0);
    TEST_ASSERT_EQUAL_INT64(13, lm_param_int(res.module, blk, "widthad_a"));
    TEST_ASSERT_EQUAL_INT64(12, lm_param_int(res.module, blk, "width_a"));
    TEST_ASSERT_EQUAL_UINT32(13, lm_port_width(res.module, blk, "address_a"));
    pb_free(&res);
}

static void test_r7_abits_zero(void) {
    memcase mc = k_sp;
    mc.abits = 0;
    mc.size = 1;
    pb_result res;
    build(mc, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    odin3_node_id blk = lm_nth(res.module, "altsyncram", 0);
    TEST_ASSERT_EQUAL_INT64(1, lm_param_int(res.module, blk, "widthad_a"));
    TEST_ASSERT_TRUE(
        is_const(res.module, lm_pin_net(res.module, blk, "address_a", 0), ODIN3_CONST_0));
    pb_free(&res);
}

/* --- F-1 ------------------------------------------------------------------------------------ */

static void test_f1_family(void) {
    pb_result res;
    build(k_sp, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
    TEST_ASSERT_EQUAL_STRING(
        "Cyclone V",
        lm_param_str(res.module, lm_nth(res.module, "altsyncram", 0), "intended_device_family"));
    pb_free(&res);
    build(k_sdp, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, "Stratix IV", NULL));
    TEST_ASSERT_EQUAL_STRING(
        "Stratix IV",
        lm_param_str(res.module, lm_nth(res.module, "altsyncram", 0), "intended_device_family"));
    pb_free(&res);
}

/* --- R-8 ------------------------------------------------------------------------------------ */

/* The attribute `lpmmap` of blk is expected. */
static void expect_lpmmap_attr(const pb_result *res, odin3_node_id blk, const char *expected) {
    uint32_t key = 0;
    TEST_ASSERT_TRUE(
        odin3_strtab_find(odin3_design_strtab(res->design), odin3_bytes_cstr("lpmmap"), &key));
    const odin3_value *val =
        odin3_attr_get(res->module, (odin3_objref){ODIN3_OBJ_NODE, blk.v}, key);
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_STRING(expected,
                             odin3_strtab_get(odin3_design_strtab(res->design), val->str));
}

static void test_r8_prov_and_attr(void) {
    static const memcase *const cases[] = {&k_sp, &k_sdp};
    static const char *const roles[] = {"altsyncram:sp", "altsyncram:sdp"};
    for (size_t i = 0; i < 2; i++) {
        pb_result res;
        build(*cases[i], &res);
        odin3_prov_id source = odin3_node_prov(res.module, lm_nth(res.module, "$mem", 0));
        TEST_ASSERT_TRUE(source.v != 0);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, lm_run(&res, NULL, NULL));
        odin3_node_id blk = lm_nth(res.module, "altsyncram", 0);
        const odin3_prov_record *rec = odin3_prov_get(res.design, odin3_node_prov(res.module, blk));
        TEST_ASSERT_NOT_NULL(rec);
        TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
        TEST_ASSERT_EQUAL_UINT32(source.v, rec->parents.ids[0].v);
        expect_lpmmap_attr(&res, blk, roles[i]);
        pb_free(&res);
    }
}

static void oom_sweep(memcase mc, const char *type_after) {
    int mapped = 0;
    for (int fail_at = 0; fail_at < OOM_LIMIT && mapped == 0; fail_at++) {
        pb_result res;
        build(mc, &res);
        odin3_util_set_alloc_fail_after(fail_at);
        odin3_status st = odin3_lpmmap_run(&res.ctx, res.design, NULL, NULL);
        odin3_util_set_alloc_fail_after(-1);
        if (st == ODIN3_OK) {
            mapped = 1;
            TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, type_after));
        } else {
            TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
            TEST_ASSERT_NULL(strstr(log_text, "no longer matches"));
            TEST_ASSERT_EQUAL_UINT32(1, lm_count(res.module, "$mem"));
            TEST_ASSERT_EQUAL_UINT32(0, lm_count(res.module, "altsyncram"));
            odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res.design, opts));
        }
        pb_free(&res);
    }
    TEST_ASSERT_EQUAL_INT(1, mapped);
}

static void test_r_oom(void) {
    oom_sweep(k_sp, "altsyncram");
    memcase mc = k_sdp;
    mc.mode = R_DFF;
    oom_sweep(mc, "altsyncram");
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_r2_single_port);
    RUN_TEST(test_r3_tie_offs);
    RUN_TEST(test_r3_params);
    RUN_TEST(test_r2_simple_dual);
    RUN_TEST(test_r5_read_during_write);
    RUN_TEST(test_r1_data_merge);
    RUN_TEST(test_r1_soft_keeps_ffs);
    RUN_TEST(test_r6_soft_cases);
    RUN_TEST(test_h1_read_register_init_stays_soft);
    RUN_TEST(test_r2_unmapped_patterns);
    RUN_TEST(test_r7_widths);
    RUN_TEST(test_r7_abits_zero);
    RUN_TEST(test_f1_family);
    RUN_TEST(test_r8_prov_and_attr);
    RUN_TEST(test_r_oom);
    return UNITY_END();
}
