/* pm_mem_fixture.c — see pm_mem_fixture.h. */
#include "pm_mem_fixture.h"

#include "frontends/arch/arch.h"
#include "hardmap_ramsim.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/module.h"
#include "unity.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    LINE = 1024,
    PATH = 1024,
    MAX_FILE = 262144,
    MAX_NETS = 256,
    SIM_CYCLES = 3000,
    MIN_ONES = 150
};

static void put(odin3_strbuf *out, const char *fmt, ...) ODIN3_PRINTF(2, 3);

static void put(odin3_strbuf *out, const char *fmt, ...) {
    char buf[LINE];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    TEST_ASSERT_TRUE(len > 0 && (size_t)len < sizeof buf);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_append(out, odin3_bytes_cstr(buf)));
}

static void wire(odin3_strbuf *out, const char *dir, uint32_t width, uint32_t *port,
                 const char *name) {
    put(out, "  wire width %" PRIu32 " %s %" PRIu32 " \\%s\n", width, dir, (*port)++, name);
}

static void instance_text(odin3_strbuf *out, pmf_case cs) {
    uint32_t port = 1;
    put(out, "module \\m\n");
    wire(out, "input", 1, &port, "clk");
    if (cs.dual) {
        const char *const in[] = {"data1", "data2", "addr1", "addr2", "we1", "we2"};
        for (uint32_t i = 0; i < 6; i++) {
            uint32_t width = i < 4 ? cs.addr : 1;
            width = i < 2 ? cs.data : width;
            wire(out, "input", width, &port, in[i]);
        }
        wire(out, "output", cs.data, &port, "out1");
        wire(out, "output", cs.data, &port, "out2");
        put(out,
            "  cell \\dual_port_ram \\u\n    parameter \\ADDR_WIDTH %" PRIu32
            "\n    parameter \\DATA_WIDTH %" PRIu32
            "\n    connect \\clk \\clk\n    connect \\data1 \\data1\n    connect \\data2 \\data2\n"
            "    connect \\addr1 \\addr1\n    connect \\addr2 \\addr2\n    connect \\we1 \\we1\n"
            "    connect \\we2 \\we2\n    connect \\out1 \\out1\n    connect \\out2 \\out2\n  "
            "end\n",
            cs.addr, cs.data);
    } else {
        wire(out, "input", cs.data, &port, "data");
        wire(out, "input", cs.addr, &port, "addr");
        wire(out, "input", 1, &port, "we");
        wire(out, "output", cs.data, &port, "out");
        put(out,
            "  cell \\single_port_ram \\u\n    parameter \\ADDR_WIDTH %" PRIu32
            "\n    parameter \\DATA_WIDTH %" PRIu32
            "\n    connect \\clk \\clk\n    connect \\data \\data\n    connect \\addr \\addr\n"
            "    connect \\we \\we\n    connect \\out \\out\n  end\n",
            cs.addr, cs.data);
    }
    put(out, "end\n");
}

static void mem_text(odin3_strbuf *out, pmf_case cs) {
    uint32_t port = 1;
    put(out, "module \\m\n");
    wire(out, "input", 1, &port, "clk");
    wire(out, "input", cs.addr, &port, "addr");
    if (cs.dual) {
        wire(out, "input", cs.addr, &port, "raddr");
    }
    wire(out, "input", cs.data, &port, "data");
    wire(out, "input", 1, &port, "we");
    if (cs.en) {
        wire(out, "input", 1, &port, "ren");
    }
    wire(out, "output", cs.data, &port, "out");
    put(out,
        "  cell $mem \\u\n    parameter \\MEMID \"\\\\mem\"\n    parameter \\SIZE %" PRIu32
        "\n    parameter \\OFFSET 0\n    parameter \\ABITS %" PRIu32
        "\n    parameter \\WIDTH %" PRIu32
        "\n    parameter \\RD_PORTS 1\n    parameter \\WR_PORTS 1\n"
        "    parameter \\RD_CLK_ENABLE 1'1\n    parameter \\RD_CLK_POLARITY 1'1\n"
        "    parameter \\RD_TRANSPARENT 1'0\n    parameter \\WR_CLK_ENABLE 1'1\n"
        "    parameter \\WR_CLK_POLARITY 1'1\n    parameter \\INIT 0'\n"
        "    connect \\RD_CLK \\clk\n    connect \\RD_EN %s\n    connect \\RD_ADDR \\%s\n"
        "    connect \\RD_DATA \\out\n    connect \\WR_CLK \\clk\n    connect \\WR_EN {",
        (uint32_t)1 << cs.addr, cs.addr, cs.data, cs.en ? "\\ren" : "1'1",
        cs.dual ? "raddr" : "addr");
    for (uint32_t i = 0; i < cs.data; i++) {
        put(out, " \\we");
    }
    put(out, " }\n    connect \\WR_ADDR \\addr\n    connect \\WR_DATA \\data\n  end\nend\n");
}

static char *slurp(const char *path, size_t *len) {
    FILE *fp = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(fp, path);
    char *text = calloc(MAX_FILE, 1);
    TEST_ASSERT_NOT_NULL(text);
    *len = fread(text, 1, MAX_FILE - 1, fp);
    (void)fclose(fp);
    return text;
}

/* The record text of the XML at path, in a buffer of MAX_FILE bytes the caller frees. */
typedef struct arch_text {
    char *text;
    size_t len;
} arch_text;

/* Reads the XML into arch (parse and generated library); asserts it works. */
static void load_arch(odin3_arch *arch, const char *name, const arch_text *xml) {
    const odin3_arch_src src = {name, {xml->text, xml->len}};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_arch_parse(arch, &src));
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_arch_make_library(arch));
}

/* A fresh directory under TMPDIR in dir, and the library file name in it in path. */
static void lib_paths(char *dir, char *path) {
    const char *base = getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp";
    TEST_ASSERT_TRUE(snprintf(dir, PATH, "%s/pmf.XXXXXX", base) < PATH);
    TEST_ASSERT_NOT_NULL(mkdtemp(dir));
    TEST_ASSERT_TRUE(snprintf(path, PATH, "%s/arch.o3lib", dir) < PATH);
}

typedef struct text_file {
    const char *path;
    const char *text;
} text_file;

static void write_text(text_file file) {
    const char *path = file.path;
    const char *text = file.text;
    FILE *fp = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_size_t(strlen(text), fwrite(text, 1, strlen(text), fp));
    (void)fclose(fp);
}

/* The library of the XML read with the RTLIL (in a temporary directory that is removed), then the
 * record attached to the design: its types are already there. */
typedef struct arch_build {
    const char *rtlil;
    const char *xml_path;
} arch_build;

static void build_with_arch(arch_build req, pb_result *out) {
    const char *rtlil = req.rtlil;
    const char *xml_path = req.xml_path;
    arch_text xml = {NULL, 0};
    xml.text = slurp(xml_path, &xml.len);
    char dir[PATH];
    char lib[PATH];
    lib_paths(dir, lib);
    odin3_arch *tmp = odin3_arch_create();
    TEST_ASSERT_NOT_NULL(tmp);
    load_arch(tmp, xml_path, &xml);
    write_text((text_file){lib, odin3_arch_library_text(tmp)});
    odin3_arch_destroy(tmp);
    odin3_status st = pb_build_lib((pb_source){rtlil, lib}, out);
    (void)remove(lib);
    (void)rmdir(dir);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, st);
    odin3_arch *arch = NULL;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_get_arch(out->design, &arch));
    load_arch(arch, xml_path, &xml);
    free(xml.text);
}

void pmf_build(pmf_case cs, const char *arch_xml, pb_result *out) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    if (cs.kind == PMF_INSTANCE) {
        instance_text(&text, cs);
    } else {
        mem_text(&text, cs);
    }
    if (arch_xml != NULL) {
        build_with_arch((arch_build){text.data, arch_xml}, out);
    } else {
        char lib[PATH];
        TEST_ASSERT_TRUE(snprintf(lib, sizeof lib, "%s/vtr.o3lib", ODIN3_LIB_DIR) <
                         (int)sizeof lib);
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, pb_build_lib((pb_source){text.data, lib}, out));
    }
    odin3_strbuf_free(&text);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_design_set_top(out->design, odin3_module_id_of(out->module)));
}

static void check_full(odin3_design *design) {
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_check_design(design, opts));
}

odin3_status pmf_run(pb_result *res, const pm_options *opts, odin3_hardmap_stats *stats) {
    check_full(res->design);
    odin3_status st = odin3_partial_map_run_opts(&res->ctx, res->design, opts, stats);
    check_full(res->design);
    return st;
}

/* --- simulation ---------------------------------------------------------------------------- */

static odin3_net_id wnet(const pb_result *res, const char *name, uint32_t bit) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(res->design, odin3_bytes_cstr(name), &str));
    odin3_wire_id wid = odin3_module_find_wire(res->module, str);
    TEST_ASSERT_TRUE_MESSAGE(odin3_wire_valid(wid), name);
    return odin3_wire_net(res->module, wid, bit);
}

typedef struct nets {
    odin3_net_id in[MAX_NETS];
    uint32_t n_in;
    odin3_net_id out[MAX_NETS];
    uint32_t n_out;
} nets;

typedef struct wire_spec {
    const char *name;
    uint32_t width;
    uint32_t mask;
} wire_spec;

static void add_wire(const pb_result *res, wire_spec ws, odin3_net_id *list, uint32_t *count) {
    for (uint32_t bit = 0; bit < ws.width; bit++) {
        if (((ws.mask >> bit) & 1U) != 0) {
            TEST_ASSERT_TRUE(*count < MAX_NETS);
            list[(*count)++] = wnet(res, ws.name, bit);
        }
    }
}

static void collect(const pb_result *res, pmf_case cs, uint32_t addr_mask, nets *io) {
    const uint32_t all = cs.data >= 32 ? 0xFFFFFFFFU : (((uint32_t)1 << cs.data) - 1U);
    io->n_in = 0;
    io->n_out = 0;
    if (cs.kind == PMF_INSTANCE && cs.dual) {
        for (uint32_t port = 1; port <= 2; port++) {
            char name[16];
            (void)snprintf(name, sizeof name, "data%" PRIu32, port);
            add_wire(res, (wire_spec){name, cs.data, all}, io->in, &io->n_in);
            (void)snprintf(name, sizeof name, "addr%" PRIu32, port);
            add_wire(res, (wire_spec){name, cs.addr, addr_mask}, io->in, &io->n_in);
            (void)snprintf(name, sizeof name, "we%" PRIu32, port);
            add_wire(res, (wire_spec){name, 1, 1}, io->in, &io->n_in);
            (void)snprintf(name, sizeof name, "out%" PRIu32, port);
            add_wire(res, (wire_spec){name, cs.data, all}, io->out, &io->n_out);
        }
        return;
    }
    add_wire(res, (wire_spec){"data", cs.data, all}, io->in, &io->n_in);
    add_wire(res, (wire_spec){"addr", cs.addr, addr_mask}, io->in, &io->n_in);
    if (cs.kind == PMF_MEM && cs.dual) {
        add_wire(res, (wire_spec){"raddr", cs.addr, addr_mask}, io->in, &io->n_in);
    }
    add_wire(res, (wire_spec){"we", 1, 1}, io->in, &io->n_in);
    if (cs.kind == PMF_MEM && cs.en) {
        add_wire(res, (wire_spec){"ren", 1, 1}, io->in, &io->n_in);
    }
    add_wire(res, (wire_spec){"out", cs.data, all}, io->out, &io->n_out);
}

static void trace_of(const pb_result *res, pmf_case cs, uint32_t addr_mask, odin3_strbuf *out) {
    nets *io = calloc(1, sizeof *io);
    TEST_ASSERT_NOT_NULL(io);
    collect(res, cs, addr_mask, io);
    rs_trace(res->module, &(rs_io){io->in, io->n_in, io->out, io->n_out},
             (rs_run){SIM_CYCLES, 4242, false}, out);
    free(io);
}

bool pmf_sim_equal(pmf_case cs, const char *arch_xml, const pm_options *opts, uint32_t addr_mask) {
    pb_result before;
    pb_result after;
    odin3_strbuf trace_before;
    odin3_strbuf trace_after;
    odin3_strbuf_init(&trace_before);
    odin3_strbuf_init(&trace_after);
    pmf_build(cs, arch_xml, &before);
    pmf_build(cs, arch_xml, &after);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, pmf_run(&after, opts, NULL));
    trace_of(&before, cs, addr_mask, &trace_before);
    trace_of(&after, cs, addr_mask, &trace_after);
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
