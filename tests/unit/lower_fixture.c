/* lower_fixture.c — see lower_fixture.h. */
#include "lower_fixture.h"

#include "backends/rtlil/writer.h"
#include "ir/celltype.h"
#include "passes/lower/lower_int.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "unity.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_PORTS = 16 };

void lf_begin(lf *fix) {
    fix->design = odin3_design_create();
    TEST_ASSERT_NOT_NULL(fix->design);
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run_begin(fix->design, lf_str(fix, "lower"), &fix->ctx));
}

void lf_end(lf *fix) {
    odin3_design_destroy(fix->design);
    fix->design = NULL;
}

uint32_t lf_str(lf *fix, const char *text) {
    uint32_t str = 0;
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_design_intern(fix->design, odin3_bytes_cstr(text), &str));
    return str;
}

void lf_binary_params(const lf_binary *spec, odin3_value out[5]) {
    out[0] = odin3_value_int(spec->a_signed ? 1 : 0);
    out[1] = odin3_value_int(spec->b_signed ? 1 : 0);
    out[2] = odin3_value_int(spec->a_width);
    out[3] = odin3_value_int(spec->b_width);
    out[4] = odin3_value_int(spec->y_width);
}

void lf_unary_params(const lf_binary *spec, odin3_value out[3]) {
    out[0] = odin3_value_int(spec->a_signed ? 1 : 0);
    out[1] = odin3_value_int(spec->a_width);
    out[2] = odin3_value_int(spec->y_width);
}

odin3_prov_id lf_source(lf *fix, const char *file, uint32_t line) {
    odin3_pass_ctx rd = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                          odin3_pass_run_begin(fix->design, lf_str(fix, "read_verilog"), &rd));
    odin3_srcloc loc = {lf_str(fix, file), line, 1, line, 4, 0};
    odin3_prov_origin origin = {&loc, 1, 0, 0};
    odin3_prov_id id = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_prov_source(&rd, &origin, &id));
    return id;
}

odin3_module *lf_module(lf *fix, const char *name) {
    odin3_module_id mid = {0};
    TEST_ASSERT_EQUAL_INT(
        ODIN3_OK, odin3_module_create(fix->design, lf_str(fix, name), (odin3_prov_id){0}, &mid));
    return odin3_module_get(fix->design, mid);
}

static odin3_celltype_id type_named(lf *fix, const char *type) {
    odin3_celltype_id tid = {0};
    TEST_ASSERT_TRUE(odin3_celltype_find(fix->design, lf_str(fix, type), &tid));
    return tid;
}

odin3_net_id lf_wnet(lf *fix, odin3_module *mod, const char *name, uint32_t bit) {
    odin3_wire_id wire = odin3_module_find_wire(mod, lf_str(fix, name));
    TEST_ASSERT_TRUE(odin3_wire_valid(wire));
    return odin3_wire_net(mod, wire, bit);
}

/* A module port for port `query->port` of the cell type, named prefix + the port name, with the
 * nets of its wire bits in *nets. */
static uint32_t add_cell_port(lf *fix, odin3_module *mod, const char *prefix,
                              const odin3_port_query *query, odin3_net_id **nets) {
    const odin3_celltype_def *def = odin3_celltype_get(fix->design, query->type);
    uint32_t width =
        odin3_celltype_port_width(fix->design, query->type, query->params, query->port);
    TEST_ASSERT_TRUE(width > 0);
    const odin3_port_def *pdef = &def->ports[query->port];
    char wname[64];
    TEST_ASSERT_TRUE(snprintf(wname, sizeof wname, "%s%s", prefix, pdef->name) < (int)sizeof wname);
    odin3_dir dir = pdef->dir == ODIN3_DIR_IN ? ODIN3_DIR_IN : ODIN3_DIR_OUT;
    odin3_port_spec port = {lf_str(fix, wname), dir, width, width == 1, {0}, false, 0, 0, false};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_module_add_port(mod, &port, NULL));
    *nets = calloc(width, sizeof(odin3_net_id));
    TEST_ASSERT_NOT_NULL(*nets);
    for (uint32_t bit = 0; bit < width; bit++) {
        (*nets)[bit] = lf_wnet(fix, mod, wname, bit);
    }
    return width;
}

/* The definition's default parameter values in out (at most MAX_PORTS of them). */
static const odin3_value *fill_defaults(const odin3_celltype_def *def, odin3_value *out) {
    TEST_ASSERT_TRUE(def->n_params <= MAX_PORTS);
    for (uint32_t i = 0; i < def->n_params; i++) {
        out[i] = def->params[i].dflt;
    }
    return out;
}

odin3_node_id lf_cell_prefixed(lf *fix, odin3_module *mod, const lf_cell_spec *spec,
                               const char *prefix) {
    odin3_celltype_id tid = type_named(fix, spec->type);
    const odin3_celltype_def *def = odin3_celltype_get(fix->design, tid);
    TEST_ASSERT_TRUE(def->n_ports <= MAX_PORTS);
    odin3_value defaults[MAX_PORTS];
    const odin3_value *params = spec->params != NULL ? spec->params : fill_defaults(def, defaults);
    uint32_t n_params = spec->params != NULL ? spec->n_params : def->n_params;
    TEST_ASSERT_EQUAL_UINT32(def->n_params, n_params);
    odin3_net_id *nets[MAX_PORTS] = {NULL};
    odin3_netvec ports[MAX_PORTS];
    for (uint32_t pidx = 0; pidx < def->n_ports; pidx++) {
        odin3_port_query query = {tid, params, pidx};
        uint32_t width = add_cell_port(fix, mod, prefix, &query, &nets[pidx]);
        ports[pidx] = (odin3_netvec){nets[pidx], width};
    }
    odin3_node_spec nspec = {tid, spec->name != NULL ? lf_str(fix, spec->name) : 0, spec->prov,
                             params, n_params};
    odin3_node_id node = {0};
    TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_node_create_connected(mod, &nspec, ports, &node));
    for (uint32_t pidx = 0; pidx < def->n_ports; pidx++) {
        free(nets[pidx]);
    }
    return node;
}

odin3_node_id lf_cell(lf *fix, odin3_module *mod, const lf_cell_spec *spec) {
    return lf_cell_prefixed(fix, mod, spec, "");
}

odin3_node_id lf_one_cell(lf *fix, const lf_cell_spec *spec, odin3_module **mod) {
    *mod = lf_module(fix, "m");
    return lf_cell(fix, *mod, spec);
}

uint32_t lf_count_type(lf *fix, odin3_module *mod, const char *type) {
    odin3_celltype_id tid = type_named(fix, type);
    uint32_t count = 0;
    for (uint32_t nid = 1; nid < odin3_module_node_end(mod); nid++) {
        odin3_node_id node = {nid};
        count += odin3_node_live(mod, node) && odin3_node_type(mod, node).v == tid.v ? 1U : 0U;
    }
    return count;
}

uint32_t lf_live_nodes(const odin3_module *mod) {
    uint32_t count = 0;
    for (uint32_t nid = 1; nid < odin3_module_node_end(mod); nid++) {
        count += odin3_node_live(mod, (odin3_node_id){nid}) ? 1U : 0U;
    }
    return count;
}

odin3_status lf_signature(lf *fix, odin3_strbuf *out) {
    uint32_t modules = odin3_design_module_end(fix->design);
    for (uint32_t midx = 1; midx < modules; midx++) {
        odin3_module *mod = odin3_module_get(fix->design, (odin3_module_id){midx});
        TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out, "module %u\n", midx));
        for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
            odin3_node_id node = {id};
            if (!odin3_node_live(mod, node)) {
                continue;
            }
            const odin3_celltype_def *def =
                odin3_celltype_get(fix->design, odin3_node_type(mod, node));
            TEST_ASSERT_EQUAL_INT(ODIN3_OK,
                                  odin3_strbuf_appendf(out, " %u %s name %u prov %u:", id,
                                                       def->name, odin3_node_name(mod, node),
                                                       odin3_node_prov(mod, node).v));
            odin3_pinslice pins = odin3_node_pins(mod, node);
            for (uint32_t i = 0; i < pins.count; i++) {
                odin3_net_id net = odin3_pin_net(mod, (odin3_pin_id){pins.first.v + i});
                TEST_ASSERT_EQUAL_INT(
                    ODIN3_OK, odin3_strbuf_appendf(out, " %u/%u", net.v, odin3_net_name(mod, net)));
            }
            TEST_ASSERT_EQUAL_INT(ODIN3_OK, odin3_strbuf_appendf(out, "\n"));
        }
    }
    return ODIN3_OK;
}

odin3_status lf_rtlil(lf *fix, odin3_strbuf *out) {
    return odin3_rtlil_text(fix->design, out);
}

/* --- simulation ----------------------------------------------------------------------------- */

static bool shapes_agree(const odin3_sim *sim_a, const odin3_sim *sim_b) {
    if (odin3_sim_input_count(sim_a) != odin3_sim_input_count(sim_b) ||
        odin3_sim_output_count(sim_a) != odin3_sim_output_count(sim_b)) {
        return false;
    }
    for (uint32_t pidx = 0; pidx < odin3_sim_input_count(sim_a); pidx++) {
        if (odin3_sim_input_width(sim_a, pidx) != odin3_sim_input_width(sim_b, pidx)) {
            return false;
        }
    }
    for (uint32_t pidx = 0; pidx < odin3_sim_output_count(sim_a); pidx++) {
        if (odin3_sim_output_width(sim_a, pidx) != odin3_sim_output_width(sim_b, pidx)) {
            return false;
        }
    }
    return true;
}

static uint32_t input_bits(const odin3_sim *sim) {
    uint32_t total = 0;
    for (uint32_t pidx = 0; pidx < odin3_sim_input_count(sim); pidx++) {
        total += odin3_sim_input_width(sim, pidx);
    }
    return total;
}

/* Sets input bit number `idx` (counting port by port, LSB first) of sim to value. */
static void set_bit(odin3_sim *sim, uint32_t idx, bool value) {
    for (uint32_t pidx = 0; pidx < odin3_sim_input_count(sim); pidx++) {
        uint32_t width = odin3_sim_input_width(sim, pidx);
        if (idx < width) {
            (void)odin3_sim_set_input(sim, (odin3_sim_bit){pidx, idx}, value);
            return;
        }
        idx -= width;
    }
}

/* Drives both sims with vector `vec` (bit i of vec is input bit i) or, if random, from prng. */
static void drive(odin3_sim *sim_a, odin3_sim *sim_b, uint64_t vec, odin3_prng *prng) {
    uint32_t total = input_bits(sim_a);
    for (uint32_t i = 0; i < total; i++) {
        bool value = prng != NULL ? odin3_prng_bit(prng) : ((vec >> i) & 1U) != 0;
        set_bit(sim_a, i, value);
        set_bit(sim_b, i, value);
    }
}

static bool compare_outputs(const odin3_sim *sim_a, const odin3_sim *sim_b, lf_sim_result *res) {
    for (uint32_t pidx = 0; pidx < odin3_sim_output_count(sim_a); pidx++) {
        for (uint32_t bit = 0; bit < odin3_sim_output_width(sim_a, pidx); bit++) {
            bool va = false;
            bool vb = false;
            (void)odin3_sim_get_output(sim_a, (odin3_sim_bit){pidx, bit}, &va);
            (void)odin3_sim_get_output(sim_b, (odin3_sim_bit){pidx, bit}, &vb);
            if (va != vb) {
                (void)snprintf(res->why, sizeof res->why,
                               "output %s[%u] differs after %llu vectors: before %d after %d",
                               odin3_sim_output_name(sim_a, pidx), bit,
                               (unsigned long long)res->vectors, va ? 1 : 0, vb ? 1 : 0);
                return false;
            }
        }
    }
    return true;
}

/* Applies bits[0..total) to both sims, cycles them and compares the outputs. */
static void try_bits(odin3_sim *sim_a, odin3_sim *sim_b, const bool *bits, lf_sim_result *res) {
    uint32_t total = input_bits(sim_a);
    for (uint32_t i = 0; i < total; i++) {
        set_bit(sim_a, i, bits[i]);
        set_bit(sim_b, i, bits[i]);
    }
    odin3_sim_cycle(sim_a);
    odin3_sim_cycle(sim_b);
    res->vectors++;
    res->equal = compare_outputs(sim_a, sim_b, res);
}

/* Directed corners for the widths random vectors cannot reach (rare outputs of $eq, $reduce_and,
 * ...): all zeros, all ones, each bit alone set, each bit alone clear. */
static void run_corners(odin3_sim *sim_a, odin3_sim *sim_b, bool *bits, lf_sim_result *res) {
    uint32_t total = input_bits(sim_a);
    for (uint32_t kind = 0; kind < 2 && res->equal; kind++) {
        for (uint32_t i = 0; i < total; i++) {
            bits[i] = kind == 1;
        }
        try_bits(sim_a, sim_b, bits, res);
    }
    for (uint32_t kind = 0; kind < 2; kind++) {
        for (uint32_t one = 0; one < total && res->equal; one++) {
            for (uint32_t i = 0; i < total; i++) {
                bits[i] = (i == one) == (kind == 0);
            }
            try_bits(sim_a, sim_b, bits, res);
        }
    }
}

/* Two-operand corners: input port 1 equals port 0 (patterns 0000, 1111, 0101), and equals it with
 * one bit flipped (every bit in turn); the "equal" outputs of $eq/$ne sit there. */
static void run_equal_operands(odin3_sim *sim_a, odin3_sim *sim_b, bool *bits, lf_sim_result *res) {
    if (odin3_sim_input_count(sim_a) < 2 ||
        odin3_sim_input_width(sim_a, 0) != odin3_sim_input_width(sim_a, 1)) {
        return;
    }
    uint32_t width = odin3_sim_input_width(sim_a, 0);
    uint32_t total = input_bits(sim_a);
    for (uint32_t pattern = 0; pattern < 3; pattern++) {
        for (uint32_t flip = 0; flip <= width && res->equal; flip++) {
            for (uint32_t i = 0; i < total; i++) {
                bits[i] = false;
            }
            for (uint32_t i = 0; i < width; i++) {
                bool value = pattern == 1 || (pattern == 2 && i % 2 == 0);
                bits[i] = value;
                bits[width + i] = value != (flip == i + 1);
            }
            try_bits(sim_a, sim_b, bits, res);
        }
    }
}

static void run_vectors(odin3_sim *sim_a, odin3_sim *sim_b, const lf_sim_opts *opts,
                        lf_sim_result *res) {
    uint32_t total = input_bits(sim_a);
    odin3_prng prng;
    odin3_prng_seed(&prng, opts->seed);
    bool all = total <= opts->exhaustive_bits && total < 64;
    uint64_t count = all ? (uint64_t)1 << total : opts->random_vectors;
    for (uint64_t vnum = 0; vnum < count && res->equal; vnum++) {
        drive(sim_a, sim_b, vnum, all ? NULL : &prng);
        odin3_sim_cycle(sim_a);
        odin3_sim_cycle(sim_b);
        res->vectors++;
        res->equal = compare_outputs(sim_a, sim_b, res);
    }
    bool *bits = all ? NULL : calloc((size_t)total + 1, sizeof *bits);
    if (!all) {
        TEST_ASSERT_NOT_NULL(bits);
        run_corners(sim_a, sim_b, bits, res);
        run_equal_operands(sim_a, sim_b, bits, res);
    }
    free(bits);
}

lf_sim_result lf_sim_equal(odin3_design *design_a, odin3_module_id top_a, odin3_design *design_b,
                           odin3_module_id top_b, const lf_sim_opts *opts) {
    lf_sim_result res = {false, 0, ""};
    odin3_sim *sim_a = NULL;
    odin3_sim *sim_b = NULL;
    if (odin3_sim_build(design_a, top_a, &sim_a) != ODIN3_OK) {
        (void)snprintf(res.why, sizeof res.why, "the before design does not build");
    } else if (odin3_sim_build(design_b, top_b, &sim_b) != ODIN3_OK) {
        (void)snprintf(res.why, sizeof res.why, "the after design does not build");
    } else if (!shapes_agree(sim_a, sim_b)) {
        (void)snprintf(res.why, sizeof res.why, "the two designs' ports differ");
    } else {
        res.equal = true;
        run_vectors(sim_a, sim_b, opts, &res);
    }
    odin3_sim_destroy(sim_a);
    odin3_sim_destroy(sim_b);
    return res;
}

/* --- counts and the reference-model simulation ----------------------------------------------- */

uint32_t lf_gate_nodes(lf *fix, odin3_module *mod) {
    uint32_t count = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(mod, node)) {
            continue;
        }
        const odin3_celltype_def *def = odin3_celltype_get(fix->design, odin3_node_type(mod, node));
        count += def->gran == ODIN3_GRAN_BIT && strncmp(def->name, "$_CONST", 7) != 0 ? 1U : 0U;
    }
    return count;
}

uint32_t lf_word_nodes(lf *fix, odin3_module *mod) {
    uint32_t count = 0;
    for (uint32_t id = 1; id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        count += odin3_node_live(mod, node) &&
                         odin3_celltype_get(fix->design, odin3_node_type(mod, node))->gran ==
                             ODIN3_GRAN_WORD
                     ? 1U
                     : 0U;
    }
    return count;
}

static uint32_t output_bits(const odin3_sim *sim) {
    uint32_t total = 0;
    for (uint32_t pidx = 0; pidx < odin3_sim_output_count(sim); pidx++) {
        total += odin3_sim_output_width(sim, pidx);
    }
    return total;
}

/* Compares the simulator's outputs with the model's wanted bits where care is set. */
static bool compare_model(const odin3_sim *sim, const uint8_t *want, const uint8_t *care,
                          lf_sim_result *res) {
    uint32_t idx = 0;
    for (uint32_t pidx = 0; pidx < odin3_sim_output_count(sim); pidx++) {
        for (uint32_t bit = 0; bit < odin3_sim_output_width(sim, pidx); bit++, idx++) {
            bool got = false;
            (void)odin3_sim_get_output(sim, (odin3_sim_bit){pidx, bit}, &got);
            if (care[idx] != 0 && (got ? 1 : 0) != want[idx]) {
                (void)snprintf(res->why, sizeof res->why,
                               "output %s[%u] differs after %llu vectors: model %d simulator %d",
                               odin3_sim_output_name(sim, pidx), bit,
                               (unsigned long long)res->vectors, want[idx], got ? 1 : 0);
                return false;
            }
        }
    }
    return true;
}

static void model_vectors(odin3_sim *sim, lf_model_fn model, const void *user,
                          const lf_sim_opts *opts, lf_sim_result *res) {
    uint32_t total = input_bits(sim);
    odin3_prng prng;
    odin3_prng_seed(&prng, opts->seed);
    bool all = total <= opts->exhaustive_bits && total < 64;
    uint64_t count = all ? (uint64_t)1 << total : opts->random_vectors;
    uint8_t in[LF_MODEL_BITS];
    uint8_t want[LF_MODEL_BITS];
    uint8_t care[LF_MODEL_BITS];
    for (uint64_t vnum = 0; vnum < count && res->equal; vnum++) {
        for (uint32_t i = 0; i < total; i++) {
            bool value = all ? ((vnum >> i) & 1U) != 0 : odin3_prng_bit(&prng);
            in[i] = value ? 1U : 0U;
            set_bit(sim, i, value);
        }
        memset(want, 0, sizeof want);
        memset(care, 1, sizeof care);
        lf_model_io io = {in, want, care};
        model(&io, user);
        odin3_sim_cycle(sim);
        res->vectors++;
        res->equal = compare_model(sim, want, care, res);
    }
}

lf_sim_result lf_sim_model(odin3_design *design, odin3_module_id top, lf_model_fn model,
                           const void *user, const lf_sim_opts *opts) {
    lf_sim_result res = {false, 0, ""};
    odin3_sim *sim = NULL;
    if (odin3_sim_build(design, top, &sim) != ODIN3_OK) {
        (void)snprintf(res.why, sizeof res.why, "the design does not build");
    } else if (input_bits(sim) > LF_MODEL_BITS || output_bits(sim) > LF_MODEL_BITS) {
        (void)snprintf(res.why, sizeof res.why, "the module is wider than LF_MODEL_BITS");
    } else {
        res.equal = true;
        model_vectors(sim, model, user, opts, &res);
    }
    odin3_sim_destroy(sim);
    return res;
}

uint64_t lf_estimate(const char *type, const odin3_value *params, const uint32_t *widths,
                     uint32_t n_ports) {
    enum { MAX_NETS = 4096 };
    static const odin3_net_id none[MAX_NETS];
    lw_cell cell;
    memset(&cell, 0, sizeof cell);
    cell.params = params;
    for (uint32_t pidx = 0; pidx < n_ports; pidx++) {
        TEST_ASSERT_TRUE(widths[pidx] <= MAX_NETS);
        cell.port[pidx] = (lw_span){none, widths[pidx]};
    }
    cell.n_ports = n_ports;
    cell.out_port = n_ports - 1;
    const lw_rule *rule = lw_rule_find(type);
    TEST_ASSERT_NOT_NULL_MESSAGE(rule, type);
    return rule->estimate(&cell);
}
