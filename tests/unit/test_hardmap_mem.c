/*
 * test_hardmap_mem.c — hardmap, PR 3C-3: inferred `$mem` (3C spec R-8..R-12, H-7, H-9; plan Task 6
 * Step 1). The `$mem` is built as 2D EL-39 emits it (asynchronous reads, raw addresses, RD_EN 1,
 * one write enable replicated over the data bits), with the flip-flops R-8 merges around it, from
 * flat RTLIL with lib/vtr.o3lib read (pb_build_lib); hm_run checks the IR (FULL) before and after.
 * The value-preserving cases use hardmap_ramsim.c on the IR before and after the pass.
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
#include "passes/hardmap/hardmap.h"
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
    MAX_NETS = 160,
    MAXP = 4,
    NAME_SIZE = 24,
    SIM_CYCLES = 2000,
    MIN_ONES = 100,
    ARCH_ADDR = 15,
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
    DP_OUT1 = 8
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

/* --- the `$mem` builder -------------------------------------------------------------------- */

/* What surrounds a read port. */
typedef enum rmode {
    R_ASYNC,    /* as EL-39 emits it: asynchronous, RD_DATA is the output */
    R_DFF,      /* RD_DATA feeds a `$dff` on the clock */
    R_DFFE_ONE, /* ... a `$dffe` whose enable is the constant 1 */
    R_DFFE_VAR, /* ... a `$dffe` whose enable is the input wire ren<pn> */
    R_AFF,      /* RD_ADDR is the Q of a `$dff` whose D is the input wire ra<pn> */
    R_SYNC      /* a synchronous read port (RD_CLK_ENABLE 1) */
} rmode;

typedef struct rdp {
    rmode mode;
    const char *clk; /* NULL: clk */
    bool neg;        /* falling edge */
    bool transparent;
    bool en_var; /* R_SYNC: RD_EN is the input wire ren<pn> */
    uint32_t
        addr_wr; /* 0: the port's own address wire ra<pn>; slot: write port slot-1's wa<slot-1> */
} rdp;

typedef struct wrp {
    const char *clk; /* NULL: clk */
    bool neg;
} wrp;

typedef struct memcase {
    uint32_t abits;
    uint32_t width;
    uint32_t size; /* 0: 2^abits */
    int32_t offset;
    uint32_t n_rd;
    uint32_t n_wr;
    rdp rd[MAXP];
    wrp wr[MAXP];
    bool init;     /* a defined INIT */
    bool split_en; /* write port 0's enable bits are two nets */
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

static void wire_decl(odin3_strbuf *out, const char *dir, uint32_t width, uint32_t *port,
                      const char *name) {
    if (width > 0) {
        put(out, "  wire width %" PRIu32 " %s %" PRIu32 " \\%s\n", width, dir, (*port)++, name);
    }
}

static const char *clk_of(const char *clk) {
    return clk != NULL ? clk : "clk";
}

static bool uses_ff(rmode mode) {
    return mode == R_DFF || mode == R_DFFE_ONE || mode == R_DFFE_VAR;
}

static uint32_t mem_size(const memcase *mc) {
    return mc->size != 0 ? mc->size : (uint32_t)1 << mc->abits;
}

/* "parameter \NAME n'bits": bit i of mask is port i. */
typedef struct bitspec {
    uint32_t n;
    uint32_t mask;
} bitspec;

static void put_bits(odin3_strbuf *out, const char *name, bitspec spec) {
    put(out, "    parameter \\%s %" PRIu32 "'", name, spec.n);
    for (uint32_t i = spec.n; i > 0; i--) {
        put(out, "%c", ((spec.mask >> (i - 1)) & 1U) != 0 ? '1' : '0');
    }
    put(out, "\n");
}

static void decl_wires(odin3_strbuf *out, const memcase *mc) {
    uint32_t port = 1;
    char name[32];
    wire_decl(out, "input", 1, &port, "clk");
    wire_decl(out, "input", 1, &port, "clk2");
    for (uint32_t pn = 0; pn < mc->n_rd; pn++) {
        (void)snprintf(name, sizeof name, "ra%" PRIu32, pn);
        wire_decl(out, "input", mc->abits, &port, name);
        (void)snprintf(name, sizeof name, "ren%" PRIu32, pn);
        wire_decl(out, "input", 1, &port, name);
        (void)snprintf(name, sizeof name, "q%" PRIu32, pn);
        wire_decl(out, "output", mc->width, &port, name);
    }
    for (uint32_t pn = 0; pn < mc->n_wr; pn++) {
        (void)snprintf(name, sizeof name, "wa%" PRIu32, pn);
        wire_decl(out, "input", mc->abits, &port, name);
        (void)snprintf(name, sizeof name, "wd%" PRIu32, pn);
        wire_decl(out, "input", mc->width, &port, name);
        (void)snprintf(name, sizeof name, "we%" PRIu32, pn);
        wire_decl(out, "input", 1, &port, name);
    }
    if (mc->split_en) {
        wire_decl(out, "input", 1, &port, "we0b");
    }
    for (uint32_t pn = 0; pn < mc->n_rd; pn++) {
        if (uses_ff(mc->rd[pn].mode)) {
            put(out, "  wire width %" PRIu32 " \\rd%" PRIu32 "\n", mc->width, pn);
        }
        if (mc->rd[pn].mode == R_AFF) {
            put(out, "  wire width %" PRIu32 " \\rq%" PRIu32 "\n", mc->abits, pn);
        }
    }
}

static bool is_dffe(rmode mode) {
    return mode == R_DFFE_ONE || mode == R_DFFE_VAR;
}

/* The flip-flop around read port pn (R_DFF*, R_AFF). */
static void ff_cell(odin3_strbuf *out, const memcase *mc, uint32_t pn) {
    const rdp *rd_port = &mc->rd[pn];
    bool en = is_dffe(rd_port->mode);
    bool addr = rd_port->mode == R_AFF;
    put(out,
        "  cell %s \\ff%" PRIu32 "\n    parameter \\WIDTH %" PRIu32
        "\n    parameter \\CLK_POLARITY %d\n",
        en ? "$dffe" : "$dff", pn, addr ? mc->abits : mc->width, rd_port->neg ? 0 : 1);
    if (en) {
        put(out, "    parameter \\EN_POLARITY 1\n    connect \\EN ");
        if (rd_port->mode == R_DFFE_ONE) {
            put(out, "1'1\n");
        } else {
            put(out, "\\ren%" PRIu32 "\n", pn);
        }
    }
    put(out, "    connect \\CLK \\%s\n", clk_of(rd_port->clk));
    if (addr) {
        put(out, "    connect \\D \\ra%" PRIu32 "\n    connect \\Q \\rq%" PRIu32 "\n", pn, pn);
    } else {
        put(out, "    connect \\D \\rd%" PRIu32 "\n    connect \\Q \\q%" PRIu32 "\n", pn, pn);
    }
    put(out, "  end\n");
}

static void ff_cells(odin3_strbuf *out, const memcase *mc) {
    for (uint32_t pn = 0; pn < mc->n_rd; pn++) {
        if (uses_ff(mc->rd[pn].mode) || mc->rd[pn].mode == R_AFF) {
            ff_cell(out, mc, pn);
        }
    }
}

static void mem_params(odin3_strbuf *out, const memcase *mc) {
    uint32_t sync = 0;
    uint32_t pos = 0;
    uint32_t transp = 0;
    uint32_t wpos = 0;
    for (uint32_t pn = 0; pn < mc->n_rd; pn++) {
        sync |= mc->rd[pn].mode == R_SYNC ? 1U << pn : 0U;
        pos |= !mc->rd[pn].neg ? 1U << pn : 0U;
        transp |= mc->rd[pn].transparent ? 1U << pn : 0U;
    }
    for (uint32_t pn = 0; pn < mc->n_wr; pn++) {
        wpos |= !mc->wr[pn].neg ? 1U << pn : 0U;
    }
    put(out,
        "  cell $mem \\u\n    parameter \\MEMID \"\\\\mem\"\n    parameter \\SIZE %" PRIu32
        "\n    parameter \\OFFSET %" PRId32 "\n    parameter \\ABITS %" PRIu32
        "\n    parameter \\WIDTH %" PRIu32 "\n    parameter \\RD_PORTS %" PRIu32
        "\n    parameter \\WR_PORTS %" PRIu32 "\n",
        mem_size(mc), mc->offset, mc->abits, mc->width, mc->n_rd, mc->n_wr);
    put_bits(out, "RD_CLK_ENABLE", (bitspec){mc->n_rd, sync});
    put_bits(out, "RD_CLK_POLARITY", (bitspec){mc->n_rd, pos});
    put_bits(out, "RD_TRANSPARENT", (bitspec){mc->n_rd, transp});
    put_bits(out, "WR_CLK_ENABLE", (bitspec){mc->n_wr, (1U << mc->n_wr) - 1U});
    put_bits(out, "WR_CLK_POLARITY", (bitspec){mc->n_wr, wpos});
    if (mc->init) {
        uint32_t total = mem_size(mc) * mc->width;
        put(out, "    parameter \\INIT %" PRIu32 "'1", total);
        for (uint32_t i = 1; i < total; i++) {
            put(out, "x");
        }
        put(out, "\n");
    } else {
        put(out, "    parameter \\INIT 0'\n");
    }
}

/* A concatenation of one item per port, the last port first. */
typedef struct cat_item {
    const char *prefix; /* wire name before the port number */
    uint32_t count;
} cat_item;

static void connect_cat(odin3_strbuf *out, const char *pin, cat_item item) {
    put(out, "    connect \\%s {", pin);
    for (uint32_t pn = item.count; pn > 0; pn--) {
        put(out, " \\%s%" PRIu32, item.prefix, pn - 1);
    }
    put(out, " }\n");
}

static void connect_rd_clk(odin3_strbuf *out, const memcase *mc) {
    put(out, "    connect \\RD_CLK {");
    for (uint32_t pn = mc->n_rd; pn > 0; pn--) {
        const rdp *rd_port = &mc->rd[pn - 1];
        if (rd_port->mode == R_SYNC) {
            put(out, " \\%s", clk_of(rd_port->clk));
        } else {
            put(out, " 1'x");
        }
    }
    put(out, " }\n");
}

static void connect_rd_en(odin3_strbuf *out, const memcase *mc) {
    put(out, "    connect \\RD_EN {");
    for (uint32_t pn = mc->n_rd; pn > 0; pn--) {
        const rdp *rd_port = &mc->rd[pn - 1];
        if (rd_port->mode == R_SYNC && rd_port->en_var) {
            put(out, " \\ren%" PRIu32, pn - 1);
        } else {
            put(out, " 1'1");
        }
    }
    put(out, " }\n");
}

static void connect_rd_addr(odin3_strbuf *out, const memcase *mc) {
    put(out, "    connect \\RD_ADDR {");
    for (uint32_t pn = mc->n_rd; mc->abits > 0 && pn > 0; pn--) {
        const rdp *rd_port = &mc->rd[pn - 1];
        if (rd_port->mode == R_AFF) {
            put(out, " \\rq%" PRIu32, pn - 1);
        } else if (rd_port->addr_wr > 0) {
            put(out, " \\wa%" PRIu32, rd_port->addr_wr - 1);
        } else {
            put(out, " \\ra%" PRIu32, pn - 1);
        }
    }
    put(out, " }\n");
}

static void connect_rd_data(odin3_strbuf *out, const memcase *mc) {
    put(out, "    connect \\RD_DATA {");
    for (uint32_t pn = mc->n_rd; pn > 0; pn--) {
        put(out, uses_ff(mc->rd[pn - 1].mode) ? " \\rd%" PRIu32 : " \\q%" PRIu32, pn - 1);
    }
    put(out, " }\n");
}

/* The write enable of each port, replicated over the data bits (EL-39); split_en makes bit 0 of
 * port 0 a net of its own. */
static void connect_wr_en(odin3_strbuf *out, const memcase *mc) {
    put(out, "    connect \\WR_EN {");
    for (uint32_t pn = mc->n_wr; pn > 0; pn--) {
        for (uint32_t bit = mc->width; bit > 0; bit--) {
            if (mc->split_en && pn == 1 && bit == 1) {
                put(out, " \\we0b");
            } else {
                put(out, " \\we%" PRIu32, pn - 1);
            }
        }
    }
    put(out, " }\n");
}

static void mem_connects(odin3_strbuf *out, const memcase *mc) {
    connect_rd_clk(out, mc);
    connect_rd_en(out, mc);
    connect_rd_addr(out, mc);
    connect_rd_data(out, mc);
    put(out, "    connect \\WR_CLK {");
    for (uint32_t pn = mc->n_wr; pn > 0; pn--) {
        put(out, " \\%s", clk_of(mc->wr[pn - 1].clk));
    }
    put(out, " }\n");
    connect_wr_en(out, mc);
    if (mc->abits > 0) {
        connect_cat(out, "WR_ADDR", (cat_item){"wa", mc->n_wr});
    } else {
        put(out, "    connect \\WR_ADDR { }\n");
    }
    connect_cat(out, "WR_DATA", (cat_item){"wd", mc->n_wr});
    put(out, "  end\n");
}

static void mem_text(odin3_strbuf *out, const memcase *mc) {
    put(out, "module \\m\n");
    decl_wires(out, mc);
    ff_cells(out, mc);
    mem_params(out, mc);
    mem_connects(out, mc);
    put(out, "end\n");
}

static void build_mem(memcase mc, pb_result *res) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    mem_text(&text, &mc);
    hm_build_ok(text.data, res);
    odin3_strbuf_free(&text);
}

/* --- inspection ---------------------------------------------------------------------------- */

static odin3_net_id wnet(const pb_result *res, const char *name, uint32_t bit) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(res->design, odin3_bytes_cstr(name), &str));
    odin3_wire_id wire = odin3_module_find_wire(res->module, str);
    TEST_ASSERT_TRUE_MESSAGE(odin3_wire_valid(wire), name);
    return odin3_wire_net(res->module, wire, bit);
}

typedef struct pin_ref {
    uint32_t port;
    uint32_t bit;
} pin_ref;

static odin3_net_id pin_at(const pb_result *res, odin3_node_id node, pin_ref pin) {
    odin3_pinslice pins = odin3_node_port(res->module, node, pin.port);
    TEST_ASSERT_TRUE(pin.bit < pins.count);
    return odin3_pin_net(res->module, (odin3_pin_id){pins.first.v + pin.bit});
}

static void expect_wire(const pb_result *res, odin3_node_id node, pin_ref pin, const char *wire,
                        uint32_t wbit) {
    odin3_net_id want = wnet(res, wire, wbit);
    TEST_ASSERT_TRUE(odin3_net_valid(want));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.v, pin_at(res, node, pin).v, wire);
}

static void expect_const(const pb_result *res, odin3_node_id node, pin_ref pin, odin3_const want) {
    odin3_net_id net = pin_at(res, node, pin);
    TEST_ASSERT_TRUE(odin3_net_valid(net));
    TEST_ASSERT_EQUAL_INT((int)want, (int)odin3_net_const_value(res->module, net));
}

static void expect_open(const pb_result *res, odin3_node_id node, pin_ref pin) {
    TEST_ASSERT_FALSE(odin3_net_valid(pin_at(res, node, pin)));
}

/* What the pins of one RAM port should be: a wire name, or NULL for the default (pad / gnd / open).
 */
typedef struct port_exp {
    const char *addr; /* abits bits, then the pad */
    const char *data; /* bit j of the wire, else the pad */
    const char *we;   /* bit 0 of the wire, else gnd */
    const char *out;  /* bit j of the wire, else open */
} port_exp;

typedef struct ram_exp {
    bool dual;
    uint32_t abits;
    port_exp port[2];
} ram_exp;

static void expect_port(const pb_result *res, odin3_node_id node, const ram_exp *ex,
                        uint32_t slot) {
    static const uint32_t addr_pin[2][2] = {{SP_ADDR, 0}, {DP_ADDR1, DP_ADDR2}};
    static const uint32_t we_pin[2][2] = {{SP_WE, 0}, {DP_WE1, DP_WE2}};
    const port_exp *pe = &ex->port[slot];
    uint32_t kind = ex->dual ? 1U : 0U;
    for (uint32_t i = 0; i < ARCH_ADDR; i++) {
        pin_ref pin = {addr_pin[kind][slot], i};
        if (i < ex->abits && pe->addr != NULL) {
            expect_wire(res, node, pin, pe->addr, i);
        } else {
            expect_const(res, node, pin, ODIN3_CONST_X);
        }
    }
    if (pe->we != NULL) {
        expect_wire(res, node, (pin_ref){we_pin[kind][slot], 0}, pe->we, 0);
    } else {
        expect_const(res, node, (pin_ref){we_pin[kind][slot], 0}, ODIN3_CONST_0);
    }
}

static void expect_ram(const pb_result *res, odin3_node_id node, const ram_exp *ex, uint32_t bit) {
    static const uint32_t data_pin[2][2] = {{SP_DATA, 0}, {DP_DATA1, DP_DATA2}};
    static const uint32_t out_pin[2][2] = {{SP_OUT, 0}, {DP_OUT1, DP_OUT2}};
    uint32_t kind = ex->dual ? 1U : 0U;
    TEST_ASSERT_EQUAL_INT64(ARCH_ADDR, odin3_node_param(res->module, node, 0)->i);
    TEST_ASSERT_EQUAL_INT64(1, odin3_node_param(res->module, node, 1)->i);
    for (uint32_t slot = 0; slot < (ex->dual ? 2U : 1U); slot++) {
        expect_port(res, node, ex, slot);
        pin_ref data = {data_pin[kind][slot], 0};
        pin_ref out = {out_pin[kind][slot], 0};
        if (ex->port[slot].data != NULL) {
            expect_wire(res, node, data, ex->port[slot].data, bit);
        } else {
            expect_const(res, node, data, ODIN3_CONST_X);
        }
        if (ex->port[slot].out != NULL) {
            expect_wire(res, node, out, ex->port[slot].out, bit);
        } else {
            expect_open(res, node, out);
        }
    }
    expect_wire(res, node, (pin_ref){ex->dual ? DP_CLK : SP_CLK, 0}, "clk", 0);
}

/* Slices [first, first + width) of the kind's nodes, in node-ID order, all as ex says. */
static void expect_rams(const pb_result *res, const ram_exp *ex, uint32_t first, uint32_t width) {
    const char *type = ex->dual ? "dual_port_ram" : "single_port_ram";
    for (uint32_t j = 0; j < width; j++) {
        expect_ram(res, hm_nth(res->module, type, first + j), ex, j);
    }
}

/* Runs hardmap; true when `$mem` is gone. */
static bool mapped(pb_result *res, odin3_hardmap_stats *stats) {
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(res, stats));
    return hm_count(res->module, "$mem") == 0;
}

/* The design as RTLIL, to compare before and after. */
static void rtlil_of(const pb_result *res, odin3_strbuf *out) {
    odin3_strbuf_init(out);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_rtlil(res, out));
}

/* hardmap leaves the design as it was, logs `warning` once, counts one soft cell. */
static void expect_soft(pb_result *res, const char *warning) {
    odin3_strbuf before;
    odin3_strbuf after;
    odin3_hardmap_stats stats;
    rtlil_of(res, &before);
    TEST_ASSERT_FALSE(mapped(res, &stats));
    rtlil_of(res, &after);
    TEST_ASSERT_EQUAL_STRING(before.data, after.data);
    TEST_ASSERT_EQUAL_UINT64(1, stats.soft);
    TEST_ASSERT_EQUAL_UINT64(0, stats.sp_slices + stats.dp_slices);
    const char *at = strstr(log_text, warning);
    TEST_ASSERT_NOT_NULL_MESSAGE(at, warning);
    TEST_ASSERT_NULL_MESSAGE(strstr(at + 1, warning), "the warning is logged once");
    odin3_strbuf_free(&before);
    odin3_strbuf_free(&after);
}

#define W_ASYNC "hardmap: memory 'mem' has an asynchronous read port; left to lower as flip-flops"
#define W_EDGE "hardmap: memory 'mem' has a port on the falling clock edge; left to lower"
#define W_EN "hardmap: memory 'mem' has a read enable that is not constant 1; left to lower"

typedef struct shape {
    uint32_t abits;
    uint32_t width;
    uint32_t n_rd;
    uint32_t n_wr;
} shape;

static memcase base(shape sh) {
    memcase mc;
    memset(&mc, 0, sizeof mc);
    mc.abits = sh.abits;
    mc.width = sh.width;
    mc.n_rd = sh.n_rd;
    mc.n_wr = sh.n_wr;
    return mc;
}

/* Wire names of port pn, "ra0", "q1", ... in storage that outlives the call (a test's lifetime). */
static const char *nm(const char *prefix, uint32_t pn) {
    static char store[16][NAME_SIZE];
    static uint32_t next;
    char *buf = store[next++ % 16U];
    int len = snprintf(buf, NAME_SIZE, "%s%" PRIu32, prefix, pn);
    TEST_ASSERT_TRUE(len > 0 && len < NAME_SIZE);
    return buf;
}

/* --- R-8: the flip-flop merge -------------------------------------------------------------- */

/* common/mem.v's shape: a registered read of a word array, a write on the same clock. */
static void test_r8_data_merge(void) {
    memcase mc = base((shape){4, 3, 1, 1});
    mc.rd[0].mode = R_DFF;
    pb_result res;
    odin3_hardmap_stats stats;
    build_mem(mc, &res);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$dff"));
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$dff"));
    TEST_ASSERT_EQUAL_UINT32(3, hm_count(res.module, "dual_port_ram"));
    TEST_ASSERT_EQUAL_UINT64(3, stats.dp_slices);
    TEST_ASSERT_EQUAL_UINT64(0, stats.soft);
    const ram_exp ex = {true, 4, {{"wa0", "wd0", "we0", NULL}, {"ra0", NULL, NULL, "q0"}}};
    expect_rams(&res, &ex, 0, 3);
    pb_free(&res);
}

static void test_r8_data_merge_dffe(void) {
    memcase mc = base((shape){4, 3, 1, 1});
    pb_result res;
    odin3_hardmap_stats stats;
    mc.rd[0].mode = R_DFFE_ONE;
    build_mem(mc, &res);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$dffe"));
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$dffe"));
    TEST_ASSERT_EQUAL_UINT32(3, hm_count(res.module, "dual_port_ram"));
    pb_free(&res);
    /* a variable enable becomes RD_EN, which has no pin: soft, the original flip-flop stays */
    mc.rd[0].mode = R_DFFE_VAR;
    build_mem(mc, &res);
    expect_soft(&res, W_EN);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$dffe"));
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mem"));
    pb_free(&res);
}

static void test_r8_addr_merge_no_write(void) {
    memcase mc = base((shape){4, 2, 1, 0});
    mc.rd[0].mode = R_AFF;
    pb_result res;
    odin3_hardmap_stats stats;
    build_mem(mc, &res);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$dff"));
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(res.module, "$dff"));
    TEST_ASSERT_EQUAL_UINT32(2, hm_count(res.module, "single_port_ram"));
    const ram_exp ex = {false, 4, {{"ra0", NULL, NULL, "q0"}}};
    expect_rams(&res, &ex, 0, 2);
    pb_free(&res);
}

/* D12: with a write port on the read clock the address merge would change the read of a
 * same-address write (the arch's RAMs are read-first): the port stays asynchronous. */
static void test_r8_addr_merge_with_write(void) {
    memcase mc = base((shape){4, 2, 1, 1});
    mc.rd[0].mode = R_AFF;
    pb_result res;
    build_mem(mc, &res);
    expect_soft(&res, W_ASYNC);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$dff"));
    pb_free(&res);
}

static void test_r8_mixed_clocks_no_merge(void) {
    memcase mc = base((shape){4, 2, 1, 1});
    mc.rd[0].mode = R_DFF;
    mc.rd[0].clk = "clk2";
    pb_result res;
    build_mem(mc, &res);
    expect_soft(&res, W_ASYNC);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$dff"));
    pb_free(&res);
}

/* --- R-9 ----------------------------------------------------------------------------------- */

static void test_r9_async_stays_soft(void) {
    memcase mc = base((shape){4, 2, 1, 1});
    pb_result res;
    build_mem(mc, &res);
    expect_soft(&res, W_ASYNC);
    TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mem"));
    TEST_ASSERT_NOT_NULL(
        strstr(log_text, "0 single_port_ram + 0 dual_port_ram slices; 1 cells left soft"));
    pb_free(&res);
}

static void test_r9_multi_clock(void) {
    memcase mc = base((shape){4, 2, 1, 1});
    mc.rd[0].mode = R_SYNC;
    mc.rd[0].clk = "clk2";
    pb_result res;
    build_mem(mc, &res);
    expect_soft(&res, W_ASYNC);
    pb_free(&res);
}

static void test_r9_negedge_soft(void) {
    memcase mc = base((shape){4, 2, 1, 1});
    pb_result res;
    mc.rd[0].mode = R_DFF;
    mc.rd[0].neg = true;
    mc.wr[0].neg = true;
    build_mem(mc, &res);
    expect_soft(&res, W_EDGE);
    pb_free(&res);
    log_text[0] = '\0';
    mc = base((shape){4, 2, 1, 0});
    mc.rd[0].mode = R_SYNC;
    mc.rd[0].neg = true;
    build_mem(mc, &res);
    expect_soft(&res, W_EDGE);
    pb_free(&res);
}

/* --- R-10: the port patterns --------------------------------------------------------------- */

static uint32_t rams_of(const pb_result *res, const char *type, uint32_t want) {
    TEST_ASSERT_EQUAL_UINT32(want, hm_count(res->module, type));
    return want;
}

static void test_p_1_0(void) {
    memcase mc = base((shape){4, 2, 1, 0});
    mc.rd[0].mode = R_SYNC;
    pb_result res;
    odin3_hardmap_stats stats;
    build_mem(mc, &res);
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    rams_of(&res, "single_port_ram", 2);
    rams_of(&res, "dual_port_ram", 0);
    const ram_exp ex = {false, 4, {{"ra0", NULL, NULL, "q0"}}};
    expect_rams(&res, &ex, 0, 2);
    pb_free(&res);
}

static void test_p_2_0(void) {
    memcase mc = base((shape){4, 2, 2, 0});
    mc.rd[0].mode = R_SYNC;
    mc.rd[1].mode = R_SYNC;
    pb_result res;
    odin3_hardmap_stats stats;
    build_mem(mc, &res);
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    rams_of(&res, "dual_port_ram", 2);
    rams_of(&res, "single_port_ram", 0);
    const ram_exp ex = {true, 4, {{"ra0", NULL, NULL, "q0"}, {"ra1", NULL, NULL, "q1"}}};
    expect_rams(&res, &ex, 0, 2);
    pb_free(&res);
}

static void test_p_3_0(void) {
    memcase mc = base((shape){4, 2, 3, 0});
    for (uint32_t pn = 0; pn < 3; pn++) {
        mc.rd[pn].mode = R_SYNC;
    }
    pb_result res;
    odin3_hardmap_stats stats;
    build_mem(mc, &res);
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    rams_of(&res, "single_port_ram", 6);
    for (uint32_t pn = 0; pn < 3; pn++) {
        const ram_exp ex = {false, 4, {{nm("ra", pn), NULL, NULL, nm("q", pn)}}};
        expect_rams(&res, &ex, pn * 2, 2);
    }
    pb_free(&res);
}

static void test_p_1_1_same_addr(void) {
    memcase mc = base((shape){4, 2, 1, 1});
    mc.rd[0].mode = R_SYNC;
    mc.rd[0].addr_wr = 1;
    pb_result res;
    odin3_hardmap_stats stats;
    build_mem(mc, &res);
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    rams_of(&res, "single_port_ram", 2);
    rams_of(&res, "dual_port_ram", 0);
    const ram_exp ex = {false, 4, {{"wa0", "wd0", "we0", "q0"}}};
    expect_rams(&res, &ex, 0, 2);
    pb_free(&res);
}

static void test_p_1_1_diff_addr(void) {
    memcase mc = base((shape){4, 2, 1, 1});
    mc.rd[0].mode = R_SYNC;
    pb_result res;
    odin3_hardmap_stats stats;
    build_mem(mc, &res);
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    rams_of(&res, "dual_port_ram", 2);
    rams_of(&res, "single_port_ram", 0);
    const ram_exp ex = {true, 4, {{"wa0", "wd0", "we0", NULL}, {"ra0", NULL, NULL, "q0"}}};
    expect_rams(&res, &ex, 0, 2);
    pb_free(&res);
}

static void test_p_1_2(void) {
    memcase mc = base((shape){4, 2, 1, 2});
    mc.rd[0].mode = R_SYNC;
    mc.rd[0].addr_wr = 2;
    pb_result res;
    odin3_hardmap_stats stats;
    build_mem(mc, &res);
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    rams_of(&res, "dual_port_ram", 2);
    const ram_exp ex = {true, 4, {{"wa0", "wd0", "we0", NULL}, {"wa1", "wd1", "we1", "q0"}}};
    expect_rams(&res, &ex, 0, 2);
    pb_free(&res);
    mc.rd[0].addr_wr = 1; /* the read goes on the other port */
    build_mem(mc, &res);
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    const ram_exp ex1 = {true, 4, {{"wa0", "wd0", "we0", "q0"}, {"wa1", "wd1", "we1", NULL}}};
    expect_rams(&res, &ex1, 0, 2);
    pb_free(&res);
}

static void test_p_2_1(void) {
    memcase mc = base((shape){4, 2, 2, 1});
    mc.rd[0].mode = R_SYNC;
    mc.rd[1].mode = R_SYNC;
    mc.rd[1].addr_wr = 1; /* the read with the identical address goes with the write on port 1 */
    pb_result res;
    odin3_hardmap_stats stats;
    build_mem(mc, &res);
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    rams_of(&res, "dual_port_ram", 2);
    const ram_exp ex = {true, 4, {{"wa0", "wd0", "we0", "q1"}, {"ra0", NULL, NULL, "q0"}}};
    expect_rams(&res, &ex, 0, 2);
    pb_free(&res);
    mc.rd[1].addr_wr = 0; /* neither identical: one DP replica per read */
    build_mem(mc, &res);
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    rams_of(&res, "dual_port_ram", 4);
    for (uint32_t pn = 0; pn < 2; pn++) {
        const ram_exp rep = {
            true, 4, {{"wa0", "wd0", "we0", NULL}, {nm("ra", pn), NULL, NULL, nm("q", pn)}}};
        expect_rams(&res, &rep, pn * 2, 2);
    }
    pb_free(&res);
}

static void test_p_2_2(void) {
    memcase mc = base((shape){4, 2, 2, 2});
    mc.rd[0].mode = R_SYNC;
    mc.rd[0].addr_wr = 1;
    mc.rd[1].mode = R_SYNC;
    mc.rd[1].addr_wr = 2;
    pb_result res;
    odin3_hardmap_stats stats;
    build_mem(mc, &res);
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    rams_of(&res, "dual_port_ram", 2);
    const ram_exp ex = {true, 4, {{"wa0", "wd0", "we0", "q0"}, {"wa1", "wd1", "we1", "q1"}}};
    expect_rams(&res, &ex, 0, 2);
    pb_free(&res);
    mc.rd[0].addr_wr = 2; /* crossed */
    mc.rd[1].addr_wr = 1;
    build_mem(mc, &res);
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    const ram_exp cross = {true, 4, {{"wa0", "wd0", "we0", "q1"}, {"wa1", "wd1", "we1", "q0"}}};
    expect_rams(&res, &cross, 0, 2);
    pb_free(&res);
    mc.rd[0].addr_wr = 0; /* no identical address: soft */
    mc.rd[1].addr_wr = 0;
    build_mem(mc, &res);
    expect_soft(&res, "hardmap: memory 'mem' (2 read, 2 write ports) has no fixed-subset mapping; "
                      "left to lower");
    pb_free(&res);
}

static void test_p_3_1(void) {
    memcase mc = base((shape){4, 2, 3, 1});
    for (uint32_t pn = 0; pn < 3; pn++) {
        mc.rd[pn].mode = R_SYNC;
    }
    pb_result res;
    odin3_hardmap_stats stats;
    build_mem(mc, &res);
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    rams_of(&res, "dual_port_ram", 6);
    for (uint32_t pn = 0; pn < 3; pn++) {
        const ram_exp rep = {
            true, 4, {{"wa0", "wd0", "we0", NULL}, {nm("ra", pn), NULL, NULL, nm("q", pn)}}};
        expect_rams(&res, &rep, pn * 2, 2);
    }
    pb_free(&res);
}

static void test_r11_soft_patterns(void) {
    memcase mc = base((shape){4, 2, 3, 2}); /* p_3_2 */
    pb_result res;
    for (uint32_t pn = 0; pn < 3; pn++) {
        mc.rd[pn].mode = R_SYNC;
    }
    build_mem(mc, &res);
    expect_soft(&res, "hardmap: memory 'mem' (3 read, 2 write ports) has no fixed-subset mapping; "
                      "left to lower");
    pb_free(&res);
    log_text[0] = '\0';
    mc = base((shape){4, 2, 1, 3}); /* p_1_3 */
    mc.rd[0].mode = R_SYNC;
    build_mem(mc, &res);
    expect_soft(&res, "hardmap: memory 'mem' (1 read, 3 write ports) has no fixed-subset mapping; "
                      "left to lower");
    pb_free(&res);
}

static void test_p_rd_en_nonconst(void) {
    memcase mc = base((shape){4, 2, 1, 1});
    mc.rd[0].mode = R_SYNC;
    mc.rd[0].en_var = true;
    pb_result res;
    build_mem(mc, &res);
    expect_soft(&res, W_EN);
    pb_free(&res);
}

/* The arch's RAMs are read-first: a transparent read beside a write has no pin to map to. */
static void test_p_transparent_and_split_enable(void) {
    memcase mc = base((shape){4, 2, 1, 1});
    pb_result res;
    mc.rd[0].mode = R_SYNC;
    mc.rd[0].transparent = true;
    build_mem(mc, &res);
    expect_soft(&res, "hardmap: memory 'mem' (1 read, 1 write ports) has no fixed-subset mapping; "
                      "left to lower");
    pb_free(&res);
    log_text[0] = '\0';
    mc.rd[0].transparent = false;
    mc.split_en = true; /* a write enable of two nets has no single `we` */
    build_mem(mc, &res);
    expect_soft(&res, "hardmap: memory 'mem' (1 read, 1 write ports) has no fixed-subset mapping; "
                      "left to lower");
    pb_free(&res);
}

/* --- R-12, the open points ----------------------------------------------------------------- */

#define W_INIT "hardmap: memory 'mem': initial contents dropped (the arch's RAMs have no init)"

/* A synchronous ROM of the shape, with or without a defined INIT, mapped; the log is kept. */
static void map_rom(bool init) {
    memcase mc = base((shape){4, 2, 1, 0});
    pb_result res;
    odin3_hardmap_stats stats;
    mc.rd[0].mode = R_SYNC;
    mc.init = init;
    log_text[0] = '\0';
    build_mem(mc, &res);
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    pb_free(&res);
}

static void test_r12_init_dropped(void) {
    map_rom(true);
    TEST_ASSERT_NOT_NULL(strstr(log_text, W_INIT));
    map_rom(false); /* all x: nothing to drop */
    TEST_ASSERT_NULL(strstr(log_text, "initial contents"));
}

static void test_e_offset_nonzero_soft(void) {
    memcase mc = base((shape){4, 2, 1, 0});
    mc.rd[0].mode = R_SYNC;
    mc.offset = 3;
    pb_result res;
    build_mem(mc, &res);
    expect_soft(&res, "hardmap: memory 'mem' has OFFSET 3; left to lower");
    pb_free(&res);
}

static void test_e_size_below_pow2_maps(void) {
    memcase mc = base((shape){4, 1, 1, 0});
    mc.size = 10;
    mc.rd[0].mode = R_SYNC;
    pb_result res;
    odin3_hardmap_stats stats;
    build_mem(mc, &res);
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    TEST_ASSERT_EQUAL_UINT64(1, stats.sp_slices);
    pb_free(&res);
}

static void test_e_abits0_maps(void) {
    memcase mc = base((shape){0, 1, 1, 1});
    mc.size = 1;
    mc.rd[0].mode = R_SYNC;
    mc.rd[0].addr_wr = 1;
    pb_result res;
    odin3_hardmap_stats stats;
    build_mem(mc, &res);
    TEST_ASSERT_TRUE(mapped(&res, &stats));
    rams_of(&res, "single_port_ram", 1);
    const ram_exp ex = {false, 0, {{NULL, "wd0", "we0", "q0"}}};
    expect_rams(&res, &ex, 0, 1); /* all 15 address pins on the pad */
    pb_free(&res);
}

static void test_e_abits_wide_soft(void) {
    memcase mc = base((shape){40, 1, 1, 0});
    mc.size = 4;
    mc.rd[0].mode = R_SYNC;
    pb_result res;
    build_mem(mc, &res);
    expect_soft(&res, "hardmap: memory 'mem' (1 read, 0 write ports) has no fixed-subset mapping; "
                      "left to lower");
    pb_free(&res);
}

/* --- simulation: the `$mem` before and the RAMs after ------------------------------------- */

static memcase g_mc;
static uint32_t g_amask; /* the address bits the simulation drives; the others stay 0 */

static odin3_status build_g(pb_result *out) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    mem_text(&text, &g_mc);
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

/* Bits of a wire: those of `mask` below `width`. */
typedef struct wire_bits {
    const char *name;
    uint32_t width;
    uint32_t mask;
} wire_bits;

static void add_nets(const pb_result *res, wire_bits wire, odin3_net_id *list, uint32_t *count) {
    for (uint32_t bit = 0; bit < wire.width; bit++) {
        if (((wire.mask >> bit) & 1U) != 0) {
            TEST_ASSERT_TRUE(*count < MAX_NETS);
            list[(*count)++] = wnet(res, wire.name, bit);
        }
    }
}

static void collect_nets(const pb_result *res, nets *io) {
    const memcase *mc = &g_mc;
    const uint32_t all_data = ((uint32_t)1 << mc->width) - 1U;
    io->n_in = 0;
    io->n_out = 0;
    for (uint32_t pn = 0; pn < mc->n_rd; pn++) {
        add_nets(res, (wire_bits){nm("ra", pn), mc->abits, g_amask}, io->in, &io->n_in);
        add_nets(res, (wire_bits){nm("q", pn), mc->width, all_data}, io->out, &io->n_out);
    }
    for (uint32_t pn = 0; pn < mc->n_wr; pn++) {
        add_nets(res, (wire_bits){nm("wa", pn), mc->abits, g_amask}, io->in, &io->n_in);
        add_nets(res, (wire_bits){nm("wd", pn), mc->width, all_data}, io->in, &io->n_in);
        add_nets(res, (wire_bits){nm("we", pn), 1, 1}, io->in, &io->n_in);
    }
}

static void trace_of(pb_result *res, odin3_strbuf *out) {
    nets io;
    collect_nets(res, &io);
    rs_trace(res->module, &(rs_io){io.in, io.n_in, io.out, io.n_out}, (rs_run){SIM_CYCLES, 777},
             out);
}

/* Builds the case twice, maps the second, and compares the traces; true when equal. */
static bool sim_equal(memcase mc, uint32_t addr_mask) {
    g_mc = mc;
    g_amask = addr_mask;
    pb_result before;
    pb_result after;
    odin3_strbuf trace_before;
    odin3_strbuf trace_after;
    odin3_strbuf_init(&trace_before);
    odin3_strbuf_init(&trace_after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, build_g(&before));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, build_g(&after));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&after, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, hm_count(after.module, "$mem"));
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

enum { LOW_BITS = 0xFU, DEEP_MASK = 0x800FU };

static void test_mem_sim_data_merge(void) {
    memcase mc = base((shape){4, 3, 1, 1}); /* common/mem.v: data merge, read-first, DP */
    mc.rd[0].mode = R_DFF;
    TEST_ASSERT_TRUE(sim_equal(mc, LOW_BITS));
    mc.rd[0].mode = R_DFFE_ONE;
    TEST_ASSERT_TRUE(sim_equal(mc, LOW_BITS));
}

static void test_mem_sim_same_address(void) {
    memcase mc =
        base((shape){4, 3, 1, 1}); /* SP: a read of the word being written returns the old one */
    mc.rd[0].mode = R_SYNC;
    mc.rd[0].addr_wr = 1;
    TEST_ASSERT_TRUE(sim_equal(mc, LOW_BITS));
    mc.rd[0].mode = R_DFF; /* the same through the data merge */
    TEST_ASSERT_TRUE(sim_equal(mc, LOW_BITS));
}

static void test_mem_sim_2r1w(void) {
    memcase mc = base((shape){4, 2, 2, 1});
    mc.rd[0].mode = R_DFF;
    mc.rd[1].mode = R_DFF;
    TEST_ASSERT_TRUE(sim_equal(mc, LOW_BITS)); /* neither address identical: two replicas */
    mc.rd[1].mode = R_SYNC;
    mc.rd[1].addr_wr = 1; /* one identical: one DP */
    TEST_ASSERT_TRUE(sim_equal(mc, LOW_BITS));
}

static void test_mem_sim_two_writes(void) {
    memcase mc = base((shape){4, 2, 1, 2});
    mc.rd[0].mode = R_SYNC;
    mc.rd[0].addr_wr = 2;
    TEST_ASSERT_TRUE(sim_equal(mc, LOW_BITS)); /* the later write port wins a collision */
    mc = base((shape){4, 2, 2, 2});
    mc.rd[0].mode = R_SYNC;
    mc.rd[0].addr_wr = 1;
    mc.rd[1].mode = R_SYNC;
    mc.rd[1].addr_wr = 2;
    TEST_ASSERT_TRUE(sim_equal(mc, LOW_BITS));
}

/* ABITS 16 splits the depth: the registered select (R-7) is in play. */
static void test_mem_sim_deep(void) {
    memcase mc = base((shape){16, 2, 1, 1});
    mc.rd[0].mode = R_DFF;
    TEST_ASSERT_TRUE(sim_equal(mc, DEEP_MASK));
    mc.rd[0].mode = R_SYNC;
    mc.rd[0].addr_wr = 1;
    TEST_ASSERT_TRUE(sim_equal(mc, DEEP_MASK));
}

/* --- H-7, names, H-8, H-9 ------------------------------------------------------------------ */

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
    TEST_ASSERT_TRUE(*op == 0 || *op == rec->op);
    *op = rec->op;
}

static void test_h7_prov_mem(void) {
    memcase mc = base((shape){16, 2, 1, 1});
    mc.rd[0].mode = R_DFF;
    pb_result res;
    build_mem(mc, &res);
    odin3_prov_id source = odin3_node_prov(res.module, hm_nth(res.module, "$mem", 0));
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
    TEST_ASSERT_TRUE(seen > 4); /* 4 RAMs and the select glue */
    TEST_ASSERT_EQUAL_UINT32(4, hm_count(res.module, "dual_port_ram"));
    pb_free(&res);
}

static const char *node_name_of(const pb_result *res, odin3_node_id node) {
    return odin3_strtab_get(odin3_design_strtab(res->design), odin3_node_name(res->module, node));
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

static void test_h7_names(void) {
    memcase mc = base((shape){4, 3, 1, 1});
    mc.rd[0].mode = R_DFF;
    pb_result res;
    build_mem(mc, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    odin3_node_id first = hm_nth(res.module, "dual_port_ram", 0);
    TEST_ASSERT_EQUAL_STRING("u", node_name_of(&res, first));
    TEST_ASSERT_EQUAL_STRING("dual_port_ram:b0", attr_text(&res, first));
    odin3_node_id last = hm_nth(res.module, "dual_port_ram", 2);
    TEST_ASSERT_EQUAL_STRING("u.b2", node_name_of(&res, last));
    pb_free(&res);
}

static void test_hardmap_twice(void) {
    memcase mc = base((shape){4, 2, 1, 1});
    mc.rd[0].mode = R_DFF;
    pb_result res;
    odin3_hardmap_stats stats;
    odin3_strbuf once;
    odin3_strbuf now;
    build_mem(mc, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT64(2, stats.dp_slices);
    rtlil_of(&res, &once);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, &stats));
    TEST_ASSERT_EQUAL_UINT64(0, stats.dp_slices);
    rtlil_of(&res, &now);
    TEST_ASSERT_EQUAL_STRING(once.data, now.data);
    odin3_strbuf_free(&once);
    odin3_strbuf_free(&now);
    pb_free(&res);
}

static void test_h8_determinism(void) {
    memcase mc = base((shape){16, 2, 2, 1});
    mc.rd[0].mode = R_DFF;
    mc.rd[1].mode = R_DFF;
    pb_result one;
    pb_result two;
    odin3_strbuf text_one;
    odin3_strbuf text_two;
    build_mem(mc, &one);
    build_mem(mc, &two);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&one, NULL));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&two, NULL));
    rtlil_of(&one, &text_one);
    rtlil_of(&two, &text_two);
    TEST_ASSERT_EQUAL_STRING(text_one.data, text_two.data);
    odin3_strbuf_free(&text_one);
    odin3_strbuf_free(&text_two);
    pb_free(&one);
    pb_free(&two);
}

static void test_h10_info(void) {
    memcase mc = base((shape){4, 2, 1, 0});
    mc.rd[0].mode = R_SYNC;
    pb_result res;
    build_mem(mc, &res);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, hm_run(&res, NULL));
    TEST_ASSERT_NOT_NULL(strstr(log_text, "hardmap: 0 multiply, 0 adder in 0 chains, 2 "
                                          "single_port_ram + 0 dual_port_ram slices; 0 cells "
                                          "left soft"));
    pb_free(&res);
}

typedef struct snapshot {
    odin3_strbuf text;
    uint32_t nodes;
    uint32_t nets;
} snapshot;

static void snap_take(const pb_result *res, snapshot *snap) {
    rtlil_of(res, &snap->text);
    snap->nodes = 0;
    snap->nets = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(res->module); id++) {
        snap->nodes += odin3_node_live(res->module, (odin3_node_id){id}) ? 1U : 0U;
    }
    for (uint32_t id = 1; id < odin3_module_net_end(res->module); id++) {
        snap->nets += odin3_net_live(res->module, (odin3_net_id){id}) ? 1U : 0U;
    }
}

typedef struct oom_case {
    memcase mc;
    bool flip_flops; /* R-8 deletes flip-flops, which go before the `$mem` (H-9) */
    bool soft;       /* the run leaves the `$mem` */
} oom_case;

/* Fails the fail_at-th allocation of the run; true when the run reported it. On a failure the
 * `$mem` is live (H-9) and the IR checks; unless a flip-flop was already gone (the run then said
 * "discard it") nothing else changed either. */
static bool oom_try(const oom_case *oc, long fail_at) {
    pb_result res;
    snapshot before;
    snapshot now;
    log_text[0] = '\0';
    build_mem(oc->mc, &res);
    snap_take(&res, &before);
    odin3_util_set_alloc_fail_after(fail_at);
    odin3_status st = odin3_hardmap_run(&res.ctx, res.design, NULL);
    odin3_util_set_alloc_fail_after(-1);
    bool injected = st != ODIN3_OK;
    if (injected) {
        TEST_ASSERT_EQUAL_INT(ODIN3_ERR_NO_MEMORY, st);
        TEST_ASSERT_EQUAL_UINT32(1, hm_count(res.module, "$mem"));
        if (strstr(log_text, "discard it") == NULL) {
            snap_take(&res, &now);
            TEST_ASSERT_EQUAL_STRING(before.text.data, now.text.data);
            TEST_ASSERT_EQUAL_UINT32(before.nodes, now.nodes);
            TEST_ASSERT_EQUAL_UINT32(before.nets, now.nets);
            odin3_strbuf_free(&now.text);
        } else {
            TEST_ASSERT_TRUE(oc->flip_flops);
        }
    } else {
        TEST_ASSERT_EQUAL_UINT32(oc->soft ? 1U : 0U, hm_count(res.module, "$mem"));
    }
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(res.design, opts));
    odin3_strbuf_free(&before.text);
    pb_free(&res);
    return injected;
}

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
    oom_case cases[5];
    memset(cases, 0, sizeof cases);
    cases[0].mc = base((shape){4, 2, 1, 1}); /* data merge, DP, the pad */
    cases[0].mc.rd[0].mode = R_DFF;
    cases[0].flip_flops = true;
    cases[1].mc = base((shape){4, 1, 1, 0}); /* address merge */
    cases[1].mc.rd[0].mode = R_AFF;
    cases[1].flip_flops = true;
    cases[2].mc = base((shape){4, 2, 3, 0}); /* replicas */
    for (uint32_t pn = 0; pn < 3; pn++) {
        cases[2].mc.rd[pn].mode = R_SYNC;
    }
    cases[3].mc = base((shape){16, 2, 1, 1}); /* depth split glue */
    cases[3].mc.rd[0].mode = R_DFF;
    cases[3].flip_flops = true;
    cases[4].mc = base((shape){4, 2, 1, 1}); /* soft: nothing to allocate in the design */
    cases[4].soft = true;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        long failed = oom_sweep(&cases[i]);
        TEST_ASSERT_TRUE(i == 4 || failed > 1);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_r8_data_merge);
    RUN_TEST(test_r8_data_merge_dffe);
    RUN_TEST(test_r8_addr_merge_no_write);
    RUN_TEST(test_r8_addr_merge_with_write);
    RUN_TEST(test_r8_mixed_clocks_no_merge);
    RUN_TEST(test_r9_async_stays_soft);
    RUN_TEST(test_r9_multi_clock);
    RUN_TEST(test_r9_negedge_soft);
    RUN_TEST(test_p_1_0);
    RUN_TEST(test_p_2_0);
    RUN_TEST(test_p_3_0);
    RUN_TEST(test_p_1_1_same_addr);
    RUN_TEST(test_p_1_1_diff_addr);
    RUN_TEST(test_p_1_2);
    RUN_TEST(test_p_2_1);
    RUN_TEST(test_p_2_2);
    RUN_TEST(test_p_3_1);
    RUN_TEST(test_r11_soft_patterns);
    RUN_TEST(test_p_rd_en_nonconst);
    RUN_TEST(test_p_transparent_and_split_enable);
    RUN_TEST(test_r12_init_dropped);
    RUN_TEST(test_e_offset_nonzero_soft);
    RUN_TEST(test_e_size_below_pow2_maps);
    RUN_TEST(test_e_abits0_maps);
    RUN_TEST(test_e_abits_wide_soft);
    RUN_TEST(test_mem_sim_data_merge);
    RUN_TEST(test_mem_sim_same_address);
    RUN_TEST(test_mem_sim_2r1w);
    RUN_TEST(test_mem_sim_two_writes);
    RUN_TEST(test_mem_sim_deep);
    RUN_TEST(test_h7_prov_mem);
    RUN_TEST(test_h7_names);
    RUN_TEST(test_hardmap_twice);
    RUN_TEST(test_h8_determinism);
    RUN_TEST(test_h10_info);
    RUN_TEST(test_h9_oom);
    return UNITY_END();
}
