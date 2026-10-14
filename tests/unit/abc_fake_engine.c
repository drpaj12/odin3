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
    NAME_BYTES = 40,
    CMD_BYTES = 8192,
    LIT_NONE = UINT32_MAX
};

enum { K_CONST = 0, K_PI, K_LATCH, K_AND };

typedef struct fnode {
    uint8_t kind;
    uint32_t a, b;     /* AND: fanin literals; LATCH: a = the D literal (set after reading) */
    uint32_t rep;      /* LATCH: replacement literal after scorr, LIT_NONE when kept */
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
    int32_t const_lut;      /* the LUT (AND index) that `const` makes constant, -1 none */
    int const_value;
    odin3_abc_text_fn text;
    void *user;
    uint32_t opens;
    char commands[CMD_BYTES];
    char fail_word[NAME_BYTES];
    bool fail_set;
    odin3_abc_fail fail;
    bool open;
} fake;

static fake g;

/* --- literals ------------------------------------------------------------------------------ */

static uint32_t new_node(uint8_t kind, const char *name) {
    if (g.n_nodes >= MAX_NODES) {
        abort();
    }
    fnode *node = &g.nodes[g.n_nodes];
    memset(node, 0, sizeof *node);
    node->kind = kind;
    node->rep = LIT_NONE;
    (void)snprintf(node->name, NAME_BYTES, "%s", name);
    return g.n_nodes++;
}

static uint32_t mk_and(uint32_t a, uint32_t b) {
    if (a == 0 || b == 0 || a == (b ^ 1U)) {
        return 0;
    }
    if (a == 1U) {
        return b;
    }
    if (b == 1U || a == b) {
        return a;
    }
    uint32_t node = new_node(K_AND, "");
    g.nodes[node].a = a < b ? a : b;
    g.nodes[node].b = a < b ? b : a;
    return 2U * node;
}

static uint32_t mk_or(uint32_t a, uint32_t b) {
    return mk_and(a ^ 1U, b ^ 1U) ^ 1U;
}

/* Follows scorr's replacements. */
static uint32_t resolve(uint32_t lit) {
    for (;;) {
        const fnode *node = &g.nodes[lit >> 1];
        if (node->kind != K_LATCH || node->rep == LIT_NONE) {
            return lit;
        }
        lit = node->rep ^ (lit & 1U);
    }
}

/* --- reading ------------------------------------------------------------------------------- */

static uint32_t cover_literal(const odin3_value *cover, const uint32_t *ins, uint32_t k) {
    size_t stride = (size_t)k + 1U;
    uint32_t sum = 0;
    bool off_set = false;
    for (size_t off = 0; off + stride <= cover->len; off += stride) {
        uint32_t cube = 1U;
        for (uint32_t j = 0; j < k; j++) {
            if (cover->bits[off + j] != '-') {
                cube = mk_and(cube, cover->bits[off + j] == '1' ? ins[j] : ins[j] ^ 1U);
            }
        }
        sum = mk_or(sum, cube);
        off_set = cover->bits[off + k] == '0';
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

static odin3_net_id pin_net(const odin3_module *mod, odin3_node_id node, uint32_t port,
                            uint32_t bit) {
    odin3_pinslice pins = odin3_node_port(mod, node, port);
    return bit < pins.count ? odin3_pin_net(mod, (odin3_pin_id){pins.first.v + bit})
                            : (odin3_net_id){0};
}

/* One resolution round over the $sop nodes; true when it resolved any. */
static bool resolve_sops(const odin3_module *mod, uint32_t *net_lit, bool *done, uint32_t nodes) {
    bool progress = false;
    for (uint32_t id = 1; id < nodes; id++) {
        odin3_node_id node = {id};
        if (done[id] || !odin3_node_live(mod, node) ||
            !is_type(odin3_module_design(mod), mod, node, "$sop")) {
            continue;
        }
        odin3_pinslice ins = odin3_node_port(mod, node, 0);
        uint32_t lits[64];
        bool ready = ins.count <= 64;
        for (uint32_t j = 0; ready && j < ins.count; j++) {
            odin3_net_id net = odin3_pin_net(mod, (odin3_pin_id){ins.first.v + j});
            lits[j] = net_lit[net.v];
            ready = lits[j] != LIT_NONE;
        }
        if (!ready) {
            continue;
        }
        net_lit[pin_net(mod, node, 1, 0).v] = cover_literal(odin3_node_param(mod, node, 1), lits, ins.count);
        done[id] = true;
        progress = true;
    }
    return progress;
}

static bool port_is_in(const odin3_module *mod, uint32_t index) {
    return is_type(odin3_module_design(mod), mod, odin3_module_port(mod, index), "$port_in");
}

static void add_port_name(fport *dst, const char *name) {
    (void)snprintf(dst->name, NAME_BYTES, "%s", name);
}

/* Forgets the network (a new `read` replaces it); the counters and commands stay. */
static void reset_network(void) {
    g.n_nodes = 0;
    g.n_pis = 0;
    g.n_pos = 0;
    g.n_latches = 0;
    g.const_lut = -1;
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
    odin3_module *mod = odin3_module_get(design, odin3_design_top(design));
    uint32_t nets = odin3_module_net_end(mod);
    uint32_t nodes = odin3_module_node_end(mod);
    uint32_t *net_lit = malloc(sizeof(uint32_t) * (nets + 1U));
    bool *done = calloc(nodes + 1U, sizeof(bool));
    memset(net_lit, 0xFF, sizeof(uint32_t) * (nets + 1U));
    new_node(K_CONST, "const0");
    for (uint32_t i = 0; i < odin3_module_port_count(mod); i++) {
        if (port_is_in(mod, i)) {
            odin3_net_id net = odin3_wire_net(mod, odin3_module_port_wire(mod, i), 0);
            const char *name = odin3_strtab_get(odin3_design_strtab(design),
                                                odin3_wire_name(mod, odin3_module_port_wire(mod, i)));
            net_lit[net.v] = 2U * new_node(K_PI, name);
            add_port_name(&g.pis[g.n_pis++], name);
        }
    }
    for (uint32_t id = 1; id < nodes; id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(mod, node) && is_type(design, mod, node, "$_FF_")) {
            odin3_net_id q = pin_net(mod, node, 1, 0);
            uint32_t idx = new_node(K_LATCH, net_name(design, mod, q));
            net_lit[q.v] = 2U * idx;
            g.latches[g.n_latches++] = idx;
        }
    }
    while (resolve_sops(mod, net_lit, done, nodes)) {
    }
    uint32_t latch = 0;
    for (uint32_t id = 1; id < nodes; id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(mod, node) && is_type(design, mod, node, "$_FF_")) {
            g.nodes[g.latches[latch++]].a = net_lit[pin_net(mod, node, 0, 0).v];
        }
    }
    for (uint32_t i = 0; i < odin3_module_port_count(mod); i++) {
        if (!port_is_in(mod, i)) {
            odin3_net_id net = odin3_wire_net(mod, odin3_module_port_wire(mod, i), 0);
            add_port_name(&g.pos[g.n_pos], odin3_strtab_get(odin3_design_strtab(design),
                          odin3_wire_name(mod, odin3_module_port_wire(mod, i))));
            g.pos[g.n_pos++].lit = net_lit[net.v];
        }
    }
    free(net_lit);
    free(done);
    odin3_design_destroy(design);
    return ODIN3_OK;
}

/* --- scorr --------------------------------------------------------------------------------- */

static void scorr(void) {
    for (uint32_t i = 0; i < g.n_latches; i++) {
        uint32_t me = g.latches[i];
        uint32_t d = resolve(g.nodes[me].a);
        if (d == 0 || d == 2U * me) {
            g.nodes[me].rep = 0;
            continue;
        }
        for (uint32_t j = 0; j < i; j++) {
            uint32_t other = g.latches[j];
            if (g.nodes[other].rep == LIT_NONE && resolve(g.nodes[other].a) == d) {
                g.nodes[me].rep = 2U * other;
                break;
            }
        }
    }
}

/* --- writing ------------------------------------------------------------------------------- */

static void lit_name(uint32_t lit, char *out) {
    const fnode *node = &g.nodes[lit >> 1];
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
        if (used[id] || g.nodes[id].kind != K_AND) {
            continue;
        }
        used[id] = true;
        stack[top++] = resolve(g.nodes[id].a) >> 1;
        stack[top++] = resolve(g.nodes[id].b) >> 1;
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
static uint64_t and_table(uint32_t a, uint32_t b) {
    uint64_t table = 0;
    for (uint32_t row = 0; row < 4U; row++) {
        bool x = (((row >> 0U) & 1U) != 0) != ((a & 1U) != 0);
        bool y = (((row >> 1U) & 1U) != 0) != ((b & 1U) != 0);
        table |= (uint64_t)(x && y ? 1U : 0U) << row;
    }
    return table;
}

static void write_lut(FILE *out, uint32_t id, int lut_index) {
    uint32_t a = resolve(g.nodes[id].a);
    uint32_t b = resolve(g.nodes[id].b);
    char an[NAME_BYTES];
    char bn[NAME_BYTES];
    lit_name(a & ~1U, an);
    lit_name(b & ~1U, bn);
    uint64_t table = and_table(a, b);
    if (g.const_lut == lut_index) {
        table = g.const_value != 0 ? 0xFULL : 0ULL;
    }
    odin3_strbuf cover;
    odin3_strbuf_init(&cover);
    uint32_t rows = 0;
    int constant = -1;
    (void)odin3_blif_isop(&table, 2, &cover, &rows, &constant);
    if (constant >= 0) {
        (void)fprintf(out, ".names n%u\n%s", (unsigned)id, constant == 1 ? "1\n" : "");
    } else {
        (void)fprintf(out, ".names %s %s n%u\n%.*s", an, bn, (unsigned)id, (int)cover.len, cover.data);
    }
    odin3_strbuf_free(&cover);
}

static void write_network(FILE *out) {
    static bool used[MAX_NODES];
    memset(used, 0, sizeof used);
    for (uint32_t i = 0; i < g.n_pos; i++) {
        mark_cone(g.pos[i].lit, used);
    }
    for (uint32_t i = 0; i < g.n_latches; i++) {
        if (g.nodes[g.latches[i]].rep == LIT_NONE) {
            mark_cone(g.nodes[g.latches[i]].a, used);
        }
    }
    (void)fprintf(out, ".model o3abc\n.inputs");
    for (uint32_t i = 0; i < g.n_pis; i++) {
        (void)fprintf(out, " %s", g.pis[i].name);
    }
    (void)fprintf(out, "\n.outputs");
    for (uint32_t i = 0; i < g.n_pos; i++) {
        (void)fprintf(out, " %s", g.pos[i].name);
    }
    (void)fprintf(out, "\n.names const0\n");
    int lut_index = 0;
    for (uint32_t id = 1; id < g.n_nodes; id++) {
        if (used[id]) {
            write_lut(out, id, lut_index++);
        }
    }
    char name[NAME_BYTES];
    char tag[NAME_BYTES];
    for (uint32_t i = 0; i < g.n_latches; i++) {
        const fnode *latch = &g.nodes[g.latches[i]];
        if (latch->rep == LIT_NONE) {
            (void)snprintf(tag, NAME_BYTES, "d%u", (unsigned)i);
            drive(out, latch->a, tag, name);
            (void)fprintf(out, ".latch %s %s 0\n", name, latch->name);
        }
    }
    for (uint32_t i = 0; i < g.n_pos; i++) {
        (void)snprintf(tag, NAME_BYTES, "%s", g.pos[i].name);
        drive(out, g.pos[i].lit, tag, name);
        (void)fprintf(out, ".names %s %s\n1 1\n", name, g.pos[i].name);
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
    size_t used = strlen(g.commands);
    if (used + strlen(command) + 2U < CMD_BYTES) {
        (void)snprintf(g.commands + used, CMD_BYTES - used, "%s\n", command);
    }
}

static odin3_status do_command(const char *command) {
    char first[NAME_BYTES] = "";
    (void)sscanf(command, "%39s", first);
    if (g.fail_set && strcmp(first, g.fail_word) == 0) {
        return ODIN3_ERR_ENGINE;
    }
    char path[CMD_BYTES / 8];
    if (strcmp(first, "read") == 0) {
        (void)quoted(command, path, sizeof path);
        return read_blif(path);
    }
    if (strcmp(first, "write_hie") == 0) {
        const char *rest = quoted(command, path, sizeof path);
        (void)quoted(rest, path, sizeof path);
        return write_hie(path);
    }
    if (strcmp(first, "scorr") == 0) {
        scorr();
    } else if (strcmp(first, "const") == 0) {
        int idx = 0;
        int value = 0;
        int got = sscanf(command, "const %d %d", &idx, &value);
        g.const_lut = got >= 1 ? idx : -1;
        g.const_value = got >= 2 ? value : 0;
    }
    return ODIN3_OK;
}

/* --- the engine table ---------------------------------------------------------------------- */

static odin3_status fake_open(void *self, odin3_abc_text_fn text, void *user, odin3_abc_fail *fail) {
    (void)self;
    (void)fail;
    g.text = text;
    g.user = user;
    g.opens++;
    g.open = true;
    return ODIN3_OK;
}

static odin3_status fake_exec(void *self, const char *command, odin3_abc_fail *fail) {
    (void)self;
    record(command);
    if (g.text != NULL) {
        char line[CMD_BYTES / 4];
        (void)snprintf(line, sizeof line, "fake: %s", command);
        g.text(g.user, line);
    }
    odin3_status st = do_command(command);
    if (st == ODIN3_ERR_ENGINE && g.fail_set) {
        *fail = g.fail;
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
    g.open = false;
    g.text = NULL;
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
    memset(&g, 0, sizeof g);
    g.const_lut = -1;
}

uint32_t odin3_abc_fake_open_count(void) {
    return g.opens;
}

const char *odin3_abc_fake_last_commands(void) {
    return g.commands;
}

void odin3_abc_fake_fail_on(const char *word, odin3_abc_fail fail) {
    g.fail_set = word != NULL;
    if (word != NULL) {
        (void)snprintf(g.fail_word, NAME_BYTES, "%s", word);
        g.fail = fail;
    }
}
