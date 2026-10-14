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

#define NO_OBJ UINT32_MAX /* a net without an object yet */

enum {
    FIRST_OBJ = 2,
    WHY_BYTES = 512,
    SOP_PORT_A = 0,
    SOP_PORT_Y = 1,
    SOP_COVER_PARAM = 1,
    NAME_DIGITS_MAX = 9, /* o3ci<n> with n below 10^9 */
    DECIMAL = 10,
    VARS_IN_PATTERN = 6 /* variables whose value pattern fits one 64-bit word */
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
    if (strncmp(name, prefix, plen) != 0 || name[plen] == '\0' ||
        strlen(name + plen) > NAME_DIGITS_MAX) {
        return false;
    }
    uint32_t value = 0;
    for (const char *cur = name + plen; *cur != '\0'; cur++) {
        if (*cur < '0' || *cur > '9') {
            return false;
        }
        value = value * DECIMAL + (uint32_t)(*cur - '0');
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

/* The numbers of input and output ports. */
typedef struct port_counts {
    uint32_t in;
    uint32_t out;
} port_counts;

/* The numbers of input and output ports, checked against what was sent. */
static odin3_status count_ports(rdctx *rd, port_counts *counts) {
    *counts = (port_counts){0, 0};
    uint32_t ports = odin3_module_port_count(rd->module);
    for (uint32_t port = 0; port < ports; port++) {
        bool input = port_is_input(rd, port);
        counts->in += input ? 1U : 0U;
        counts->out += input ? 0U : 1U;
    }
    const odin3_abc_region *region = rd->run->region;
    if (counts->in != region->cis.len) {
        return malformed(rd, "%u primary inputs, expected %zu", (unsigned)counts->in,
                         region->cis.len);
    }
    if (counts->out != region->cos.len) {
        return malformed(rd, "%u outputs, expected %zu", (unsigned)counts->out, region->cos.len);
    }
    return ODIN3_OK;
}

/* count zero entries in vec. */
static odin3_status fill_zeros(odin3_vec *vec, uint32_t count) {
    for (uint32_t idx = 0; idx < count; idx++) {
        odin3_status st = push_u32(vec, 0);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Port number port goes to slot o3ci<n> or o3co<n> of in_nets / out_nets. */
static odin3_status bind_port(rdctx *rd, uint32_t port) {
    bool input = port_is_input(rd, port);
    uint32_t number = 0;
    const char *name = port_name(rd, port);
    odin3_vec *slots = input ? &rd->in_nets : &rd->out_nets;
    if (!name_index(name, input ? "o3ci" : "o3co", &number) || number >= slots->len ||
        u32_at(slots, number) != 0) {
        return malformed(rd, "an unknown %s '%s'", input ? "input" : "output", name);
    }
    ((uint32_t *)slots->data)[number] = port_net(rd, port).v;
    return ODIN3_OK;
}

/* Counts the ports against what was sent, then fills in_nets / out_nets by index. */
static odin3_status read_ports(rdctx *rd) {
    port_counts counts;
    odin3_status st = count_ports(rd, &counts);
    st = st == ODIN3_OK ? fill_zeros(&rd->in_nets, counts.in) : st;
    st = st == ODIN3_OK ? fill_zeros(&rd->out_nets, counts.out) : st;
    uint32_t ports = odin3_module_port_count(rd->module);
    for (uint32_t port = 0; st == ODIN3_OK && port < ports; port++) {
        st = bind_port(rd, port);
    }
    return st;
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
        } else if (def->gran == ODIN3_GRAN_BIT) {
            return malformed(rd, "an unexpected cell '%s'", def->name);
        } else {
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
    odin3_net_id none = {0};
    return pins.count == 0 ? none : odin3_pin_net(rd->module, pins.first);
}

static uint32_t *obj_slot(rdctx *rd, odin3_net_id net) {
    return (uint32_t *)rd->obj.data + net.v;
}

/* The k of a latch whose Q net is named o3q<k>, else ODIN3_ABC_NO_REG. */
static uint32_t latch_register(const rdctx *rd, odin3_net_id qnet) {
    uint32_t number = ODIN3_ABC_NO_REG;
    if (odin3_net_valid(qnet) && odin3_net_name(rd->module, qnet) != 0 &&
        name_index(text_of(rd, odin3_net_name(rd->module, qnet)), "o3q", &number)) {
        return number;
    }
    return ODIN3_ABC_NO_REG;
}

/* Latches: Q must be o3q<k> and init 0; fills reg_from and the Q objects. */
static odin3_status read_latches(rdctx *rd) {
    odin3_abc_result *res = rd->res;
    for (size_t latch = 0; latch < rd->latches.len; latch++) {
        odin3_node_id node = {u32_at(&rd->latches, latch)};
        const odin3_value *init = odin3_node_param(rd->module, node, 0);
        if (init != NULL && init->i != 0) {
            return malformed(rd, "a latch with init %d (all were sent at 0)", (int)init->i);
        }
        odin3_net_id qnet = node_out_net(rd, node, 1);
        odin3_status st = push_u32(&res->reg_from, latch_register(rd, qnet));
        if (st != ODIN3_OK) {
            return st;
        }
        if (odin3_net_valid(qnet)) {
            *obj_slot(rd, qnet) = FIRST_OBJ + res->n_cis + (uint32_t)latch;
        }
    }
    return ODIN3_OK;
}

/* --- truth tables -------------------------------------------------------------------------- */

static const uint64_t k_var_pattern[VARS_IN_PATTERN] = {
    0xAAAAAAAAAAAAAAAAULL, 0xCCCCCCCCCCCCCCCCULL, 0xF0F0F0F0F0F0F0F0ULL,
    0xFF00FF00FF00FF00ULL, 0xFFFF0000FFFF0000ULL, 0xFFFFFFFF00000000ULL};

/* The word number word of the value of variable var over the table's minterms. */
static uint64_t var_word(uint32_t var, uint32_t word) {
    if (var < VARS_IN_PATTERN) {
        return k_var_pattern[var];
    }
    return ((word >> (var - VARS_IN_PATTERN)) & 1U) != 0 ? ~0ULL : 0ULL;
}

/* A cube row (inputs plane characters '0' '1' '-') ORed into the table. */
typedef struct cube_req {
    const uint8_t *row;
    uint32_t inputs;
    uint32_t words;
} cube_req;

static void add_cube(const cube_req *cube, uint64_t *table) {
    for (uint32_t word = 0; word < cube->words; word++) {
        uint64_t bits = ~0ULL;
        for (uint32_t var = 0; var < cube->inputs; var++) {
            if (cube->row[var] != '-') {
                uint64_t mask = var_word(var, word);
                bits &= cube->row[var] == '1' ? mask : ~mask;
            }
        }
        table[word] |= bits;
    }
}

/* The truth table of a $sop cover over inputs variables into table (odin3_lut_words words). */
static void cover_table(const odin3_value *cover, uint32_t inputs, uint64_t *table) {
    uint32_t words = odin3_lut_words(inputs);
    memset(table, 0, sizeof(uint64_t) * words);
    size_t stride = (size_t)inputs + 1U;
    bool off_set = false;
    for (size_t off = 0; off + stride <= cover->len; off += stride) {
        add_cube(&(cube_req){cover->bits + off, inputs, words}, table);
        off_set = cover->bits[off + inputs] == '0';
    }
    if (off_set) {
        for (uint32_t word = 0; word < words; word++) {
            table[word] = ~table[word];
        }
    }
    if (inputs < VARS_IN_PATTERN) {
        table[0] &= (1ULL << (1U << inputs)) - 1ULL;
    }
}

/* A one-input `1 1` cover: a connection, not a LUT. */
static bool is_buffer(const odin3_value *cover, uint32_t inputs) {
    return inputs == 1 && cover->len == 2 && cover->bits[0] == '1' && cover->bits[1] == '1';
}

/* --- ordering ------------------------------------------------------------------------------ */

/* The object of net, or NO_OBJ. */
static uint32_t net_obj(rdctx *rd, odin3_net_id net) {
    return odin3_net_valid(net) ? *obj_slot(rd, net) : NO_OBJ;
}

static odin3_pinslice sop_inputs(const rdctx *rd, odin3_node_id node) {
    return odin3_node_port(rd->module, node, SOP_PORT_A);
}

static odin3_net_id input_net(const rdctx *rd, odin3_pinslice ins, uint32_t idx) {
    return odin3_pin_net(rd->module, (odin3_pin_id){ins.first.v + idx});
}

/* Every input net of a sop node has an object or a driver. */
static odin3_status check_driven(rdctx *rd, odin3_node_id node) {
    odin3_pinslice ins = sop_inputs(rd, node);
    for (uint32_t idx = 0; idx < ins.count; idx++) {
        odin3_net_id net = input_net(rd, ins, idx);
        if (net_obj(rd, net) == NO_OBJ && odin3_net_driver_count(rd->module, net) == 0) {
            return malformed(rd, "an undriven net '%s'",
                             text_of(rd, odin3_net_name(rd->module, net)));
        }
    }
    return ODIN3_OK;
}

/* The inputs of a sop node that have no object yet. */
static uint32_t unresolved_inputs(rdctx *rd, odin3_node_id node) {
    odin3_pinslice ins = sop_inputs(rd, node);
    uint32_t count = 0;
    for (uint32_t idx = 0; idx < ins.count; idx++) {
        count += net_obj(rd, input_net(rd, ins, idx)) == NO_OBJ ? 1U : 0U;
    }
    return count;
}

static odin3_status init_positions(rdctx *rd) {
    odin3_status st = fill_zeros(&rd->sop_pos, odin3_module_node_end(rd->module));
    for (size_t sop = 0; st == ODIN3_OK && sop < rd->sops.len; sop++) {
        ((uint32_t *)rd->sop_pos.data)[u32_at(&rd->sops, sop)] = (uint32_t)sop + 1U;
    }
    return st;
}

static odin3_status init_order(rdctx *rd) {
    odin3_status st = init_positions(rd);
    for (size_t sop = 0; st == ODIN3_OK && sop < rd->sops.len; sop++) {
        odin3_node_id node = {u32_at(&rd->sops, sop)};
        st = check_driven(rd, node);
        uint32_t deg = st == ODIN3_OK ? unresolved_inputs(rd, node) : 0;
        st = st == ODIN3_OK ? push_u32(&rd->indeg, deg) : st;
        if (st == ODIN3_OK && deg == 0) {
            st = odin3_abc_heap_push(&rd->heap, node.v);
        }
    }
    return st;
}

/* Net got its object: one input of each sop reader is resolved. */
static odin3_status release_net(rdctx *rd, odin3_net_id net) {
    odin3_pinlist sinks = odin3_net_sinks(rd->module, net);
    for (uint32_t idx = 0; idx < sinks.count; idx++) {
        uint32_t reader = odin3_pin_node(rd->module, sinks.pins[idx]).v;
        uint32_t pos = u32_at(&rd->sop_pos, reader);
        if (pos == 0) {
            continue;
        }
        uint32_t *deg = (uint32_t *)rd->indeg.data + (pos - 1U);
        if (*deg == 0) {
            continue;
        }
        *deg -= 1U;
        if (*deg == 0) {
            odin3_status st = odin3_abc_heap_push(&rd->heap, reader);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

/* --- emitting a LUT ------------------------------------------------------------------------ */

/* Appends the fanins, table and offsets of a LUT with the given inputs to the result. */
static odin3_status append_lut(rdctx *rd, odin3_pinslice ins, const odin3_value *cover) {
    odin3_abc_result *res = rd->res;
    uint32_t inputs = ins.count;
    uint64_t table[ODIN3_LUT_MAX_WORDS];
    cover_table(cover, inputs, table);
    odin3_status st = push_u32(&res->lut_k, inputs);
    for (uint32_t idx = 0; st == ODIN3_OK && idx < inputs; idx++) {
        st = push_u32(&res->fanins, net_obj(rd, input_net(rd, ins, idx)));
    }
    for (uint32_t word = 0; st == ODIN3_OK && word < odin3_lut_words(inputs); word++) {
        uint64_t *slot = odin3_vec_push(&res->truth);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = table[word];
    }
    st = st == ODIN3_OK ? push_u32(&res->fanin_start, (uint32_t)res->fanins.len) : st;
    st = st == ODIN3_OK ? push_u32(&res->truth_start, (uint32_t)res->truth.len) : st;
    res->n_luts += st == ODIN3_OK ? 1U : 0U;
    return st;
}

/* The object a sop node's output stands for: a constant, an input, or a new LUT (*obj). */
static odin3_status sop_object(rdctx *rd, odin3_node_id node, uint32_t *obj) {
    const odin3_value *cover = odin3_node_param(rd->module, node, SOP_COVER_PARAM);
    odin3_pinslice ins = sop_inputs(rd, node);
    if (ins.count > ODIN3_LUT_MAX_WIDTH) {
        return malformed(rd, "a LUT with %u inputs (at most %u)", (unsigned)ins.count,
                         (unsigned)ODIN3_LUT_MAX_WIDTH);
    }
    if (ins.count == 0) {
        *obj = cover->len > 0 && cover->bits[0] == '1' ? 1U : 0U;
        return ODIN3_OK;
    }
    if (is_buffer(cover, ins.count)) {
        *obj = net_obj(rd, input_net(rd, ins, 0));
        return ODIN3_OK;
    }
    *obj = odin3_abc_result_first_lut(rd->res) + rd->res->n_luts;
    return append_lut(rd, ins, cover);
}

/* Maps the sop node's output net to its object and releases the net's readers. */
static odin3_status emit_sop(rdctx *rd, odin3_node_id node) {
    uint32_t obj = 0;
    odin3_status st = sop_object(rd, node, &obj);
    odin3_net_id out = node_out_net(rd, node, SOP_PORT_Y);
    if (st != ODIN3_OK || !odin3_net_valid(out)) {
        return st;
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
    for (size_t out = 0; st == ODIN3_OK && out < rd->out_nets.len; out++) {
        st = push_driver(rd, (odin3_net_id){u32_at(&rd->out_nets, out)});
    }
    for (size_t latch = 0; st == ODIN3_OK && latch < rd->latches.len; latch++) {
        odin3_node_id node = {u32_at(&rd->latches, latch)};
        st = push_driver(rd, node_out_net(rd, node, 0));
    }
    return st;
}

/* The CIs get their objects; every other net starts as NO_OBJ. */
static odin3_status seed_objects(rdctx *rd) {
    uint32_t nets = odin3_module_net_end(rd->module);
    odin3_status st = odin3_vec_reserve(&rd->obj, (size_t)nets + 1U);
    for (uint32_t net = 0; st == ODIN3_OK && net < nets; net++) {
        st = push_u32(&rd->obj, NO_OBJ);
    }
    for (size_t in = 0; st == ODIN3_OK && in < rd->in_nets.len; in++) {
        *obj_slot(rd, (odin3_net_id){u32_at(&rd->in_nets, in)}) = FIRST_OBJ + (uint32_t)in;
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
    return st == ODIN3_OK ? collect_outputs(rd) : st;
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
        const char *last = rd->run->last_line;
        (void)snprintf(rd->why, sizeof rd->why, "no output network%s%s%s",
                       last[0] != '\0' ? " (last ABC line: '" : "", last,
                       last[0] != '\0' ? "')" : "");
        return ODIN3_ERR_ENGINE;
    }
    (void)fclose(file);
    rd->scratch = odin3_design_create();
    if (rd->scratch == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = odin3_blif_read(rd->scratch, path);
    if (st == ODIN3_ERR_PARSE) {
        (void)snprintf(rd->why, sizeof rd->why, "the output network cannot be read");
        return ODIN3_ERR_ENGINE;
    }
    if (st == ODIN3_ERR_IO) {
        /* the reader's lexer also reports a failed buffer allocation this way */
        odin3_log(ODIN3_LOG_ERROR, "abc: %s: cannot read the output network", path);
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
        const odin3_abc_region *region = run->region;
        odin3_abc_result_expect expect = {(uint32_t)region->cis.len, (uint32_t)region->cos.len,
                                          (uint32_t)region->regs.len, run->opts.lut_k};
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
