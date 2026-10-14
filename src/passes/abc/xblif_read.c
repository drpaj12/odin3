/*
 * xblif_read.c — the BLIF exchange's read-back (3D spec §6.2, AB-15; plan Task 16): ABC's output
 * read with 1C's reader into a scratch design, then turned into an abc_result.
 */
#include "frontends/blif/reader.h"
#include "ir/lut.h"
#include "passes/abc/abc.h"
#include "passes/abc/region.h"
#include "passes/abc/result.h"
#include "passes/abc/run.h"
#include "passes/abc/xblif.h"
#include "util/attr.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    NO_OBJ = UINT32_MAX, /* a net without an object yet */
    FIRST_OBJ = 2,
    MAX_WORDS = 1U << (ODIN3_LUT_MAX_WIDTH - 6U),
    WHY_BYTES = 256,
    WORD_BITS = 64,
    SOP_PORT_A = 0
};

/* The state of one read-back. */
typedef struct rdctx {
    odin3_abc_run *run;
    odin3_design *scratch;
    odin3_module *module;
    odin3_abc_result *res;
    odin3_vec obj;      /* uint32_t per net: its object, or NO_OBJ */
    odin3_vec sops;     /* uint32_t: the $sop nodes in ID order */
    odin3_vec latches;  /* uint32_t: the latch nodes in ID order */
    odin3_vec indeg;    /* uint32_t per sop index */
    odin3_vec heap;     /* ready sop nodes */
    odin3_vec sop_pos;  /* uint32_t per node: 1 + sop index */
    odin3_vec in_nets;  /* uint32_t: net of each CI port by index i (0: absent) */
    odin3_vec out_nets; /* uint32_t: net of each CO port by index i */
    char why[WHY_BYTES];
} rdctx;

static uint32_t u32_at(const odin3_vec *vec, size_t idx) {
    return ((const uint32_t *)vec->data)[idx];
}

static odin3_status push_u32(odin3_vec *vec, uint32_t value) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

/* Records the why text and returns ENGINE (the caller logs it once). */
static odin3_status malformed(rdctx *rd, const char *fmt, ...) ODIN3_PRINTF(2, 3);

static odin3_status malformed(rdctx *rd, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(rd->why, sizeof rd->why, fmt, args);
    va_end(args);
    return ODIN3_ERR_ENGINE;
}

static const char *text_of(const rdctx *rd, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(rd->scratch), str);
}

/* The index n of a name `<prefix><n>` (decimal, no sign), or false. */
static bool name_index(const char *name, const char *prefix, uint32_t *index) {
    size_t plen = strlen(prefix);
    if (strncmp(name, prefix, plen) != 0 || name[plen] == '\0' || strlen(name + plen) > 9) {
        return false;
    }
    uint32_t value = 0;
    for (const char *c = name + plen; *c != '\0'; c++) {
        if (*c < '0' || *c > '9') {
            return false;
        }
        value = value * 10U + (uint32_t)(*c - '0');
    }
    *index = value;
    return true;
}

/* --- ports --------------------------------------------------------------------------------- */

static odin3_net_id port_net(const rdctx *rd, uint32_t index) {
    return odin3_wire_net(rd->module, odin3_module_port_wire(rd->module, index), 0);
}

static bool port_is_input(const rdctx *rd, uint32_t index) {
    odin3_node_id node = odin3_module_port(rd->module, index);
    const odin3_celltype_def *def =
        odin3_celltype_get(rd->scratch, odin3_node_type(rd->module, node));
    return strcmp(def->name, "$port_in") == 0;
}

static const char *port_name(const rdctx *rd, uint32_t index) {
    return text_of(rd, odin3_wire_name(rd->module, odin3_module_port_wire(rd->module, index)));
}

/* Counts the ports against what was sent, then fills in_nets / out_nets by index. */
static odin3_status read_ports(rdctx *rd) {
    uint32_t n_in = 0;
    uint32_t n_out = 0;
    uint32_t ports = odin3_module_port_count(rd->module);
    for (uint32_t i = 0; i < ports; i++) {
        n_in += port_is_input(rd, i) ? 1U : 0U;
        n_out += port_is_input(rd, i) ? 0U : 1U;
    }
    const odin3_abc_region *rg = rd->run->region;
    if (n_in != rg->cis.len) {
        return malformed(rd, "%u primary inputs, expected %zu", (unsigned)n_in, rg->cis.len);
    }
    if (n_out != rg->cos.len) {
        return malformed(rd, "%u outputs, expected %zu", (unsigned)n_out, rg->cos.len);
    }
    for (size_t i = 0; i < n_in; i++) {
        (void)push_u32(&rd->in_nets, 0);
    }
    for (size_t i = 0; i < n_out; i++) {
        (void)push_u32(&rd->out_nets, 0);
    }
    if (rd->in_nets.len != n_in || rd->out_nets.len != n_out) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < ports; i++) {
        bool input = port_is_input(rd, i);
        uint32_t idx = 0;
        const char *name = port_name(rd, i);
        odin3_vec *dst = input ? &rd->in_nets : &rd->out_nets;
        if (!name_index(name, input ? "o3ci" : "o3co", &idx) || idx >= dst->len ||
            u32_at(dst, idx) != 0) {
            return malformed(rd, "an unknown %s '%s'", input ? "input" : "output", name);
        }
        ((uint32_t *)dst->data)[idx] = port_net(rd, i).v;
    }
    return ODIN3_OK;
}

/* --- cells --------------------------------------------------------------------------------- */

/* Sorts the nodes into $sop and latch lists; anything else is refused. */
static odin3_status read_cells(rdctx *rd) {
    for (uint32_t id = 1; id < odin3_module_node_end(rd->module); id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(rd->module, node)) {
            continue;
        }
        const odin3_celltype_def *def =
            odin3_celltype_get(rd->scratch, odin3_node_type(rd->module, node));
        odin3_status st = ODIN3_OK;
        if (def->gran == ODIN3_GRAN_PORT) {
            continue;
        }
        if (strcmp(def->name, "$sop") == 0) {
            st = push_u32(&rd->sops, id);
        } else if (strcmp(def->name, "$_FF_") == 0) {
            st = push_u32(&rd->latches, id);
        } else {
            if (def->gran == ODIN3_GRAN_BIT) {
                return malformed(rd, "an unexpected cell '%s'", def->name);
            }
            return malformed(rd, "a .subckt");
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static odin3_net_id node_out_net(const rdctx *rd, odin3_node_id node, uint32_t port) {
    odin3_pinslice pins = odin3_node_port(rd->module, node, port);
    return pins.count == 0 ? (odin3_net_id){0} : odin3_pin_net(rd->module, pins.first);
}

static uint32_t *obj_slot(rdctx *rd, odin3_net_id net) {
    return (uint32_t *)rd->obj.data + net.v;
}

/* Latches: Q must be o3q<k> and init 0; fills reg_from and the Q objects. */
static odin3_status read_latches(rdctx *rd) {
    odin3_abc_result *res = rd->res;
    for (size_t j = 0; j < rd->latches.len; j++) {
        odin3_node_id node = {u32_at(&rd->latches, j)};
        const odin3_value *init = odin3_node_param(rd->module, node, 0);
        if (init != NULL && init->i != 0) {
            return malformed(rd, "a latch with init %d (all were sent at 0)", (int)init->i);
        }
        odin3_net_id q = node_out_net(rd, node, 1);
        uint32_t k = ODIN3_ABC_NO_REG;
        const char *name = odin3_net_valid(q) && odin3_net_name(rd->module, q) != 0
                               ? text_of(rd, odin3_net_name(rd->module, q))
                               : NULL;
        if (name == NULL || !name_index(name, "o3q", &k)) {
            k = ODIN3_ABC_NO_REG;
        }
        odin3_status st = push_u32(&res->reg_from, k);
        if (st != ODIN3_OK) {
            return st;
        }
        if (odin3_net_valid(q)) {
            *obj_slot(rd, q) = FIRST_OBJ + res->n_cis + (uint32_t)j;
        }
    }
    return ODIN3_OK;
}

/* --- truth tables -------------------------------------------------------------------------- */

static const uint64_t k_var_pattern[6] = {0xAAAAAAAAAAAAAAAAULL, 0xCCCCCCCCCCCCCCCCULL,
                                          0xF0F0F0F0F0F0F0F0ULL, 0xFF00FF00FF00FF00ULL,
                                          0xFFFF0000FFFF0000ULL, 0xFFFFFFFF00000000ULL};

/* The word w of the value of variable var over the table's minterms. */
static uint64_t var_word(uint32_t var, uint32_t w) {
    if (var < 6) {
        return k_var_pattern[var];
    }
    return ((w >> (var - 6U)) & 1U) != 0 ? ~0ULL : 0ULL;
}

/* table |= the cube row (k plane characters '0' '1' '-'). */
static void add_cube(const uint8_t *row, uint32_t k, uint32_t words, uint64_t *table) {
    for (uint32_t w = 0; w < words; w++) {
        uint64_t cube = ~0ULL;
        for (uint32_t v = 0; v < k; v++) {
            if (row[v] != '-') {
                uint64_t mask = var_word(v, w);
                cube &= row[v] == '1' ? mask : ~mask;
            }
        }
        table[w] |= cube;
    }
}

/* The truth table of a $sop cover over k inputs into table (odin3_lut_words(k) words). */
static void cover_table(const odin3_value *cover, uint32_t k, uint64_t *table) {
    uint32_t words = odin3_lut_words(k);
    memset(table, 0, sizeof(uint64_t) * words);
    size_t stride = (size_t)k + 1U;
    bool off_set = false;
    for (size_t off = 0; off + stride <= cover->len; off += stride) {
        add_cube(cover->bits + off, k, words, table);
        off_set = cover->bits[off + k] == '0';
    }
    if (off_set) {
        for (uint32_t w = 0; w < words; w++) {
            table[w] = ~table[w];
        }
    }
    if (k < 6) {
        table[0] &= (1ULL << (1U << k)) - 1ULL;
    }
}

/* A one-input `1 1` cover: a connection, not a LUT. */
static bool is_buffer(const odin3_value *cover, uint32_t k) {
    return k == 1 && cover->len == 2 && cover->bits[0] == '1' && cover->bits[1] == '1';
}

/* --- ordering ------------------------------------------------------------------------------ */

/* The object of net, or NO_OBJ. */
static uint32_t net_obj(rdctx *rd, odin3_net_id net) {
    return odin3_net_valid(net) ? *obj_slot(rd, net) : NO_OBJ;
}

static odin3_pinslice sop_inputs(const rdctx *rd, odin3_node_id node) {
    return odin3_node_port(rd->module, node, SOP_PORT_A);
}

/* True when a sop node reads net and net has a driver that is neither assigned nor a sop. */
static odin3_status check_driven(rdctx *rd, odin3_node_id node) {
    odin3_pinslice ins = sop_inputs(rd, node);
    for (uint32_t i = 0; i < ins.count; i++) {
        odin3_net_id net = odin3_pin_net(rd->module, (odin3_pin_id){ins.first.v + i});
        if (net_obj(rd, net) == NO_OBJ && odin3_net_driver_count(rd->module, net) == 0) {
            return malformed(rd, "an undriven net '%s'",
                             text_of(rd, odin3_net_name(rd->module, net)));
        }
    }
    return ODIN3_OK;
}

static odin3_status init_order(rdctx *rd) {
    size_t count = rd->sops.len;
    odin3_status st = odin3_vec_reserve(&rd->indeg, count + 1U);
    st = st == ODIN3_OK ? odin3_vec_reserve(&rd->sop_pos, odin3_module_node_end(rd->module))
                        : st;
    for (uint32_t id = 0; st == ODIN3_OK && id < odin3_module_node_end(rd->module); id++) {
        st = push_u32(&rd->sop_pos, 0);
    }
    for (size_t s = 0; st == ODIN3_OK && s < count; s++) {
        ((uint32_t *)rd->sop_pos.data)[u32_at(&rd->sops, s)] = (uint32_t)s + 1U;
    }
    for (size_t s = 0; st == ODIN3_OK && s < count; s++) {
        odin3_node_id node = {u32_at(&rd->sops, s)};
        st = check_driven(rd, node);
        odin3_pinslice ins = sop_inputs(rd, node);
        uint32_t deg = 0;
        for (uint32_t i = 0; st == ODIN3_OK && i < ins.count; i++) {
            odin3_net_id net = odin3_pin_net(rd->module, (odin3_pin_id){ins.first.v + i});
            deg += net_obj(rd, net) == NO_OBJ ? 1U : 0U;
        }
        st = st == ODIN3_OK ? push_u32(&rd->indeg, deg) : st;
        st = st == ODIN3_OK && deg == 0 ? odin3_abc_heap_push(&rd->heap, node.v) : st;
    }
    return st;
}

/* Net got its object: one input of each sop reader is resolved. */
static odin3_status release_net(rdctx *rd, odin3_net_id net) {
    odin3_pinlist sinks = odin3_net_sinks(rd->module, net);
    for (uint32_t i = 0; i < sinks.count; i++) {
        uint32_t reader = odin3_pin_node(rd->module, sinks.pins[i]).v;
        uint32_t pos = u32_at(&rd->sop_pos, reader);
        if (pos == 0) {
            continue;
        }
        uint32_t *deg = (uint32_t *)rd->indeg.data + (pos - 1U);
        if (*deg > 0 && --*deg == 0) {
            odin3_status st = odin3_abc_heap_push(&rd->heap, reader);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

/* --- emitting a LUT ------------------------------------------------------------------------ */

/* Appends the sop node as a LUT of the result, or maps its net to a connection or constant. */
static odin3_status emit_sop(rdctx *rd, odin3_node_id node) {
    odin3_abc_result *res = rd->res;
    const odin3_value *cover = odin3_node_param(rd->module, node, 1);
    odin3_pinslice ins = sop_inputs(rd, node);
    uint32_t k = ins.count;
    odin3_net_id out = node_out_net(rd, node, 1);
    if (k > ODIN3_LUT_MAX_WIDTH) {
        return malformed(rd, "a LUT with %u inputs (at most %u)", (unsigned)k,
                         (unsigned)ODIN3_LUT_MAX_WIDTH);
    }
    uint32_t obj = 0;
    if (k == 0) {
        obj = cover->len > 0 && cover->bits[0] == '1' ? 1U : 0U;
    } else if (is_buffer(cover, k)) {
        obj = net_obj(rd, odin3_pin_net(rd->module, ins.first));
    } else {
        uint64_t table[MAX_WORDS];
        cover_table(cover, k, table);
        obj = odin3_abc_result_first_lut(res) + res->n_luts;
        odin3_status st = push_u32(&res->lut_k, k);
        for (uint32_t i = 0; st == ODIN3_OK && i < k; i++) {
            st = push_u32(&res->fanins,
                          net_obj(rd, odin3_pin_net(rd->module, (odin3_pin_id){ins.first.v + i})));
        }
        for (uint32_t w = 0; st == ODIN3_OK && w < odin3_lut_words(k); w++) {
            uint64_t *slot = odin3_vec_push(&res->truth);
            st = slot == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
            if (slot != NULL) {
                *slot = table[w];
            }
        }
        st = st == ODIN3_OK ? push_u32(&res->fanin_start, (uint32_t)res->fanins.len) : st;
        st = st == ODIN3_OK ? push_u32(&res->truth_start, (uint32_t)res->truth.len) : st;
        if (st != ODIN3_OK) {
            return st;
        }
        res->n_luts++;
    }
    if (!odin3_net_valid(out)) {
        return ODIN3_OK;
    }
    *obj_slot(rd, out) = obj;
    return release_net(rd, out);
}

static odin3_status order_sops(rdctx *rd) {
    odin3_status st = init_order(rd);
    size_t done = 0;
    while (st == ODIN3_OK && rd->heap.len > 0) {
        odin3_node_id node = {odin3_abc_heap_pop(&rd->heap)};
        st = emit_sop(rd, node);
        done++;
    }
    if (st == ODIN3_OK && done < rd->sops.len) {
        return malformed(rd, "a combinational loop");
    }
    return st;
}

/* --- outputs ------------------------------------------------------------------------------- */

static odin3_status push_driver(rdctx *rd, odin3_net_id net) {
    uint32_t obj = net_obj(rd, net);
    if (obj == NO_OBJ) {
        return malformed(rd, "an undriven net '%s'",
                         odin3_net_valid(net) ? text_of(rd, odin3_net_name(rd->module, net)) : "");
    }
    return push_u32(&rd->res->co_driver, obj);
}

static odin3_status collect_outputs(rdctx *rd) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < rd->out_nets.len; i++) {
        st = push_driver(rd, (odin3_net_id){u32_at(&rd->out_nets, i)});
    }
    for (size_t j = 0; st == ODIN3_OK && j < rd->latches.len; j++) {
        odin3_node_id node = {u32_at(&rd->latches, j)};
        st = push_driver(rd, node_out_net(rd, node, 0));
    }
    return st;
}

/* The CIs and constants get their objects; the rest are NO_OBJ. */
static odin3_status seed_objects(rdctx *rd) {
    uint32_t nets = odin3_module_net_end(rd->module);
    odin3_status st = odin3_vec_reserve(&rd->obj, nets + 1U);
    for (uint32_t n = 0; st == ODIN3_OK && n < nets; n++) {
        st = push_u32(&rd->obj, NO_OBJ);
    }
    for (size_t i = 0; st == ODIN3_OK && i < rd->in_nets.len; i++) {
        uint32_t net = u32_at(&rd->in_nets, i);
        *obj_slot(rd, (odin3_net_id){net}) = FIRST_OBJ + (uint32_t)i;
    }
    return st;
}

/* Builds the result from the scratch module. */
static odin3_status build_result(rdctx *rd) {
    odin3_abc_result *res = rd->res;
    odin3_status st = read_ports(rd);
    st = st == ODIN3_OK ? read_cells(rd) : st;
    res->n_cis = (uint32_t)rd->in_nets.len;
    res->n_cos = (uint32_t)rd->out_nets.len;
    res->n_regs_out = (uint32_t)rd->latches.len;
    st = st == ODIN3_OK ? seed_objects(rd) : st;
    st = st == ODIN3_OK ? push_u32(&res->fanin_start, 0) : st;
    st = st == ODIN3_OK ? push_u32(&res->truth_start, 0) : st;
    st = st == ODIN3_OK ? read_latches(rd) : st;
    st = st == ODIN3_OK ? order_sops(rd) : st;
    st = st == ODIN3_OK ? collect_outputs(rd) : st;
    return st;
}

static void rdctx_init(rdctx *rd, odin3_abc_run *run, odin3_abc_result *res) {
    *rd = (rdctx){0};
    rd->run = run;
    rd->res = res;
    odin3_vec_init(&rd->obj, sizeof(uint32_t));
    odin3_vec_init(&rd->sops, sizeof(uint32_t));
    odin3_vec_init(&rd->latches, sizeof(uint32_t));
    odin3_vec_init(&rd->indeg, sizeof(uint32_t));
    odin3_vec_init(&rd->heap, sizeof(uint32_t));
    odin3_vec_init(&rd->sop_pos, sizeof(uint32_t));
    odin3_vec_init(&rd->in_nets, sizeof(uint32_t));
    odin3_vec_init(&rd->out_nets, sizeof(uint32_t));
}

static void rdctx_free(rdctx *rd) {
    odin3_vec_free(&rd->obj);
    odin3_vec_free(&rd->sops);
    odin3_vec_free(&rd->latches);
    odin3_vec_free(&rd->indeg);
    odin3_vec_free(&rd->heap);
    odin3_vec_free(&rd->sop_pos);
    odin3_vec_free(&rd->in_nets);
    odin3_vec_free(&rd->out_nets);
    odin3_design_destroy(rd->scratch);
}

static void log_malformed(const char *why) {
    odin3_log(ODIN3_LOG_ERROR, "abc: ABC returned a malformed network: %s", why);
}

/* The reader's own log lines name the file; the pass adds the one-line cause. */
static odin3_status open_scratch(rdctx *rd, const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        (void)snprintf(rd->why, sizeof rd->why, "no output network%s%s%s",
                       rd->run->last_line[0] != '\0' ? " (last ABC line: '" : "",
                       rd->run->last_line, rd->run->last_line[0] != '\0' ? "')" : "");
        return ODIN3_ERR_ENGINE;
    }
    (void)fclose(file);
    rd->scratch = odin3_design_create();
    if (rd->scratch == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = odin3_blif_read(rd->scratch, path);
    if (st == ODIN3_ERR_PARSE || st == ODIN3_ERR_IO) {
        (void)snprintf(rd->why, sizeof rd->why, "the output network cannot be read");
        return ODIN3_ERR_ENGINE;
    }
    if (st == ODIN3_OK) {
        rd->module = odin3_module_get(rd->scratch, odin3_design_top(rd->scratch));
        if (rd->module == NULL) {
            (void)snprintf(rd->why, sizeof rd->why, "the output network has no model");
            return ODIN3_ERR_ENGINE;
        }
    }
    return st;
}

odin3_status odin3_abc_xblif_read(odin3_abc_run *run, const char *path, odin3_abc_result *out) {
    odin3_abc_result_init(out);
    rdctx rd;
    rdctx_init(&rd, run, out);
    odin3_status st = open_scratch(&rd, path);
    st = st == ODIN3_OK ? build_result(&rd) : st;
    if (st == ODIN3_OK) {
        const odin3_abc_region *rg = run->region;
        odin3_abc_result_expect expect = {(uint32_t)rg->cis.len, (uint32_t)rg->cos.len,
                                          (uint32_t)rg->regs.len, run->opts.lut_k};
        odin3_abc_fail fail = {0};
        if (odin3_abc_result_check(out, &expect, &fail) != ODIN3_OK) {
            (void)snprintf(rd.why, sizeof rd.why, "%s", fail.text);
            st = ODIN3_ERR_ENGINE;
        }
    }
    if (st == ODIN3_ERR_ENGINE) {
        log_malformed(rd.why);
    }
    rdctx_free(&rd);
    if (st != ODIN3_OK) {
        odin3_abc_result_free(out);
    }
    return st;
}
