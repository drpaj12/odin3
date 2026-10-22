/*
 * abc_fake_engine.c — the ABC engine test double (see abc_fake_engine.h).
 */
#include "abc_fake_engine.h"

#include "backends/blif/isop.h"
#include "frontends/blif/reader.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MAX_NODES = 4096,
    MAX_PORTS = 256,
    MAX_SOP_INPUTS = 64,
    NAME_BYTES = 40,
    CMD_BYTES = 8192,
    GENLIB_BYTES = 65536
};

#define LIT_NONE UINT32_MAX

enum { K_CONST = 0, K_PI, K_LATCH, K_AND };

typedef struct fnode {
    uint8_t kind;
    uint32_t fan0, fan1; /* AND: fanin literals; LATCH: a = the D literal (set after reading) */
    uint32_t rep;        /* LATCH: replacement literal after scorr, LIT_NONE when kept */
    char name[NAME_BYTES];
} fnode;

typedef struct fport {
    char name[NAME_BYTES];
    uint32_t lit;
} fport;

typedef struct fake {
    fnode nodes[MAX_NODES];
    uint32_t n_nodes;
    fport pis[MAX_PORTS];
    uint32_t n_pis;
    fport pos[MAX_PORTS];
    uint32_t n_pos;
    uint32_t latches[MAX_PORTS]; /* node indices in file order */
    uint32_t n_latches;
    int32_t const_lut; /* the LUT (AND index) that `const` makes constant, -1 none */
    int const_value;
    odin3_abc_text_fn text;
    void *user;
    uint32_t opens;
    char commands[CMD_BYTES];
    char fail_word[NAME_BYTES];
    bool fail_set;
    char extra[NAME_BYTES * 2]; /* a command run just before every write_hie */
    odin3_abc_fail fail;
    bool open;
    int map_mode;               /* odin3_abc_fake_name_map */
    bool aig_loaded;            /* the network came from load_aig (the in-memory exchange) */
    char genlib[GENLIB_BYTES];  /* the last text load_genlib got */
    char gate_and2[NAME_BYTES]; /* odin3_abc_fake_gate_names; "" means the default */
    char gate_inv[NAME_BYTES];
} fake;

static fake fk;

/* --- literals ------------------------------------------------------------------------------ */

static uint32_t new_node(uint8_t kind, const char *name) {
    if (fk.n_nodes >= MAX_NODES) {
        abort();
    }
    fnode *node = &fk.nodes[fk.n_nodes];
    memset(node, 0, sizeof *node);
    node->kind = kind;
    node->rep = LIT_NONE;
    (void)snprintf(node->name, NAME_BYTES, "%s", name);
    return fk.n_nodes++;
}

static uint32_t mk_and(uint32_t fan0, uint32_t fan1) {
    if (fan0 == 0 || fan1 == 0 || fan0 == (fan1 ^ 1U)) {
        return 0;
    }
    if (fan0 == 1U) {
        return fan1;
    }
    if (fan1 == 1U || fan0 == fan1) {
        return fan0;
    }
    uint32_t node = new_node(K_AND, "");
    fk.nodes[node].fan0 = fan0 < fan1 ? fan0 : fan1;
    fk.nodes[node].fan1 = fan0 < fan1 ? fan1 : fan0;
    return 2U * node;
}

static uint32_t mk_or(uint32_t fan0, uint32_t fan1) {
    return mk_and(fan0 ^ 1U, fan1 ^ 1U) ^ 1U;
}

/* Follows scorr's replacements. */
static uint32_t resolve(uint32_t lit) {
    for (;;) {
        const fnode *node = &fk.nodes[lit >> 1];
        if (node->kind != K_LATCH || node->rep == LIT_NONE) {
            return lit;
        }
        lit = node->rep ^ (lit & 1U);
    }
}

/* --- reading ------------------------------------------------------------------------------- */

static uint32_t cover_literal(const odin3_value *cover, const uint32_t *ins, uint32_t inputs) {
    size_t stride = (size_t)inputs + 1U;
    uint32_t sum = 0;
    bool off_set = false;
    for (size_t off = 0; off + stride <= cover->len; off += stride) {
        uint32_t cube = 1U;
        for (uint32_t j = 0; j < inputs; j++) {
            if (cover->bits[off + j] != '-') {
                cube = mk_and(cube, cover->bits[off + j] == '1' ? ins[j] : ins[j] ^ 1U);
            }
        }
        sum = mk_or(sum, cube);
        off_set = cover->bits[off + inputs] == '0';
    }
    return off_set ? sum ^ 1U : sum;
}

static bool is_type(const odin3_design *design, const odin3_module *mod, odin3_node_id node,
                    const char *name) {
    const odin3_celltype_def *def = odin3_celltype_get(design, odin3_node_type(mod, node));
    return def != NULL && strcmp(def->name, name) == 0;
}

static const char *net_name(const odin3_design *design, const odin3_module *mod, odin3_net_id net) {
    return odin3_strtab_get(odin3_design_strtab(design), odin3_net_name(mod, net));
}

/* The net on the first pin of a port. */
static odin3_net_id port_net0(const odin3_module *mod, odin3_node_id node, uint32_t port) {
    odin3_pinslice pins = odin3_node_port(mod, node, port);
    odin3_net_id none = {0};
    return pins.count > 0 ? odin3_pin_net(mod, pins.first) : none;
}

/* The scratch design being converted and the literal of each of its nets. */
typedef struct rctx {
    odin3_design *design;
    const odin3_module *mod;
    uint32_t *net_lit; /* per net: its literal, LIT_NONE until known */
    bool *done;        /* per node: its $sop is resolved */
    uint32_t nodes;
} rctx;

/* One resolution round over the $sop nodes; true when it resolved any. */
static bool resolve_sops(rctx *rc) {
    bool progress = false;
    for (uint32_t id = 1; id < rc->nodes; id++) {
        odin3_node_id node = {id};
        if (rc->done[id] || !odin3_node_live(rc->mod, node) ||
            !is_type(rc->design, rc->mod, node, "$sop")) {
            continue;
        }
        odin3_pinslice ins = odin3_node_port(rc->mod, node, 0);
        uint32_t lits[MAX_SOP_INPUTS];
        bool ready = ins.count <= MAX_SOP_INPUTS;
        for (uint32_t j = 0; ready && j < ins.count; j++) {
            odin3_net_id net = odin3_pin_net(rc->mod, (odin3_pin_id){ins.first.v + j});
            lits[j] = rc->net_lit[net.v];
            ready = lits[j] != LIT_NONE;
        }
        if (ready) {
            rc->net_lit[port_net0(rc->mod, node, 1).v] =
                cover_literal(odin3_node_param(rc->mod, node, 1), lits, ins.count);
            rc->done[id] = true;
            progress = true;
        }
    }
    return progress;
}

static bool port_is_in(const odin3_module *mod, uint32_t index) {
    return is_type(odin3_module_design(mod), mod, odin3_module_port(mod, index), "$port_in");
}

static void add_port_name(fport *dst, const char *name) {
    (void)snprintf(dst->name, NAME_BYTES, "%s", name);
}

static const char *port_wire_name(const rctx *rc, uint32_t index) {
    return odin3_strtab_get(odin3_design_strtab(rc->design),
                            odin3_wire_name(rc->mod, odin3_module_port_wire(rc->mod, index)));
}

static odin3_net_id port_wire_net(const rctx *rc, uint32_t index) {
    return odin3_wire_net(rc->mod, odin3_module_port_wire(rc->mod, index), 0);
}

/* Forgets the network (a new `read` replaces it); the counters and commands stay. */
static void reset_network(void) {
    fk.n_nodes = 0;
    fk.n_pis = 0;
    fk.n_pos = 0;
    fk.n_latches = 0;
    fk.const_lut = -1;
    fk.aig_loaded = false;
}

/* The inputs become PIs, the flip-flops latches. */
static void make_sources(rctx *rc) {
    for (uint32_t port = 0; port < odin3_module_port_count(rc->mod); port++) {
        if (port_is_in(rc->mod, port)) {
            const char *name = port_wire_name(rc, port);
            rc->net_lit[port_wire_net(rc, port).v] = 2U * new_node(K_PI, name);
            add_port_name(&fk.pis[fk.n_pis++], name);
        }
    }
    for (uint32_t id = 1; id < rc->nodes; id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(rc->mod, node) && is_type(rc->design, rc->mod, node, "$_FF_")) {
            odin3_net_id qnet = port_net0(rc->mod, node, 1);
            uint32_t idx = new_node(K_LATCH, net_name(rc->design, rc->mod, qnet));
            rc->net_lit[qnet.v] = 2U * idx;
            fk.latches[fk.n_latches++] = idx;
        }
    }
}

/* The latches' D literals and the outputs' drivers, once every $sop is resolved. */
static void make_sinks(const rctx *rc) {
    uint32_t latch = 0;
    for (uint32_t id = 1; id < rc->nodes; id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(rc->mod, node) && is_type(rc->design, rc->mod, node, "$_FF_")) {
            fk.nodes[fk.latches[latch++]].fan0 = rc->net_lit[port_net0(rc->mod, node, 0).v];
        }
    }
    for (uint32_t port = 0; port < odin3_module_port_count(rc->mod); port++) {
        if (!port_is_in(rc->mod, port)) {
            add_port_name(&fk.pos[fk.n_pos], port_wire_name(rc, port));
            fk.pos[fk.n_pos++].lit = rc->net_lit[port_wire_net(rc, port).v];
        }
    }
}

/* Builds the AIG from the BLIF at path. */
static odin3_status read_blif(const char *path) {
    reset_network();
    odin3_design *design = odin3_design_create();
    odin3_status st = odin3_blif_read(design, path);
    if (st != ODIN3_OK) {
        odin3_design_destroy(design);
        return st;
    }
    const odin3_module *mod = odin3_module_get(design, odin3_design_top(design));
    rctx rc = {design, mod, NULL, NULL, odin3_module_node_end(mod)};
    uint32_t nets = odin3_module_net_end(mod);
    rc.net_lit = malloc(sizeof(uint32_t) * (nets + 1U));
    rc.done = calloc(rc.nodes + 1U, sizeof(bool));
    memset(rc.net_lit, 0xFF, sizeof(uint32_t) * (nets + 1U));
    new_node(K_CONST, "const0");
    make_sources(&rc);
    while (resolve_sops(&rc)) {
    }
    make_sinks(&rc);
    free(rc.net_lit);
    free(rc.done);
    odin3_design_destroy(design);
    return ODIN3_OK;
}

/* --- scorr --------------------------------------------------------------------------------- */

static void scorr(void) {
    for (uint32_t i = 0; i < fk.n_latches; i++) {
        uint32_t me = fk.latches[i];
        uint32_t dnet = resolve(fk.nodes[me].fan0);
        if (dnet == 0 || dnet == 2U * me) {
            fk.nodes[me].rep = 0;
            continue;
        }
        for (uint32_t j = 0; j < i; j++) {
            uint32_t other = fk.latches[j];
            if (fk.nodes[other].rep == LIT_NONE && resolve(fk.nodes[other].fan0) == dnet) {
                fk.nodes[me].rep = 2U * other;
                break;
            }
        }
    }
}

/* --- writing ------------------------------------------------------------------------------- */

static void lit_name(uint32_t lit, char *out) {
    const fnode *node = &fk.nodes[lit >> 1];
    if (node->kind == K_AND) {
        (void)snprintf(out, NAME_BYTES, "n%u", (unsigned)(lit >> 1));
    } else {
        (void)snprintf(out, NAME_BYTES, "%s", node->name);
    }
}

/* Marks the AND cone of lit. */
static void mark_cone(uint32_t lit, bool *used) {
    uint32_t stack[3 * MAX_NODES];
    uint32_t top = 0;
    stack[top++] = resolve(lit) >> 1;
    while (top > 0) {
        uint32_t id = stack[--top];
        if (used[id] || fk.nodes[id].kind != K_AND) {
            continue;
        }
        used[id] = true;
        stack[top++] = resolve(fk.nodes[id].fan0) >> 1;
        stack[top++] = resolve(fk.nodes[id].fan1) >> 1;
    }
}

static void put_const_names(FILE *out, const char *name, uint32_t lit) {
    (void)fprintf(out, ".names %s\n%s", name, lit == 1U ? "1\n" : "");
}

/* A name for the value of literal lit: the node's name, an inverter, or a constant net. */
static void drive(FILE *out, uint32_t lit, const char *tag, char *name) {
    lit = resolve(lit);
    if ((lit >> 1) == 0) {
        (void)snprintf(name, NAME_BYTES, "%s_c", tag);
        put_const_names(out, name, lit);
    } else if ((lit & 1U) != 0) {
        char base[NAME_BYTES];
        lit_name(lit & ~1U, base);
        (void)snprintf(name, NAME_BYTES, "%s_i", tag);
        (void)fprintf(out, ".names %s %s\n0 1\n", base, name);
    } else {
        lit_name(lit, name);
    }
}

/* The truth table (bits 0..3) of AND with complemented fanins. */
static uint64_t and_table(const uint32_t *fanins) {
    uint64_t table = 0;
    for (uint32_t row = 0; row < 4U; row++) {
        bool left = (((row >> 0U) & 1U) != 0) != ((fanins[0] & 1U) != 0);
        bool right = (((row >> 1U) & 1U) != 0) != ((fanins[1] & 1U) != 0);
        table |= (uint64_t)(left && right ? 1U : 0U) << row;
    }
    return table;
}

/* An AND node written as a LUT, and its number among the written LUTs. */
typedef struct lut_ref {
    uint32_t id;
    int index;
} lut_ref;

static void write_lut(FILE *out, const lut_ref *ref) {
    uint32_t id = ref->id;
    const uint32_t fanins[2] = {resolve(fk.nodes[id].fan0), resolve(fk.nodes[id].fan1)};
    char name0[NAME_BYTES];
    char name1[NAME_BYTES];
    lit_name(fanins[0] & ~1U, name0);
    lit_name(fanins[1] & ~1U, name1);
    uint64_t table = and_table(fanins);
    if (fk.const_lut == ref->index) {
        table = fk.const_value != 0 ? 0xFULL : 0ULL;
    }
    odin3_strbuf cover;
    odin3_strbuf_init(&cover);
    uint32_t rows = 0;
    int constant = -1;
    (void)odin3_blif_isop(&table, 2, &cover, &rows, &constant);
    if (constant >= 0) {
        (void)fprintf(out, ".names n%u\n%s", (unsigned)id, constant == 1 ? "1\n" : "");
    } else {
        (void)fprintf(out, ".names %s %s n%u\n%.*s", name0, name1, (unsigned)id, (int)cover.len,
                      cover.data);
    }
    odin3_strbuf_free(&cover);
}

static void write_network(FILE *out) {
    static bool used[MAX_NODES];
    memset(used, 0, sizeof used);
    for (uint32_t i = 0; i < fk.n_pos; i++) {
        mark_cone(fk.pos[i].lit, used);
    }
    for (uint32_t i = 0; i < fk.n_latches; i++) {
        if (fk.nodes[fk.latches[i]].rep == LIT_NONE) {
            mark_cone(fk.nodes[fk.latches[i]].fan0, used);
        }
    }
    (void)fprintf(out, ".model o3abc\n.inputs");
    for (uint32_t i = 0; i < fk.n_pis; i++) {
        (void)fprintf(out, " %s", fk.pis[i].name);
    }
    (void)fprintf(out, "\n.outputs");
    for (uint32_t i = 0; i < fk.n_pos; i++) {
        (void)fprintf(out, " %s", fk.pos[i].name);
    }
    (void)fprintf(out, "\n.names const0\n");
    int lut_index = 0;
    for (uint32_t id = 1; id < fk.n_nodes; id++) {
        if (used[id]) {
            write_lut(out, &(lut_ref){id, lut_index++});
        }
    }
    char name[NAME_BYTES];
    char tag[NAME_BYTES];
    for (uint32_t i = 0; i < fk.n_latches; i++) {
        const fnode *latch = &fk.nodes[fk.latches[i]];
        if (latch->rep == LIT_NONE) {
            (void)snprintf(tag, NAME_BYTES, "d%u", (unsigned)i);
            drive(out, latch->fan0, tag, name);
            (void)fprintf(out, ".latch %s %s 0\n", name, latch->name);
        }
    }
    for (uint32_t i = 0; i < fk.n_pos; i++) {
        (void)snprintf(tag, NAME_BYTES, "%s", fk.pos[i].name);
        drive(out, fk.pos[i].lit, tag, name);
        (void)fprintf(out, ".names %s %s\n1 1\n", name, fk.pos[i].name);
    }
    (void)fprintf(out, ".end\n");
}

static odin3_status write_hie(const char *path) {
    FILE *out = fopen(path, "w");
    if (out == NULL) {
        return ODIN3_ERR_IO;
    }
    write_network(out);
    return fclose(out) == 0 ? ODIN3_OK : ODIN3_ERR_IO;
}

/* --- commands ------------------------------------------------------------------------------ */

/* The first quoted string after position at in line, into out; the position after it. */
static const char *quoted(const char *at, char *out, size_t cap) {
    const char *open = strchr(at, '"');
    const char *close = open == NULL ? NULL : strchr(open + 1, '"');
    if (close == NULL || (size_t)(close - open) >= cap) {
        out[0] = '\0';
        return at + strlen(at);
    }
    memcpy(out, open + 1, (size_t)(close - open - 1));
    out[close - open - 1] = '\0';
    return close + 1;
}

static void record(const char *command) {
    size_t used = strlen(fk.commands);
    if (used + strlen(command) + 2U < CMD_BYTES) {
        (void)snprintf(fk.commands + used, CMD_BYTES - used, "%s\n", command);
    }
}

/* `const <i> [value]`: LUT i of the next write becomes constant 0 (or 1 with the value 1). */
static void set_const(const char *args) {
    char *end = NULL;
    long idx = strtol(args, &end, 10);
    fk.const_lut = end != args ? (int)idx : -1;
    const char *value_text = end;
    long value = strtol(value_text, &end, 10);
    fk.const_value = end != value_text ? (int)value : 0;
}

/* The commands that only change the network: `scorr` and `const <i> [value]`. */
static void run_simple(const char *command, const char *first) {
    if (strcmp(first, "scorr") == 0) {
        scorr();
    } else if (strcmp(first, "const") == 0) {
        set_const(command + strlen("const"));
    }
}

/* The injected extra command (odin3_abc_fake_extra), run just before every write. */
static void run_extra(void) {
    char first[NAME_BYTES] = "";
    (void)sscanf(fk.extra, "%39s", first);
    run_simple(fk.extra, first);
}

static odin3_status do_command(const char *command) {
    char first[NAME_BYTES] = "";
    (void)sscanf(command, "%39s", first);
    if (fk.fail_set && strcmp(first, fk.fail_word) == 0) {
        return ODIN3_ERR_ENGINE;
    }
    char path[CMD_BYTES / 8];
    if (strcmp(first, "read") == 0) {
        (void)quoted(command, path, sizeof path);
        return read_blif(path);
    }
    if (strcmp(first, "write_hie") == 0) {
        run_extra();
        const char *rest = quoted(command, path, sizeof path);
        (void)quoted(rest, path, sizeof path);
        return write_hie(path);
    }
    run_simple(command, first);
    return ODIN3_OK;
}

/* --- the engine table ---------------------------------------------------------------------- */

static odin3_status fake_open(void *self, odin3_abc_text_fn text, void *user,
                              odin3_abc_fail *fail) {
    (void)self;
    (void)fail;
    fk.text = text;
    fk.user = user;
    fk.opens++;
    fk.open = true;
    return ODIN3_OK;
}

static odin3_status fake_exec(void *self, const char *command, odin3_abc_fail *fail) {
    (void)self;
    record(command);
    if (fk.text != NULL) {
        char line[CMD_BYTES / 4];
        (void)snprintf(line, sizeof line, "fake: %s", command);
        fk.text(fk.user, line);
    }
    odin3_status st = do_command(command);
    if (st == ODIN3_ERR_ENGINE && fk.fail_set) {
        *fail = fk.fail;
    } else if (st != ODIN3_OK) {
        fail->kind = ODIN3_ABC_FAIL_COMMAND;
        fail->code = 1;
        (void)snprintf(fail->text, sizeof fail->text, "fake command failed");
        st = ODIN3_ERR_ENGINE;
    }
    return st;
}

static odin3_status fake_fail_result(odin3_abc_fail *fail, const char *text) {
    fail->kind = ODIN3_ABC_FAIL_RESULT;
    fail->code = 0;
    (void)snprintf(fail->text, sizeof fail->text, "%s", text);
    return ODIN3_ERR_ENGINE;
}

/* --- the in-memory exchange ---------------------------------------------------------------- */

enum {
    MINI_NULL = 0x7FFFFFFF,
    MINI_NULL2 = 0x7FFFFFFE,
    MAX_OBJS = 2 * MAX_NODES + 4 * MAX_PORTS,
    LUT_SIZE = 2,
    TABLE_FULL = 0xF,
    TABLE_NOT = 0x1,
    WORD_BITS = 32
};

/* The two literals of loaded object obj. */
static int32_t aig_first(const odin3_abc_aig *aig, uint32_t obj) {
    return aig->objs[(size_t)2 * obj];
}

static int32_t aig_second(const odin3_abc_aig *aig, uint32_t obj) {
    return aig->objs[((size_t)2 * obj) + 1U];
}

/* n of a name "c<n>" (the PIs of a loaded AIG). */
static int32_t name_index(const char *name) {
    return (int32_t)strtol(name + 1, NULL, 10);
}

/* The loaded MiniAIG object (index) -> the fake node, and the literal converted. */
static uint32_t aig_node[MAX_OBJS];
static uint32_t aig_obj_of_node[MAX_NODES]; /* the loaded object of a node, UINT32_MAX for none */
static int32_t lut_lit[MAX_OBJS];           /* per emitted LUT: its loaded AIG literal or -1 */
static uint32_t n_lut_lit;

static uint32_t aig_lit(int32_t lit) {
    return 2U * aig_node[(uint32_t)lit >> 1] + ((uint32_t)lit & 1U);
}

/* Reads the PIs and ANDs of aig; returns the first object that is a PO (n_objs when none). */
static void load_nodes(const odin3_abc_aig *aig, uint32_t pis) {
    uint32_t real = pis - aig->n_regs;
    uint32_t pi = 0;
    memset(aig_obj_of_node, 0xFF, sizeof aig_obj_of_node);
    aig_node[0] = new_node(K_CONST, "const0");
    for (uint32_t obj = 1; obj < aig->n_objs; obj++) {
        int32_t lit0 = aig_first(aig, obj);
        int32_t lit1 = aig_second(aig, obj);
        char name[NAME_BYTES];
        if (lit0 == MINI_NULL) {
            (void)snprintf(name, NAME_BYTES, "c%u", (unsigned)pi);
            aig_node[obj] = new_node(pi < real ? K_PI : K_LATCH, name);
            if (pi < real) {
                add_port_name(&fk.pis[fk.n_pis++], name);
            } else {
                fk.latches[fk.n_latches++] = aig_node[obj];
            }
            pi++;
        } else if (lit1 != MINI_NULL) {
            uint32_t node = new_node(K_AND, "");
            fk.nodes[node].fan0 = aig_lit(lit0);
            fk.nodes[node].fan1 = aig_lit(lit1);
            aig_node[obj] = node;
            aig_obj_of_node[node] = obj;
        }
    }
}

static odin3_status fake_load_aig(void *self, const odin3_abc_aig *aig, odin3_abc_fail *fail) {
    (void)self;
    uint32_t pis = 0;
    uint32_t pos = 0;
    for (uint32_t obj = 1; obj < aig->n_objs; obj++) {
        pis += aig_first(aig, obj) == MINI_NULL ? 1U : 0U;
        pos += aig_first(aig, obj) != MINI_NULL && aig_second(aig, obj) == MINI_NULL ? 1U : 0U;
    }
    if (aig->n_objs >= MAX_OBJS || pis > MAX_PORTS || pos > MAX_PORTS || aig->n_regs > pis ||
        aig->n_regs > pos) {
        return fake_fail_result(fail, "the fake engine cannot hold this AIG");
    }
    reset_network();
    load_nodes(aig, pis);
    uint32_t po = 0;
    for (uint32_t obj = 1; obj < aig->n_objs; obj++) {
        if (aig_first(aig, obj) == MINI_NULL || aig_second(aig, obj) != MINI_NULL) {
            continue;
        }
        uint32_t lit = aig_lit(aig_first(aig, obj));
        if (po < pos - aig->n_regs) {
            (void)snprintf(fk.pos[fk.n_pos].name, NAME_BYTES, "co%u", (unsigned)po);
            fk.pos[fk.n_pos++].lit = lit;
        } else {
            fk.nodes[fk.latches[po - (pos - aig->n_regs)]].fan0 = lit;
        }
        po++;
    }
    fk.aig_loaded = true;
    return ODIN3_OK;
}

/* The MiniLUT of the network, in static arrays. */
static int32_t out_fanins[MAX_OBJS * LUT_SIZE];
static uint32_t out_truths[MAX_OBJS];
static int32_t out_ci_from[MAX_PORTS * 2];
static uint32_t out_objs;
static uint32_t out_node[MAX_NODES]; /* node -> MiniLUT object (PIs, kept latches, used ANDs) */

static uint32_t out_push(const int32_t *fanins, uint32_t table) {
    uint32_t obj = out_objs++;
    for (uint32_t slot = 0; slot < LUT_SIZE; slot++) {
        out_fanins[((size_t)obj * LUT_SIZE) + slot] = fanins != NULL ? fanins[slot] : MINI_NULL;
    }
    out_truths[obj] = table;
    return obj;
}

/* A 2-input table (bits 0..3) replicated over the word. */
static uint32_t replicate(uint64_t table) {
    uint32_t word = 0;
    for (uint32_t at = 0; at < WORD_BITS; at += 4U) {
        word |= (uint32_t)(table & TABLE_FULL) << at;
    }
    return word;
}

static void emit_ands(const bool *used) {
    int lut_index = 0;
    for (uint32_t id = 1; id < fk.n_nodes; id++) {
        if (!used[id]) {
            continue;
        }
        const uint32_t fan[2] = {resolve(fk.nodes[id].fan0), resolve(fk.nodes[id].fan1)};
        uint64_t table = and_table(fan);
        if (fk.const_lut == lut_index++) {
            table = fk.const_value != 0 ? TABLE_FULL : 0U;
        }
        int32_t objs[LUT_SIZE] = {(int32_t)out_node[fan[0] >> 1], (int32_t)out_node[fan[1] >> 1]};
        out_node[id] = out_push(objs, replicate(table));
        lut_lit[n_lut_lit++] =
            aig_obj_of_node[id] == UINT32_MAX ? -1 : (int32_t)(2U * aig_obj_of_node[id]);
    }
}

/* One PO: an inverter LUT first when the driver is complemented and not a constant. */
static void emit_po(uint32_t lit) {
    lit = resolve(lit);
    uint32_t drv = out_node[lit >> 1];
    if ((lit >> 1) == 0) {
        drv = lit & 1U;
    } else if ((lit & 1U) != 0) {
        int32_t one[LUT_SIZE] = {(int32_t)drv, MINI_NULL};
        drv = out_push(one, replicate(TABLE_NOT));
        lut_lit[n_lut_lit++] = -1;
    }
    int32_t fan[LUT_SIZE] = {(int32_t)drv, MINI_NULL2};
    (void)out_push(fan, 0);
}

/* The PIs (real inputs first, in the order loaded), then the kept latches; ci_from is the index
 * each was loaded with (its name is c<n>). */
static void emit_inputs(void) {
    uint32_t pi = 0;
    for (uint32_t node = 1; node < fk.n_nodes; node++) {
        if (fk.nodes[node].kind == K_PI) {
            out_ci_from[pi++] = name_index(fk.nodes[node].name);
            out_node[node] = out_push(NULL, 0);
        }
    }
    for (uint32_t i = 0; i < fk.n_latches; i++) {
        const fnode *latch = &fk.nodes[fk.latches[i]];
        if (latch->rep == LIT_NONE) {
            out_ci_from[pi++] = name_index(latch->name);
            out_node[fk.latches[i]] = out_push(NULL, 0);
        }
    }
}

static odin3_status fake_get_luts(void *self, odin3_abc_luts *luts, odin3_abc_fail *fail) {
    (void)self;
    (void)fail;
    run_extra();
    static bool used[MAX_NODES];
    memset(used, 0, sizeof used);
    for (uint32_t i = 0; i < fk.n_pos; i++) {
        mark_cone(fk.pos[i].lit, used);
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < fk.n_latches; i++) {
        if (fk.nodes[fk.latches[i]].rep == LIT_NONE) {
            mark_cone(fk.nodes[fk.latches[i]].fan0, used);
            kept++;
        }
    }
    out_objs = 0;
    n_lut_lit = 0;
    (void)out_push(NULL, 0);          /* constant 0 */
    (void)out_push(NULL, UINT32_MAX); /* constant 1 */
    emit_inputs();
    emit_ands(used);
    for (uint32_t i = 0; i < fk.n_pos; i++) {
        emit_po(fk.pos[i].lit);
    }
    for (uint32_t i = 0; i < fk.n_latches; i++) {
        if (fk.nodes[fk.latches[i]].rep == LIT_NONE) {
            emit_po(fk.nodes[fk.latches[i]].fan0);
        }
    }
    *luts = (odin3_abc_luts){out_fanins, out_truths, out_ci_from, out_objs, kept, LUT_SIZE};
    return ODIN3_OK;
}

static int32_t out_lits[MAX_OBJS];

/* The map of mode FAKE_MAP_LITS: each LUT of the aligned MiniLUT (they keep the order of the
 * result's) gets the literal of the loaded AND it was made from, -1 for an inverter LUT. */
static void fill_lits(const odin3_abc_luts *aligned) {
    uint32_t next = 0;
    for (uint32_t obj = 0; obj < aligned->n_objs; obj++) {
        const int32_t *fan = aligned->fanins + ((size_t)aligned->lut_size * obj);
        bool is_lut = obj >= 2U && fan[0] != MINI_NULL && fan[1] != MINI_NULL2;
        out_lits[obj] = is_lut && next < n_lut_lit ? lut_lit[next] : -1;
        next += is_lut ? 1U : 0U;
    }
}

static odin3_status fake_name_map(void *self, const odin3_abc_luts *aligned, const int32_t **lits,
                                  odin3_abc_fail *fail) {
    (void)self;
    if (fk.map_mode == ODIN3_FAKE_MAP_LITS && aligned->n_objs < MAX_OBJS) {
        fill_lits(aligned);
        *lits = out_lits;
        return ODIN3_OK;
    }
    if (fk.map_mode == ODIN3_FAKE_MAP_COMMAND) {
        fail->kind = ODIN3_ABC_FAIL_COMMAND;
        fail->code = 1;
        (void)snprintf(fail->text, sizeof fail->text, "the fake name map command failed");
        return ODIN3_ERR_ENGINE;
    }
    return fake_fail_result(fail, "the fake engine has no name map");
}

/* --- gate mapping (ABI 5) ------------------------------------------------------------------ */

static odin3_status fake_load_genlib(void *self, const char *text, odin3_abc_fail *fail) {
    (void)self;
    if (text == NULL || strlen(text) >= GENLIB_BYTES) {
        return fake_fail_result(fail, "the fake engine cannot hold this genlib");
    }
    (void)snprintf(fk.genlib, GENLIB_BYTES, "%s", text);
    return ODIN3_OK;
}

enum { GATE_AND2 = 0, GATE_INV = 1, FAKE_GATES = 2, TABLE_AND = 0x8 };

static int32_t out_gate[MAX_OBJS]; /* per object: index into out_names, -1 none */
static const char *out_names[FAKE_GATES];
static int32_t name_slot[FAKE_GATES]; /* GATE_* -> index into out_names, -1 not used yet */
static uint32_t n_out_names;
static uint32_t inv_obj[MAX_NODES]; /* source node -> its inverter object, UINT32_MAX none */

static const char *fake_gate_name(int which) {
    const char *set = which == GATE_AND2 ? fk.gate_and2 : fk.gate_inv;
    if (set[0] != '\0') {
        return set;
    }
    return which == GATE_AND2 ? "AND2" : "INV";
}

/* Pushes a gate object of kind which; its name gets an index on first use. */
static uint32_t push_gate(int which, const int32_t *fanins, uint32_t table) {
    if (name_slot[which] < 0) {
        name_slot[which] = (int32_t)n_out_names;
        out_names[n_out_names++] = fake_gate_name(which);
    }
    uint32_t obj = out_push(fanins, table);
    out_gate[obj] = name_slot[which];
    return obj;
}

/* The object carrying literal lit: a constant, a source object, or the source's inverter. */
static uint32_t cell_source(uint32_t lit) {
    lit = resolve(lit);
    uint32_t node = lit >> 1;
    if (node == 0) {
        return lit & 1U;
    }
    if ((lit & 1U) == 0) {
        return out_node[node];
    }
    if (inv_obj[node] == UINT32_MAX) {
        const int32_t one[LUT_SIZE] = {(int32_t)out_node[node], MINI_NULL};
        inv_obj[node] = push_gate(GATE_INV, one, replicate(TABLE_NOT));
    }
    return inv_obj[node];
}

static void emit_cell_ands(const bool *used) {
    for (uint32_t id = 1; id < fk.n_nodes; id++) {
        if (used[id]) {
            const int32_t fan[LUT_SIZE] = {(int32_t)cell_source(fk.nodes[id].fan0),
                                           (int32_t)cell_source(fk.nodes[id].fan1)};
            out_node[id] = push_gate(GATE_AND2, fan, replicate(TABLE_AND));
        }
    }
}

static void emit_cell_po(uint32_t lit) {
    const int32_t fan[LUT_SIZE] = {(int32_t)cell_source(lit), MINI_NULL2};
    (void)out_push(fan, 0);
}

/* Marks the cones of the POs and the kept registers; returns the kept register count. */
static uint32_t mark_used(bool *used) {
    memset(used, 0, sizeof(bool) * MAX_NODES);
    for (uint32_t i = 0; i < fk.n_pos; i++) {
        mark_cone(fk.pos[i].lit, used);
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < fk.n_latches; i++) {
        if (fk.nodes[fk.latches[i]].rep == LIT_NONE) {
            mark_cone(fk.nodes[fk.latches[i]].fan0, used);
            kept++;
        }
    }
    return kept;
}

static odin3_status fake_get_cells(void *self, uint32_t space, odin3_abc_cells *cells,
                                   odin3_abc_fail *fail) {
    (void)self;
    (void)space;
    if (!fk.aig_loaded) {
        return fake_fail_result(fail, "the fake engine has no cells");
    }
    static bool used[MAX_NODES];
    uint32_t kept = mark_used(used);
    memset(out_gate, 0xFF, sizeof out_gate);
    memset(inv_obj, 0xFF, sizeof inv_obj);
    name_slot[GATE_AND2] = -1;
    name_slot[GATE_INV] = -1;
    n_out_names = 0;
    out_objs = 0;
    (void)out_push(NULL, 0);          /* constant 0 */
    (void)out_push(NULL, UINT32_MAX); /* constant 1 */
    emit_inputs();
    emit_cell_ands(used);
    for (uint32_t i = 0; i < fk.n_pos; i++) {
        emit_cell_po(fk.pos[i].lit);
    }
    for (uint32_t i = 0; i < fk.n_latches; i++) {
        if (fk.nodes[fk.latches[i]].rep == LIT_NONE) {
            emit_cell_po(fk.nodes[fk.latches[i]].fan0);
        }
    }
    cells->net = (odin3_abc_luts){out_fanins, out_truths, out_ci_from, out_objs, kept, LUT_SIZE};
    cells->gate = out_gate;
    cells->names = out_names;
    cells->n_names = n_out_names;
    return ODIN3_OK;
}

static void fake_close(void *self) {
    (void)self;
    fk.open = false;
    fk.text = NULL;
}

static const odin3_abc_engine k_fake = {ODIN3_ABC_ENGINE_VERSION,
                                        "fake",
                                        "fa4e00000000000000000000000000000000fa4e",
                                        NULL,
                                        fake_open,
                                        fake_load_aig,
                                        fake_exec,
                                        fake_get_luts,
                                        fake_name_map,
                                        fake_load_genlib,
                                        fake_get_cells,
                                        fake_close,
                                        {NULL, NULL, NULL, NULL}};

const odin3_abc_engine *odin3_abc_fake_engine(void) {
    return &k_fake;
}

void odin3_abc_fake_reset(void) {
    memset(&fk, 0, sizeof fk);
    fk.const_lut = -1;
}

uint32_t odin3_abc_fake_open_count(void) {
    return fk.opens;
}

const char *odin3_abc_fake_last_commands(void) {
    return fk.commands;
}

void odin3_abc_fake_fail_on(const char *word, odin3_abc_fail fail) {
    fk.fail_set = word != NULL;
    if (word != NULL) {
        (void)snprintf(fk.fail_word, NAME_BYTES, "%s", word);
        fk.fail = fail;
    }
}

void odin3_abc_fake_extra(const char *command) {
    (void)snprintf(fk.extra, sizeof fk.extra, "%s", command == NULL ? "" : command);
}

void odin3_abc_fake_name_map(int mode) {
    fk.map_mode = mode;
}

const char *odin3_abc_fake_genlib(void) {
    return fk.genlib;
}

void odin3_abc_fake_gate_names(odin3_abc_fake_gates names) {
    (void)snprintf(fk.gate_and2, NAME_BYTES, "%s", names.and2 == NULL ? "" : names.and2);
    (void)snprintf(fk.gate_inv, NAME_BYTES, "%s", names.inv == NULL ? "" : names.inv);
}
