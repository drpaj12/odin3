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

enum { MAX_NODES = 4096, MAX_PORTS = 256, MAX_SOP_INPUTS = 64, NAME_BYTES = 40, CMD_BYTES = 8192 };

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

static odin3_status fake_unsupported(odin3_abc_fail *fail) {
    fail->kind = ODIN3_ABC_FAIL_RESULT;
    fail->code = 0;
    (void)snprintf(fail->text, sizeof fail->text, "the fake engine has no in-memory exchange yet");
    return ODIN3_ERR_ENGINE;
}

static odin3_status fake_load_aig(void *self, const odin3_abc_aig *aig, odin3_abc_fail *fail) {
    (void)self;
    (void)aig;
    return fake_unsupported(fail);
}

static odin3_status fake_get_luts(void *self, odin3_abc_luts *luts, odin3_abc_fail *fail) {
    (void)self;
    (void)luts;
    return fake_unsupported(fail);
}

static odin3_status fake_name_map(void *self, const odin3_abc_luts *aligned, const int32_t **lits,
                                  odin3_abc_fail *fail) {
    (void)self;
    (void)aligned;
    (void)lits;
    return fake_unsupported(fail);
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
