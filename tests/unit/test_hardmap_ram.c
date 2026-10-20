/*
 * test_hardmap_ram.c — hardmap, PR 3C-3: explicit RAM instances (3C spec R-1..R-7, H-4, H-7, H-9;
 * plan Task 5 Step 1). Every case is flat RTLIL with lib/vtr.o3lib read (pb_build_lib); hm_run
 * checks the IR (FULL) before and after. The 1E simulator has no model of a hard RAM, so the
 * value-preserving cases use hardmap_ramsim.c (a read-first, registered-read model) on the IR
 * before and after the pass.
 */
#include "hardmap_fixture.h"
#include "hardmap_ramsim.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/partial_map/hardmap.h"
#include "proc_build.h"
#include "unity.h"
#include "util/alloc.h"
#include "util/attr.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    LOG_TEXT = 16384,
    OOM_LIMIT = 20000,
    OOM_QUIET = 4,
    MAX_NETS = 128,
    SIM_CYCLES = 3000,
    MIN_ONES = 200,
    /* single_port_ram pins */
    SP_CLK = 0,
    SP_DATA = 1,
    SP_ADDR = 2,
    SP_WE = 3,
    SP_OUT = 4,
    /* dual_port_ram pins */
    DP_CLK = 0,
    DP_DATA2 = 1,
    DP_DATA1 = 2,
    DP_ADDR2 = 3,
    DP_ADDR1 = 4,
    DP_WE2 = 5,
    DP_WE1 = 6,
    DP_OUT2 = 7,
    DP_OUT1 = 8,
    /* glue pins */
    G_AND_A = 0,
    G_AND_B = 1,
    G_AND_Y = 2,
    G_NOT_A = 0,
    G_NOT_Y = 1,
    G_MUX_A = 0,
    G_MUX_B = 1,
    G_MUX_S = 2,
    G_MUX_Y = 3,
    G_DFF_CLK = 0,
    G_DFF_D = 1,
    G_DFF_Q = 2,
    ARCH_ADDR = 15
};

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

/* --- builders ------------------------------------------------------------------------------ */

/* One RAM instance `u`. we1 / we2 (we for a single port): NULL is the input wire, else the signal
 * text (a constant such as "1'0"). */
typedef struct ramcase {
    bool dual;
    uint32_t addr;
    uint32_t data;
    const char *we1;
    const char *we2;
} ramcase;

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

static void wire_decl(odin3_strbuf *out, const char *dir, uint32_t width, uint32_t *port,
                      const char *name) {
    put(out, "  wire width %" PRIu32 " %s %" PRIu32 " \\%s\n", width, dir, (*port)++, name);
}

static void ram_text(odin3_strbuf *out, const ramcase *rc) {
    uint32_t port = 1;
    put(out, "module \\m\n");
    wire_decl(out, "input", 1, &port, "clk");
    if (rc->dual) {
        wire_decl(out, "input", rc->data, &port, "data1");
        wire_decl(out, "input", rc->data, &port, "data2");
        wire_decl(out, "input", rc->addr, &port, "addr1");
        wire_decl(out, "input", rc->addr, &port, "addr2");
        if (rc->we1 == NULL) {
            wire_decl(out, "input", 1, &port, "we1");
        }
        if (rc->we2 == NULL) {
            wire_decl(out, "input", 1, &port, "we2");
        }
        wire_decl(out, "output", rc->data, &port, "out1");
        wire_decl(out, "output", rc->data, &port, "out2");
        put(out,
            "  cell \\dual_port_ram \\u\n    parameter \\ADDR_WIDTH %" PRIu32
            "\n    parameter \\DATA_WIDTH %" PRIu32
            "\n    connect \\clk \\clk\n    connect \\data1 \\data1\n    connect \\data2 \\data2\n"
            "    connect \\addr1 \\addr1\n    connect \\addr2 \\addr2\n    connect \\we1 %s\n"
            "    connect \\we2 %s\n    connect \\out1 \\out1\n    connect \\out2 \\out2\n  end\n",
            rc->addr, rc->data, rc->we1 != NULL ? rc->we1 : "\\we1",
            rc->we2 != NULL ? rc->we2 : "\\we2");
    } else {
        wire_decl(out, "input", rc->data, &port, "data");
        wire_decl(out, "input", rc->addr, &port, "addr");
        if (rc->we1 == NULL) {
            wire_decl(out, "input", 1, &port, "we");
        }
        wire_decl(out, "output", rc->data, &port, "out");
        put(out,
            "  cell \\single_port_ram \\u\n    parameter \\ADDR_WIDTH %" PRIu32
            "\n    parameter \\DATA_WIDTH %" PRIu32
            "\n    connect \\clk \\clk\n    connect \\data \\data\n    connect \\addr \\addr\n"
            "    connect \\we %s\n    connect \\out \\out\n  end\n",
            rc->addr, rc->data, rc->we1 != NULL ? rc->we1 : "\\we");
    }
    put(out, "end\n");
}

static void build_ram(ramcase rc, pb_result *res) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    ram_text(&text, &rc);
    hm_build_ok(text.data, res);
    odin3_strbuf_free(&text);
}

static odin3_net_id wnet(const pb_result *res, const char *name, uint32_t bit) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(res->design, odin3_bytes_cstr(name), &str));
    odin3_wire_id wire = odin3_module_find_wire(res->module, str);
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    return odin3_wire_net(res->module, wire, bit);
}

/* The net on pin `bit` of port `port` of node. */
typedef struct pin_ref {
    uint32_t port;
    uint32_t bit;
} pin_ref;

static odin3_net_id pin_at(const pb_result *res, odin3_node_id node, pin_ref pin) {
    odin3_pinslice pins = odin3_node_port(res->module, node, pin.port);
    TEST_ASSERT_TRUE(pin.bit < pins.count);
    return odin3_pin_net(res->module, (odin3_pin_id){pins.first.v + pin.bit});
}

#define pin_of(res, node, port, bit) pin_at((res), (node), (pin_ref){(port), (bit)})

static void expect_net(odin3_net_id want, odin3_net_id got) {
    TEST_ASSERT_TRUE(odin3_net_valid(want));
    TEST_ASSERT_EQUAL_UINT32(want.v, got.v);
}

static bool is_type(const pb_result *res, odin3_node_id node, const char *type) {
    return odin3_node_live(res->module, node) &&
           strcmp(odin3_celltype_get(res->design, odin3_node_type(res->module, node))->name,
                  type) == 0;
}

/* The first live node of `type` whose pin (port, bit) is on `net`; none when absent. */
static odin3_node_id find_on(const pb_result *res, const char *type, pin_ref pin,
                             odin3_net_id net) {
    for (uint32_t id = 1; id < odin3_module_node_end(res->module); id++) {
        odin3_node_id node = {id};
        if (is_type(res, node, type) &&
            pin.bit < odin3_node_port(res->module, node, pin.port).count &&
            pin_at(res, node, pin).v == net.v) {
            return node;
        }
    }
    return (odin3_node_id){0};
}

static odin3_node_id must_find(const pb_result *res, const char *type, uint32_t port,
                               odin3_net_id net) {
    odin3_node_id node = find_on(res, type, (pin_ref){port, 0}, net);
    TEST_ASSERT_TRUE_MESSAGE(odin3_node_valid(node), type);
    return node;
}

static int64_t param_int(const pb_result *res, odin3_node_id node, uint32_t index) {
    const odin3_value *val = odin3_node_param(res->module, node, index);
    TEST_ASSERT_NOT_NULL(val);
    return val->i;
}

/* A slice has the arch's shape: ADDR_WIDTH 15, DATA_WIDTH 1, and the pin counts to match. */
static void expect_slice(const pb_result *res, odin3_node_id node, bool dual) {
    TEST_ASSERT_EQUAL_INT64(ARCH_ADDR, param_int(res, node, 0));
    TEST_ASSERT_EQUAL_INT64(1, param_int(res, node, 1));
    TEST_ASSERT_EQUAL_UINT32(ARCH_ADDR,
                             odin3_node_port(res->module, node, dual ? DP_ADDR1 : SP_ADDR).count);
    TEST_ASSERT_EQUAL_UINT32(1, odin3_node_port(res->module, node, dual ? DP_OUT1 : SP_OUT).count);
}

static const char *node_name_of(const pb_result *res, odin3_node_id node) {
    return odin3_strtab_get(odin3_design_strtab(res->design), odin3_node_name(res->module, node));
}

static void assert_rtlil_equal(const pb_result *res, const odin3_strbuf *expected) {
    odin3_strbuf now;
    odin3_strbuf_init(&now);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(res, &now));
    TEST_ASSERT_EQUAL_STRING(expected->data, now.data);
    odin3_strbuf_free(&now);
}

static void count_glue(const pb_result *res, uint32_t want_and, uint32_t want_not,
                       uint32_t want_mux, uint32_t want_dff) {
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want_and, hm_count(res->module, "$and"), "$and");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want_not, hm_count(res->module, "$not"), "$not");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want_mux, hm_count(res->module, "$mux"), "$mux");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want_dff, hm_count(res->module, "$dff"), "$dff");
}

/* --- simulation ---------------------------------------------------------------------------- */

static ramcase g_case;
static uint32_t g_addr_mask; /* the address bits the simulation drives; the others stay 0 */

static odin3_status build_g(pb_result *out) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    ram_text(&text, &g_case);
    odin3_status st = hm_build(text.data, out);
    odin3_strbuf_free(&text);
    if (st == ODIN3_OK) {
        TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                              odin3_design_set_top(out->design, odin3_module_id_of(out->module)));
    }
    return st;
}

typedef struct nets {
    odin3_net_id in[MAX_NETS];
    uint32_t n_in;
    odin3_net_id out[MAX_NETS];
    uint32_t n_out;
} nets;

/* The wires of one read/write port of the case. */
typedef struct wire_names {
    const char *data;
    const char *addr;
    const char *out;
    const char *we;
} wire_names;

static const wire_names k_sp_names[1] = {{"data", "addr", "out", "we"}};
static const wire_names k_dp_names[2] = {{"data1", "addr1", "out1", "we1"},
                                         {"data2", "addr2", "out2", "we2"}};

/* Bits of a wire: those of `mask` below `width`. */
typedef struct wire_bits {
    const char *name;
    uint32_t width;
    uint32_t mask;
} wire_bits;

static void add_wire_nets(const pb_result *res, wire_bits wire, odin3_net_id *list,
                          uint32_t *count) {
    for (uint32_t bit = 0; bit < wire.width; bit++) {
        if (((wire.mask >> bit) & 1U) != 0) {
            TEST_ASSERT_TRUE(*count < MAX_NETS);
            list[(*count)++] = wnet(res, wire.name, bit);
        }
    }
}

/* The driven and traced nets of the case: data, we and the masked address bits in, out out. */
static void collect_nets(const pb_result *res, nets *io) {
    const ramcase *rc = &g_case;
    const uint32_t all_data = ((uint32_t)1 << rc->data) - 1U;
    const char *const consts[] = {rc->we1, rc->we2};
    const wire_names *names = rc->dual ? k_dp_names : k_sp_names;
    io->n_in = 0;
    io->n_out = 0;
    for (uint32_t port = 0; port < (rc->dual ? 2U : 1U); port++) {
        add_wire_nets(res, (wire_bits){names[port].data, rc->data, all_data}, io->in, &io->n_in);
        add_wire_nets(res, (wire_bits){names[port].addr, rc->addr, g_addr_mask}, io->in, &io->n_in);
        add_wire_nets(res, (wire_bits){names[port].out, rc->data, all_data}, io->out, &io->n_out);
        if (consts[port] == NULL) {
            add_wire_nets(res, (wire_bits){names[port].we, 1, 1}, io->in, &io->n_in);
        }
    }
}

static void trace_of(pb_result *res, odin3_strbuf *out) {
    nets io;
    collect_nets(res, &io);
    rs_trace(res->module, &(rs_io){io.in, io.n_in, io.out, io.n_out},
             (rs_run){SIM_CYCLES, 12345, false}, out);
}

/* Builds the case twice, maps the second, and compares the traces; returns true when equal. */
static bool sim_equal(ramcase rc, uint32_t addr_mask) {
    g_case = rc;
    g_addr_mask = addr_mask;
    pb_result before;
    pb_result after;
    odin3_strbuf trace_before;
    odin3_strbuf trace_after;
    odin3_strbuf_init(&trace_before);
    odin3_strbuf_init(&trace_after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, build_g(&before));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, build_g(&after));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&after, NULL));
    trace_of(&before, &trace_before);
    trace_of(&after, &trace_after);
    uint32_t ones = 0;
    for (size_t i = 0; i < trace_before.len; i++) {
        ones += trace_before.data[i] == '1' ? 1U : 0U;
    }
    TEST_ASSERT_TRUE_MESSAGE(ones >= MIN_ONES, "the trace must read back written data");
    bool same = trace_before.len == trace_after.len &&
                memcmp(trace_before.data, trace_after.data, trace_before.len) == 0;
    odin3_strbuf_free(&trace_before);
    odin3_strbuf_free(&trace_after);
    pb_free(&before);
    pb_free(&after);
    return same;
}

/* --- R-3, R-4: width split and address padding --------------------------------------------- */

static void test_r3_width_split_sp(void) {
    pb_result res;
    build_ram((ramcase){false, ARCH_ADDR, 8, NULL, NULL}, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT32(8, hm_count(res.module, "single_port_ram"));
    TEST_ASSERT_EQUAL_UINT64(8, stats.sp_slices);
    count_glue(&res, 0, 0, 0, 0);
    for (uint32_t j = 0; j < 8; j++) {
        odin3_node_id node = hm_nth(res.module, "single_port_ram", j);
        expect_slice(&res, node, false);
        expect_net(wnet(&res, "data", j), pin_of(&res, node, SP_DATA, 0));
        expect_net(wnet(&res, "out", j), pin_of(&res, node, SP_OUT, 0));
        expect_net(wnet(&res, "we", 0), pin_of(&res, node, SP_WE, 0));
        expect_net(wnet(&res, "clk", 0), pin_of(&res, node, SP_CLK, 0));
        for (uint32_t i = 0; i < ARCH_ADDR; i++) {
            expect_net(wnet(&res, "addr", i), pin_of(&res, node, SP_ADDR, i));
        }
    }
    pb_free(&res);
}

static void test_r3_width_split_dp(void) {
    pb_result res;
    build_ram((ramcase){true, ARCH_ADDR, 3, NULL, NULL}, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT32(3, hm_count(res.module, "dual_port_ram"));
    TEST_ASSERT_EQUAL_UINT64(3, stats.dp_slices);
    count_glue(&res, 0, 0, 0, 0);
    for (uint32_t j = 0; j < 3; j++) {
        odin3_node_id node = hm_nth(res.module, "dual_port_ram", j);
        expect_slice(&res, node, true);
        expect_net(wnet(&res, "data1", j), pin_of(&res, node, DP_DATA1, 0));
        expect_net(wnet(&res, "data2", j), pin_of(&res, node, DP_DATA2, 0));
        expect_net(wnet(&res, "out1", j), pin_of(&res, node, DP_OUT1, 0));
        expect_net(wnet(&res, "out2", j), pin_of(&res, node, DP_OUT2, 0));
        expect_net(wnet(&res, "we1", 0), pin_of(&res, node, DP_WE1, 0));
        expect_net(wnet(&res, "we2", 0), pin_of(&res, node, DP_WE2, 0));
        expect_net(wnet(&res, "clk", 0), pin_of(&res, node, DP_CLK, 0));
        for (uint32_t i = 0; i < ARCH_ADDR; i++) {
            expect_net(wnet(&res, "addr1", i), pin_of(&res, node, DP_ADDR1, i));
            expect_net(wnet(&res, "addr2", i), pin_of(&res, node, DP_ADDR2, i));
        }
    }
    pb_free(&res);
}

static void expect_pad_from(const pb_result *res, odin3_node_id node, uint32_t port,
                            uint32_t first) {
    for (uint32_t i = first; i < ARCH_ADDR; i++) {
        odin3_net_id net = pin_of(res, node, port, i);
        TEST_ASSERT_TRUE(odin3_net_valid(net));
        TEST_ASSERT_EQUAL_INT(ODIN3_CONST_X, odin3_net_const_value(res->module, net));
    }
}

static void test_r4_addr_pad(void) {
    static const uint32_t k_widths[] = {10, 1, 14};
    for (size_t wi = 0; wi < sizeof k_widths / sizeof k_widths[0]; wi++) {
        pb_result res;
        build_ram((ramcase){false, k_widths[wi], 1, NULL, NULL}, &res);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
        TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "single_port_ram"));
        odin3_node_id node = hm_nth(res.module, "single_port_ram", 0);
        expect_slice(&res, node, false);
        for (uint32_t i = 0; i < k_widths[wi]; i++) {
            expect_net(wnet(&res, "addr", i), pin_of(&res, node, SP_ADDR, i));
        }
        expect_pad_from(&res, node, SP_ADDR, k_widths[wi]);
        pb_free(&res);
    }
    pb_result dp;
    build_ram((ramcase){true, 12, 2, NULL, NULL}, &dp);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&dp, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, hm_count(dp.module, "dual_port_ram"));
    odin3_node_id node = hm_nth(dp.module, "dual_port_ram", 1);
    expect_pad_from(&dp, node, DP_ADDR1, 12);
    expect_pad_from(&dp, node, DP_ADDR2, 12);
    pb_free(&dp);
}

static void test_r_arch_shape_untouched(void) {
    for (int dual = 0; dual < 2; dual++) {
        pb_result res;
        odin3_strbuf before;
        odin3_strbuf_init(&before);
        build_ram((ramcase){dual != 0, ARCH_ADDR, 1, NULL, NULL}, &res);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, &before));
        odin3_hardmap_stats stats;
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
        TEST_ASSERT_EQUAL_UINT64(0, stats.sp_slices + stats.dp_slices);
        assert_rtlil_equal(&res, &before);
        odin3_strbuf_free(&before);
        pb_free(&res);
    }
}

static void test_width_split_sim_sp(void) {
    TEST_ASSERT_TRUE(sim_equal((ramcase){false, ARCH_ADDR, 4, NULL, NULL}, 0x1FU));
    TEST_ASSERT_TRUE(sim_equal((ramcase){false, 10, 3, NULL, NULL}, 0x1FU));
}

static void test_width_split_sim_dp(void) {
    TEST_ASSERT_TRUE(sim_equal((ramcase){true, ARCH_ADDR, 3, NULL, NULL}, 0x0FU));
    TEST_ASSERT_TRUE(sim_equal((ramcase){true, 9, 2, NULL, NULL}, 0x0FU));
}

/* --- R-5, R-6 ------------------------------------------------------------------------------ */

static void test_r5_const_we_kept(void) {
    pb_result res;
    build_ram((ramcase){false, ARCH_ADDR, 2, "1'0", NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, hm_count(res.module, "single_port_ram"));
    count_glue(&res, 0, 0, 0, 0);
    for (uint32_t j = 0; j < 2; j++) {
        odin3_net_id we = pin_of(&res, hm_nth(res.module, "single_port_ram", j), SP_WE, 0);
        TEST_ASSERT_EQUAL_INT(ODIN3_CONST_0, odin3_net_const_value(res.module, we));
    }
    pb_free(&res);
    build_ram((ramcase){true, ARCH_ADDR, 2, "1'0", "1'1"}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, hm_count(res.module, "dual_port_ram"));
    odin3_node_id node = hm_nth(res.module, "dual_port_ram", 1);
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_0,
                          odin3_net_const_value(res.module, pin_of(&res, node, DP_WE1, 0)));
    TEST_ASSERT_EQUAL_INT(ODIN3_CONST_1,
                          odin3_net_const_value(res.module, pin_of(&res, node, DP_WE2, 0)));
    pb_free(&res);
}

static void test_r6_two_live_writes(void) {
    pb_result res;
    build_ram((ramcase){true, ARCH_ADDR, 3, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(3, hm_count(res.module, "dual_port_ram"));
    count_glue(&res, 0, 0, 0, 0); /* no priority logic */
    for (uint32_t j = 0; j < 3; j++) {
        odin3_node_id node = hm_nth(res.module, "dual_port_ram", j);
        expect_net(wnet(&res, "we1", 0), pin_of(&res, node, DP_WE1, 0));
        expect_net(wnet(&res, "we2", 0), pin_of(&res, node, DP_WE2, 0));
    }
    pb_free(&res);
}

/* --- R-1, R-2, R-7: depth split ------------------------------------------------------------ */

/* The select of the output mux driving net `out`: the dff behind it, checked against addr bit
 * `bit` of wire `addr` and clk. Returns the mux. */
static odin3_node_id check_select(const pb_result *res, odin3_net_id out, const char *addr,
                                  uint32_t bit) {
    odin3_node_id mux = must_find(res, "$mux", G_MUX_Y, out);
    odin3_node_id dff = must_find(res, "$dff", G_DFF_Q, pin_of(res, mux, G_MUX_S, 0));
    expect_net(wnet(res, addr, bit), pin_of(res, dff, G_DFF_D, 0));
    expect_net(wnet(res, "clk", 0), pin_of(res, dff, G_DFF_CLK, 0));
    return mux;
}

/* we of a leaf: $and(prev, addr bit) when `high`, $and(prev, ~addr bit) otherwise; returns prev. */
static odin3_net_id check_we_and(const pb_result *res, odin3_net_id we, const char *addr,
                                 uint32_t bit, bool high) {
    odin3_node_id gate = must_find(res, "$and", G_AND_Y, we);
    odin3_net_id other = pin_of(res, gate, G_AND_B, 0);
    if (high) {
        expect_net(wnet(res, addr, bit), other);
    } else {
        odin3_node_id inv = must_find(res, "$not", G_NOT_Y, other);
        expect_net(wnet(res, addr, bit), pin_of(res, inv, G_NOT_A, 0));
    }
    return pin_of(res, gate, G_AND_A, 0);
}

static void test_r2_depth_16(void) {
    pb_result res;
    build_ram((ramcase){false, 16, 1, NULL, NULL}, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT32(2, hm_count(res.module, "single_port_ram"));
    TEST_ASSERT_EQUAL_UINT64(2, stats.sp_slices);
    count_glue(&res, 2, 1, 1, 1);
    odin3_node_id half = hm_nth(res.module, "single_port_ram", 0);
    odin3_node_id upper = hm_nth(res.module, "single_port_ram", 1);
    for (uint32_t i = 0; i < ARCH_ADDR; i++) {
        expect_net(wnet(&res, "addr", i + 1), pin_of(&res, half, SP_ADDR, i));
        expect_net(wnet(&res, "addr", i + 1), pin_of(&res, upper, SP_ADDR, i));
    }
    expect_slice(&res, half, false);
    expect_slice(&res, upper, false);
    expect_net(wnet(&res, "we", 0),
               check_we_and(&res, pin_of(&res, half, SP_WE, 0), "addr", 0, false));
    expect_net(wnet(&res, "we", 0),
               check_we_and(&res, pin_of(&res, upper, SP_WE, 0), "addr", 0, true));
    odin3_node_id mux = check_select(&res, wnet(&res, "out", 0), "addr", 0);
    expect_net(pin_of(&res, half, SP_OUT, 0), pin_of(&res, mux, G_MUX_A, 0));
    expect_net(pin_of(&res, upper, SP_OUT, 0), pin_of(&res, mux, G_MUX_B, 0));
    pb_free(&res);
}

static void test_r2_depth_17(void) {
    pb_result res;
    build_ram((ramcase){false, 17, 2, NULL, NULL}, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT32(8, hm_count(res.module, "single_port_ram"));
    TEST_ASSERT_EQUAL_UINT64(8, stats.sp_slices);
    count_glue(&res, 6, 2, 6, 2); /* the selects are shared by the two data bits */
    for (uint32_t i = 0; i < 8; i++) {
        odin3_node_id node = hm_nth(res.module, "single_port_ram", i);
        expect_slice(&res, node, false);
        for (uint32_t k = 0; k < ARCH_ADDR; k++) {
            expect_net(wnet(&res, "addr", k + 2), pin_of(&res, node, SP_ADDR, k));
        }
    }
    /* data bit 1 is leaves 4 .. 7 (bit-major); leaf 2 = addr[1:0] = 0b10: s at level 1, h at 0 */
    odin3_node_id leaf = hm_nth(res.module, "single_port_ram", 4 + 2);
    odin3_net_id prev = check_we_and(&res, pin_of(&res, leaf, SP_WE, 0), "addr", 1, true);
    expect_net(wnet(&res, "we", 0), check_we_and(&res, prev, "addr", 0, false));
    odin3_node_id root = check_select(&res, wnet(&res, "out", 1), "addr", 0);
    odin3_node_id lower = must_find(&res, "$mux", G_MUX_Y, pin_of(&res, root, G_MUX_A, 0));
    odin3_node_id inner = must_find(&res, "$dff", G_DFF_Q, pin_of(&res, lower, G_MUX_S, 0));
    expect_net(wnet(&res, "addr", 1), pin_of(&res, inner, G_DFF_D, 0));
    pb_free(&res);
}

static void test_r1_order(void) {
    pb_result res;
    build_ram((ramcase){false, 16, 4, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(8, hm_count(res.module, "single_port_ram"));
    for (uint32_t i = 0; i < 8; i++) {
        expect_slice(&res, hm_nth(res.module, "single_port_ram", i), false);
    }
    count_glue(&res, 2, 1, 4, 1);
    pb_free(&res);
}

static void test_r7_dp_depth(void) {
    pb_result res;
    build_ram((ramcase){true, 16, 1, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_EQUAL_UINT32(2, hm_count(res.module, "dual_port_ram"));
    count_glue(&res, 4, 2, 2, 2); /* one select per read port */
    odin3_node_id half = hm_nth(res.module, "dual_port_ram", 0);
    odin3_node_id upper = hm_nth(res.module, "dual_port_ram", 1);
    odin3_node_id mux1 = check_select(&res, wnet(&res, "out1", 0), "addr1", 0);
    odin3_node_id mux2 = check_select(&res, wnet(&res, "out2", 0), "addr2", 0);
    expect_net(pin_of(&res, half, DP_OUT1, 0), pin_of(&res, mux1, G_MUX_A, 0));
    expect_net(pin_of(&res, upper, DP_OUT2, 0), pin_of(&res, mux2, G_MUX_B, 0));
    /* each port splits on its own address and enable; both halves carry both ports */
    expect_net(wnet(&res, "we1", 0),
               check_we_and(&res, pin_of(&res, upper, DP_WE1, 0), "addr1", 0, true));
    expect_net(wnet(&res, "we2", 0),
               check_we_and(&res, pin_of(&res, half, DP_WE2, 0), "addr2", 0, false));
    for (uint32_t i = 0; i < ARCH_ADDR; i++) {
        expect_net(wnet(&res, "addr1", i + 1), pin_of(&res, half, DP_ADDR1, i));
        expect_net(wnet(&res, "addr2", i + 1), pin_of(&res, upper, DP_ADDR2, i));
    }
    pb_free(&res);
}

/* addr[0] changes in about half the cycles: a live select would read the wrong half (D4) */
static void test_r7_select_registered_sim_sp(void) {
    TEST_ASSERT_TRUE(sim_equal((ramcase){false, 16, 2, NULL, NULL}, 0x800FU));
    TEST_ASSERT_TRUE(sim_equal((ramcase){false, 17, 2, NULL, NULL}, 0x1800FU));
}

static void test_r7_select_registered_sim_dp(void) {
    TEST_ASSERT_TRUE(sim_equal((ramcase){true, 16, 2, NULL, NULL}, 0x800FU));
    TEST_ASSERT_TRUE(sim_equal((ramcase){true, 18, 1, NULL, NULL}, 0x3000FU));
}

static void test_r7_select_constant(void) {
    TEST_ASSERT_TRUE(sim_equal((ramcase){false, 16, 2, NULL, NULL}, 0x800EU));
    TEST_ASSERT_TRUE(sim_equal((ramcase){true, 16, 2, NULL, NULL}, 0x800EU));
}

/* --- H-7, names, H-8, H-9, big ------------------------------------------------------------- */

/* The module's port nodes and the constants: not made by the mapping. */
static bool is_infrastructure(const pb_result *res, odin3_node_id node) {
    const char *name = odin3_celltype_get(res->design, odin3_node_type(res->module, node))->name;
    return strncmp(name, "$port_", 6) == 0 || strncmp(name, "$_CONST", 7) == 0;
}

/* A node the mapping made: DERIVED from the source's record, in operation *op (the first one
 * seen sets it). */
static void expect_derived(const pb_result *res, odin3_node_id node, odin3_prov_id source,
                           uint32_t *op) {
    const odin3_prov_record *rec = odin3_prov_get(res->design, odin3_node_prov(res->module, node));
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_INT(ODIN3_PROV_DERIVED, rec->kind);
    TEST_ASSERT_EQUAL_UINT32(1, rec->parents.count);
    TEST_ASSERT_EQUAL_UINT32(source.v, rec->parents.ids[0].v);
    TEST_ASSERT_TRUE(*op == 0 || *op == rec->op); /* one operation */
    *op = rec->op;
}

static void test_h7_prov_ram(void) {
    pb_result res;
    build_ram((ramcase){false, 16, 2, NULL, NULL}, &res);
    odin3_prov_id source = odin3_node_prov(res.module, hm_nth(res.module, "single_port_ram", 0));
    TEST_ASSERT_TRUE(source.v != 0);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    uint32_t seen = 0;
    uint32_t op = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(res.module); id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(res.module, node) && !is_infrastructure(&res, node)) {
            expect_derived(&res, node, source, &op);
            seen++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(4 + 2 + 1 + 1 + 2, seen); /* RAM, mux, dff, not, and */
    pb_free(&res);
}

static const char *attr_text(const pb_result *res, odin3_node_id node) {
    uint32_t key = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_intern(res->design, odin3_bytes_cstr("hardmap"), &key));
    const odin3_value *val =
        odin3_attr_get(res->module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, key);
    TEST_ASSERT_NOT_NULL(val);
    return odin3_strtab_get(odin3_design_strtab(res->design), val->str);
}

static void test_r_names(void) {
    pb_result res;
    build_ram((ramcase){false, 17, 2, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    odin3_node_id first = hm_nth(res.module, "single_port_ram", 0);
    TEST_ASSERT_EQUAL_STRING("u", node_name_of(&res, first));
    TEST_ASSERT_EQUAL_STRING("single_port_ram:b0.hh", attr_text(&res, first));
    odin3_node_id last = hm_nth(res.module, "single_port_ram", 7);
    TEST_ASSERT_EQUAL_STRING("u.b1.ss", node_name_of(&res, last));
    TEST_ASSERT_EQUAL_STRING("single_port_ram:b1.ss", attr_text(&res, last));
    odin3_node_id mid = hm_nth(res.module, "single_port_ram", 4 + 1);
    TEST_ASSERT_EQUAL_STRING("u.b1.sh", node_name_of(&res, mid));
    odin3_node_id mux = must_find(&res, "$mux", G_MUX_Y, wnet(&res, "out", 1));
    TEST_ASSERT_EQUAL_STRING("u.mux.b1", node_name_of(&res, mux));
    TEST_ASSERT_EQUAL_STRING("$mux:mux.b1", attr_text(&res, mux));
    odin3_node_id dff = must_find(&res, "$dff", G_DFF_D, wnet(&res, "addr", 1));
    TEST_ASSERT_EQUAL_STRING("u.sel.l1", node_name_of(&res, dff));
    pb_free(&res);
    build_ram((ramcase){true, 16, 1, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    odin3_node_id dmux = must_find(&res, "$mux", G_MUX_Y, wnet(&res, "out2", 0));
    TEST_ASSERT_EQUAL_STRING("u.mux2.b0", node_name_of(&res, dmux));
    TEST_ASSERT_EQUAL_STRING("u", node_name_of(&res, hm_nth(res.module, "dual_port_ram", 0)));
    pb_free(&res);
}

static void test_hardmap_twice(void) {
    pb_result res;
    build_ram((ramcase){true, 17, 3, NULL, NULL}, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT64(12, stats.dp_slices);
    odin3_strbuf once;
    odin3_strbuf_init(&once);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, &once));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT64(0, stats.dp_slices);
    assert_rtlil_equal(&res, &once);
    odin3_strbuf_free(&once);
    pb_free(&res);
}

static void test_h8_determinism(void) {
    pb_result one;
    pb_result two;
    odin3_strbuf text_one;
    odin3_strbuf text_two;
    odin3_strbuf_init(&text_one);
    odin3_strbuf_init(&text_two);
    build_ram((ramcase){true, 17, 2, NULL, NULL}, &one);
    build_ram((ramcase){true, 17, 2, NULL, NULL}, &two);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&one, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&two, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&one, &text_one));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&two, &text_two));
    TEST_ASSERT_EQUAL_STRING(text_one.data, text_two.data);
    odin3_strbuf_free(&text_one);
    odin3_strbuf_free(&text_two);
    pb_free(&one);
    pb_free(&two);
}

static void test_h10_info(void) {
    pb_result res;
    build_ram((ramcase){false, 16, 3, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_NOT_NULL(
        strstr(log_text, "partial_map: 0 multiply, 0 adder in 0 chains, 6 single_port_ram + 0 "
                         "dual_port_ram slices; 0 cells left soft"));
    pb_free(&res);
}

static void test_r_addr_too_wide(void) {
    pb_result res;
    odin3_strbuf before;
    odin3_strbuf_init(&before);
    build_ram((ramcase){false, 32, 1, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(&res, &before));
    TEST_ASSERT_EQUAL_INT(ODIN3_ERR_INVALID_ARG, odin3_hardmap_run(&res.ctx, res.design, NULL));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "partial_map: RAM instance 'u' has ADDR_WIDTH 32"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "single_port_ram"));
    assert_rtlil_equal(&res, &before);
    odin3_strbuf_free(&before);
    pb_free(&res);
}

static void test_r_big(void) {
    pb_result res;
    build_ram((ramcase){false, 16, 36, NULL, NULL}, &res);
    odin3_hardmap_stats stats;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT32(72, hm_count(res.module, "single_port_ram"));
    TEST_ASSERT_EQUAL_UINT64(72, stats.sp_slices);
    count_glue(&res, 2, 1, 36, 1);
    pb_free(&res);
    build_ram((ramcase){true, 16, 36, NULL, NULL}, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT32(72, hm_count(res.module, "dual_port_ram"));
    count_glue(&res, 4, 2, 72, 2);
    pb_free(&res);
}

typedef struct snapshot {
    odin3_strbuf text;
    uint32_t nodes;
    uint32_t nets;
} snapshot;

static void snap_take(const pb_result *res, snapshot *snap) {
    odin3_strbuf_init(&snap->text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(res, &snap->text));
    snap->nodes = 0;
    snap->nets = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(res->module); id++) {
        snap->nodes += odin3_node_live(res->module, (odin3_node_id){id}) ? 1U : 0U;
    }
    for (uint32_t id = 1; id < odin3_module_net_end(res->module); id++) {
        snap->nets += odin3_net_live(res->module, (odin3_net_id){id}) ? 1U : 0U;
    }
}

static void snap_expect(const pb_result *res, const snapshot *want) {
    snapshot now;
    snap_take(res, &now);
    TEST_ASSERT_EQUAL_STRING(want->text.data, now.text.data);
    TEST_ASSERT_EQUAL_UINT32(want->nodes, now.nodes);
    TEST_ASSERT_EQUAL_UINT32(want->nets, now.nets);
    odin3_strbuf_free(&now.text);
}

typedef struct oom_case {
    ramcase rc;
    uint32_t slices; /* what the finished run leaves */
} oom_case;

static bool oom_try(const oom_case *oc, long fail_at) {
    pb_result res;
    snapshot before;
    const char *type = oc->rc.dual ? "dual_port_ram" : "single_port_ram";
    build_ram(oc->rc, &res);
    snap_take(&res, &before);
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st = odin3_hardmap_run(&res.ctx, res.design, NULL);
    odin3_util_set_alloc_fail_after(-1);
    bool injected = st != ODIN3_OK;
    if (injected) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, type)); /* the source is intact */
        snap_expect(&res, &before); /* and nothing else changed: no fresh net, no glue, no pad */
    } else {
        TEST_ASSERT_EQUAL_UINT32(oc->slices, hm_count(res.module, type));
    }
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res.design, opts));
    odin3_strbuf_free(&before.text);
    pb_free(&res);
    return injected;
}

/* Fails the n-th allocation for n = 0, 1 ... until OOM_QUIET runs in a row succeed. */
static long oom_sweep(const oom_case *oc) {
    long failed = 0;
    long quiet = 0;
    for (long fail_at = 0; fail_at < OOM_LIMIT && quiet < OOM_QUIET; fail_at++) {
        bool injected = oom_try(oc, fail_at);
        failed += injected ? 1 : 0;
        quiet = injected ? 0 : quiet + 1;
    }
    TEST_ASSERT_TRUE(quiet == OOM_QUIET);
    return failed;
}

static void test_h9_oom(void) {
    /* width split only; pad (the constant node is made inside the transaction); depth split with
     * glue; dual port with depth and width */
    const oom_case cases[] = {{{false, ARCH_ADDR, 3, NULL, NULL}, 3},
                              {{false, 10, 1, NULL, NULL}, 1},
                              {{false, 16, 2, NULL, NULL}, 4},
                              {{true, 16, 2, NULL, NULL}, 4},
                              {{true, 11, 2, "1'0", NULL}, 2}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        TEST_ASSERT_TRUE(oom_sweep(&cases[i]) > 1);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_r3_width_split_sp);
    RUN_TEST(test_r3_width_split_dp);
    RUN_TEST(test_r4_addr_pad);
    RUN_TEST(test_r_arch_shape_untouched);
    RUN_TEST(test_width_split_sim_sp);
    RUN_TEST(test_width_split_sim_dp);
    RUN_TEST(test_r5_const_we_kept);
    RUN_TEST(test_r6_two_live_writes);
    RUN_TEST(test_r2_depth_16);
    RUN_TEST(test_r2_depth_17);
    RUN_TEST(test_r1_order);
    RUN_TEST(test_r7_dp_depth);
    RUN_TEST(test_r7_select_registered_sim_sp);
    RUN_TEST(test_r7_select_registered_sim_dp);
    RUN_TEST(test_r7_select_constant);
    RUN_TEST(test_h7_prov_ram);
    RUN_TEST(test_r_names);
    RUN_TEST(test_hardmap_twice);
    RUN_TEST(test_h8_determinism);
    RUN_TEST(test_h10_info);
    RUN_TEST(test_r_addr_too_wide);
    RUN_TEST(test_r_big);
    RUN_TEST(test_h9_oom);
    return UNITY_END();
}
