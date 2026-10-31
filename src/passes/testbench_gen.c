/* testbench_gen.c — the `testbench_gen` pass: writes a Verilog testbench for the top module from
 * its ports (spec docs/specs/2026-10-31-testbench-design.md section 5). */
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "util/log.h"
#include "util/str.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { PATH_LEN = 1024, DEFAULT_CYCLES = 100, RESET_CYCLES = 4, MAX_PORTS = 256, DEC = 10 };

typedef struct gen_args {
    char out[PATH_LEN];
    uint64_t cycles;
    uint64_t seed;
} gen_args;

static char g_clk[128] = "clk"; /* the clock port's name (simple identifiers only) */

typedef enum { ROLE_CLOCK, ROLE_RESET, ROLE_DATA } role;

typedef struct in_port {
    uint32_t index, width;
    role kind;
    bool active_low;
} in_port;

typedef struct gen {
    const odin3_sim *sim;
    const char *top, *out;
    in_port ins[MAX_PORTS];
    uint32_t nin;
    bool have_clk;
    uint64_t cycles;
} gen;

static bool word_is(odin3_bytes word, const char *text) {
    return word.len == strlen(text) && memcmp(word.ptr, text, word.len) == 0;
}

static bool lower_has(const char *name, const char *needle) {
    char low[128];
    size_t n = 0;
    for (; name[n] != '\0' && n + 1 < sizeof low; n++) {
        low[n] = (char)tolower((unsigned char)name[n]);
    }
    low[n] = '\0';
    return strstr(low, needle) != NULL;
}

static bool name_is_clock(const char *name) {
    return lower_has(name, "clk") || lower_has(name, "clock") || strcasecmp(name, "ck") == 0;
}

static bool name_is_reset(const char *name, bool *low) {
    static const char *const LOW[] = {"rstn",     "rst_n", "reset_n", "resetn",  "nrst",
                                      "nreset",   "rst_b", "rstb",    "reset_b", "resetb",
                                      "areset_n", "arstn", NULL};
    static const char *const HIGH[] = {"rst",   "reset",  "arst", "areset", "clr",
                                       "clear", "sreset", "srst", NULL};
    for (size_t i = 0; LOW[i] != NULL; i++) {
        if (strcasecmp(name, LOW[i]) == 0) {
            *low = true;
            return true;
        }
    }
    for (size_t i = 0; HIGH[i] != NULL; i++) {
        if (strcasecmp(name, HIGH[i]) == 0) {
            *low = false;
            return true;
        }
    }
    return false;
}

static void ident(odin3_strbuf *buf, const char *name) {
    bool simple = isalpha((unsigned char)name[0]) || name[0] == '_';
    for (const char *c = name; *c != '\0'; c++) {
        simple = simple && (isalnum((unsigned char)*c) || *c == '_');
    }
    (void)odin3_strbuf_appendf(buf, simple ? "%s" : "\\%s ", name);
}

static void file_stem(char *dst, size_t cap, const char *name) {
    size_t n = 0;
    for (; name[n] != '\0' && n + 1 < cap; n++) {
        dst[n] = isalnum((unsigned char)name[n]) ? name[n] : '_';
    }
    dst[n] = '\0';
}

static void decl(odin3_strbuf *buf, const char *kind, const char *name, uint32_t width) {
    (void)odin3_strbuf_appendf(buf, "  %s ", kind);
    if (width > 1) {
        (void)odin3_strbuf_appendf(buf, "[%u:0] ", width - 1U);
    }
    ident(buf, name);
    (void)odin3_strbuf_appendf(buf, ";\n");
}

static odin3_status write_vectors(const char *out, const char *name, uint32_t width,
                                  uint64_t cycles, odin3_prng *prng) {
    char stem[128];
    char path[PATH_LEN + 160];
    file_stem(stem, sizeof stem, name);
    (void)snprintf(path, sizeof path, "%s.%s.hex", out, stem);
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    for (uint64_t c = 0; c < cycles; c++) {
        uint64_t val = 0;
        for (uint32_t b = 0; b < width; b++) {
            val |= odin3_prng_bit(prng) ? (uint64_t)1 << b : 0;
        }
        (void)odin3_strbuf_appendf(&text, "%llx\n", (unsigned long long)val);
    }
    odin3_status st = odin3_pass_write_text(path, &text);
    odin3_strbuf_free(&text);
    return st;
}

static void emit_header(odin3_strbuf *tb, const gen *g) {
    const odin3_sim *sim = g->sim;
    const in_port *ins = g->ins;
    const char *top = g->top;
    uint32_t nin = g->nin;
    bool have_clk = g->have_clk;
    (void)odin3_strbuf_appendf(tb, "`timescale 1ns/1ps\nmodule tb_%s;\n", top);
    if (!have_clk) {
        (void)odin3_strbuf_appendf(tb, "  reg %s;\n", g_clk);
    }
    for (uint32_t i = 0; i < nin; i++) {
        decl(tb, "reg", odin3_sim_input_name(sim, ins[i].index), ins[i].width);
    }
    for (uint32_t o = 0; o < odin3_sim_output_count(sim); o++) {
        decl(tb, "wire", odin3_sim_output_name(sim, o), odin3_sim_output_width(sim, o));
    }
}

static void emit_dut(odin3_strbuf *tb, const gen *g) {
    const odin3_sim *sim = g->sim;
    const char *top = g->top;
    (void)odin3_strbuf_appendf(tb, "  %s dut(", top);
    uint32_t total = odin3_sim_input_count(sim) + odin3_sim_output_count(sim);
    for (uint32_t k = 0; k < total; k++) {
        bool is_in = k < odin3_sim_input_count(sim);
        const char *name = is_in ? odin3_sim_input_name(sim, k)
                                 : odin3_sim_output_name(sim, k - odin3_sim_input_count(sim));
        (void)odin3_strbuf_appendf(tb, "%s.", k > 0 ? ", " : "");
        ident(tb, name);
        (void)odin3_strbuf_appendf(tb, "(");
        ident(tb, name);
        (void)odin3_strbuf_appendf(tb, ")");
    }
    (void)odin3_strbuf_appendf(tb, ");\n  always #5 %s = ~%s;\n", g_clk, g_clk);
}

static void emit_body(odin3_strbuf *tb, const gen *g) {
    const odin3_sim *sim = g->sim;
    const in_port *ins = g->ins;
    const char *out = g->out;
    uint32_t nin = g->nin;
    uint64_t cycles = g->cycles;
    char stem[128];
    for (uint32_t i = 0; i < nin; i++) {
        if (ins[i].kind == ROLE_DATA) {
            file_stem(stem, sizeof stem, odin3_sim_input_name(sim, ins[i].index));
            (void)odin3_strbuf_appendf(tb, "  reg [%u:0] vec_%s [0:%llu];\n", ins[i].width - 1U,
                                       stem, (unsigned long long)(cycles > 0 ? cycles - 1U : 0));
        }
    }
    (void)odin3_strbuf_appendf(tb, "  integer cyc;\n  initial begin\n    %s = 0;\n", g_clk);
    for (uint32_t i = 0; i < nin; i++) {
        const char *name = odin3_sim_input_name(sim, ins[i].index);
        if (ins[i].kind == ROLE_DATA) {
            file_stem(stem, sizeof stem, name);
            (void)odin3_strbuf_appendf(tb, "    $readmemh(\"%s.%s.hex\", vec_%s);\n", out, stem,
                                       stem);
        }
        if (ins[i].kind != ROLE_CLOCK) {
            (void)odin3_strbuf_appendf(tb, "    ");
            ident(tb, name);
            (void)odin3_strbuf_appendf(tb, " = %d;\n",
                                       ins[i].kind == ROLE_RESET && !ins[i].active_low);
            if (ins[i].kind == ROLE_RESET && ins[i].active_low) {
                (void)odin3_strbuf_appendf(tb, "    ");
                ident(tb, name);
                (void)odin3_strbuf_appendf(tb, " = 0;\n");
            }
        }
    }
    (void)odin3_strbuf_appendf(tb, "    repeat (%d) @(posedge %s);\n", RESET_CYCLES, g_clk);
}

static void emit_loop(odin3_strbuf *tb, const gen *g) {
    const odin3_sim *sim = g->sim;
    const in_port *ins = g->ins;
    uint32_t nin = g->nin;
    uint64_t cycles = g->cycles;
    char stem[128];
    for (uint32_t i = 0; i < nin; i++) {
        if (ins[i].kind == ROLE_RESET) {
            (void)odin3_strbuf_appendf(tb, "    @(negedge %s) ", g_clk);
            ident(tb, odin3_sim_input_name(sim, ins[i].index));
            (void)odin3_strbuf_appendf(tb, " = %d;\n", ins[i].active_low ? 1 : 0);
        }
    }
    (void)odin3_strbuf_appendf(
        tb, "    for (cyc = 0; cyc < %llu; cyc = cyc + 1) begin\n      @(negedge %s);\n",
        (unsigned long long)cycles, g_clk);
    for (uint32_t i = 0; i < nin; i++) {
        if (ins[i].kind == ROLE_DATA) {
            const char *name = odin3_sim_input_name(sim, ins[i].index);
            file_stem(stem, sizeof stem, name);
            (void)odin3_strbuf_appendf(tb, "      ");
            ident(tb, name);
            (void)odin3_strbuf_appendf(tb, " = vec_%s[cyc];\n", stem);
        }
    }
    (void)odin3_strbuf_appendf(tb, "      #4;\n      $display(\"%%0d");
    for (uint32_t o = 0; o < odin3_sim_output_count(sim); o++) {
        (void)odin3_strbuf_appendf(tb, " %%h");
    }
    (void)odin3_strbuf_appendf(tb, "\", cyc");
    for (uint32_t o = 0; o < odin3_sim_output_count(sim); o++) {
        (void)odin3_strbuf_appendf(tb, ", ");
        ident(tb, odin3_sim_output_name(sim, o));
    }
    (void)odin3_strbuf_appendf(tb, ");\n    end\n    $finish;\n  end\nendmodule\n");
}

/* Classifies the top's inputs; returns the count, or -1 on an unsupported port. */
static int classify(const odin3_sim *sim, in_port *ins, bool *have_clk) {
    uint32_t nin = odin3_sim_input_count(sim);
    bool clk_found = false;
    (void)snprintf(g_clk, sizeof g_clk, "clk");
    for (uint32_t i = 0; i < nin && i < MAX_PORTS; i++) {
        const char *name = odin3_sim_input_name(sim, i);
        odin3_sim_bit bit = {i, 0};
        ins[i] = (in_port){i, odin3_sim_input_width(sim, i), ROLE_DATA, false};
        if (ins[i].width > 64) {
            odin3_log(ODIN3_LOG_ERROR, "testbench_gen: input `%s` is wider than 64 bits", name);
            return -1;
        }
        if (odin3_sim_input_is_clock(sim, bit)) {
            ins[i].kind = ROLE_CLOCK;
        } else if (ins[i].width == 1 && name_is_reset(name, &ins[i].active_low)) {
            ins[i].kind = ROLE_RESET;
        }
        clk_found = clk_found || ins[i].kind == ROLE_CLOCK;
    }
    for (uint32_t i = 0; i < nin && !clk_found; i++) {
        if (ins[i].width == 1 && ins[i].kind == ROLE_DATA &&
            name_is_clock(odin3_sim_input_name(sim, i))) {
            ins[i].kind = ROLE_CLOCK;
            clk_found = true;
        }
    }
    for (uint32_t i = 0; i < nin; i++) {
        if (ins[i].kind == ROLE_CLOCK) {
            (void)snprintf(g_clk, sizeof g_clk, "%s", odin3_sim_input_name(sim, i));
            break;
        }
    }
    *have_clk = clk_found;
    return (int)nin;
}

static odin3_status check_outputs(const odin3_sim *sim) {
    odin3_status st = ODIN3_OK;
    for (uint32_t o = 0; o < odin3_sim_output_count(sim); o++) {
        if (odin3_sim_output_width(sim, o) > 64) {
            odin3_log(ODIN3_LOG_ERROR, "testbench_gen: output `%s` is wider than 64 bits",
                      odin3_sim_output_name(sim, o));
            st = ODIN3_ERR_INVALID_ARG;
        }
    }
    return st;
}

static odin3_status write_all(const gen *g, const gen_args *args, odin3_strbuf *tb) {
    odin3_prng prng;
    odin3_status st = ODIN3_OK;
    odin3_prng_seed(&prng, args->seed);
    emit_header(tb, g);
    emit_dut(tb, g);
    emit_body(tb, g);
    emit_loop(tb, g);
    for (uint32_t i = 0; i < g->nin && st == ODIN3_OK; i++) {
        if (g->ins[i].kind == ROLE_DATA) {
            st = write_vectors(args->out, odin3_sim_input_name(g->sim, g->ins[i].index),
                               g->ins[i].width, args->cycles, &prng);
        }
    }
    return st == ODIN3_OK ? odin3_pass_write_text(args->out, tb) : st;
}

static odin3_status generate(odin3_design *design, const gen_args *args) {
    odin3_module_id top = odin3_design_top(design);
    odin3_sim *sim = NULL;
    if (!odin3_module_valid(top)) {
        odin3_log(ODIN3_LOG_ERROR, "testbench_gen: the design has no top module");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = odin3_sim_build(design, top, &sim);
    if (st != ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "testbench_gen: the top must be simulatable (run lower first)");
        return st;
    }
    gen *g = calloc(1, sizeof *g);
    odin3_strbuf tb;
    odin3_strbuf_init(&tb);
    st = g == NULL ? ODIN3_ERR_NO_MEMORY : check_outputs(sim);
    if (st == ODIN3_OK) {
        int nin = classify(sim, g->ins, &g->have_clk);
        g->nin = nin < 0 ? 0U : (uint32_t)nin;
        g->sim = sim;
        g->out = args->out;
        g->cycles = args->cycles;
        g->top = odin3_strtab_get(odin3_design_strtab(design),
                                  odin3_module_name(odin3_module_get(design, top)));
        st = nin < 0 ? ODIN3_ERR_INVALID_ARG : write_all(g, args, &tb);
    }
    free(g);
    odin3_strbuf_free(&tb);
    odin3_sim_destroy(sim);
    return st;
}

static bool take(odin3_bytes *args, char *dst, size_t cap) {
    odin3_bytes word = odin3_pass_arg_next(args);
    if (word.ptr == NULL || word.len == 0 || word.len >= cap) {
        return false;
    }
    memcpy(dst, word.ptr, word.len);
    dst[word.len] = '\0';
    return true;
}

static bool take_number(odin3_bytes *args, uint64_t *out) {
    char text[32] = "";
    char *end = NULL;
    if (!take(args, text, sizeof text)) {
        return false;
    }
    errno = 0;
    *out = strtoull(text, &end, DEC);
    return errno == 0 && *end == '\0';
}

static odin3_status testbench_gen_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                      void *user) {
    (void)ctx;
    (void)user;
    gen_args parsed = {{0}, DEFAULT_CYCLES, 1};
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        bool ok = true;
        if (word_is(word, "--out")) {
            ok = take(&args, parsed.out, sizeof parsed.out);
        } else if (word_is(word, "--cycles")) {
            ok = take_number(&args, &parsed.cycles) && parsed.cycles > 0;
        } else if (word_is(word, "--seed")) {
            ok = take_number(&args, &parsed.seed);
        } else {
            ok = false;
        }
        if (!ok) {
            odin3_log(ODIN3_LOG_ERROR,
                      "testbench_gen: expects --out <tb.v> [--cycles N] [--seed N]");
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    if (parsed.out[0] == '\0') {
        odin3_log(ODIN3_LOG_ERROR, "testbench_gen: --out <tb.v> is required");
        return ODIN3_ERR_INVALID_ARG;
    }
    return generate(design, &parsed);
}

const odin3_pass_def odin3_pass_testbench_gen = {
    "testbench_gen",
    "testbench_gen --out <tb.v> [--cycles N] [--seed N]: write a Verilog testbench for the top "
    "(clock and reset detection, reset sequence, N random vectors in <tb.v>.<input>.hex files, "
    "one output line per cycle); the top must be in the netlist view (run lower first)",
    testbench_gen_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
