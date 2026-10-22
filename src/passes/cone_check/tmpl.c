/*
 * tmpl.c — the template cell of cone_check (Phase 7 spec 3.3, C-1, C-2): the cell type and its
 * parameters looked up in the temporary design, the flat numbering of its port bits, and the check
 * of the nets' `odin3:bind` texts `PORT[BIT]` against it (each template bit bound exactly once).
 */
#include "ir/celltype.h"
#include "passes/cone_check/cone_check_int.h"
#include "util/log.h"
#include "util/str.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LABEL_BYTES = 64, WHY_BYTES = 256, DECIMAL = 10, CONST_MAX_BITS = 64 };

void cc_tmpl_init(cc_tmpl *tmpl) {
    *tmpl = (cc_tmpl){0};
    odin3_vec_init(&tmpl->params, sizeof(odin3_value));
    odin3_vec_init(&tmpl->first, sizeof(uint32_t));
    odin3_vec_init(&tmpl->ord, sizeof(uint32_t));
    odin3_vec_init(&tmpl->cbit, sizeof(uint8_t));
}

void cc_tmpl_free(cc_tmpl *tmpl) {
    odin3_vec_free(&tmpl->params);
    odin3_vec_free(&tmpl->first);
    odin3_vec_free(&tmpl->ord);
    odin3_vec_free(&tmpl->cbit);
    odin3_arena_destroy(tmpl->arena);
    tmpl->arena = NULL;
}

#define TMPL_ERR(tmpl, ...) cc_error((tmpl)->design, (odin3_prov_id){0}, __VA_ARGS__)

/* --- the type ------------------------------------------------------------------------------ */

static odin3_status find_type(cc_tmpl *tmpl, const char *cell) {
    uint32_t str = 0;
    odin3_status st = odin3_design_intern(tmpl->design, odin3_bytes_cstr(cell), &str);
    if (st != ODIN3_OK) {
        return st;
    }
    if (!odin3_celltype_find(tmpl->design, str, &tmpl->type)) {
        return TMPL_ERR(tmpl, "cone_check: unknown cell type '%s'", cell);
    }
    tmpl->def = odin3_celltype_get(tmpl->design, tmpl->type);
    if ((tmpl->def->flags & (ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL)) != 0) {
        return TMPL_ERR(tmpl,
                        "cone_check: cell type '%s' is a register: v1.0 proves combinational "
                        "cones only",
                        cell);
    }
    if ((tmpl->def->flags & (ODIN3_CT_TRISTATE | ODIN3_CT_PROCESS)) != 0 ||
        (tmpl->def->gran != ODIN3_GRAN_WORD && tmpl->def->gran != ODIN3_GRAN_BIT)) {
        return TMPL_ERR(tmpl, "cone_check: cell type '%s' is not a combinational logic cell", cell);
    }
    return ODIN3_OK;
}

/* --- the parameters ------------------------------------------------------------------------ */

static odin3_status parse_int(const cc_tmpl *tmpl, const cc_param *param, odin3_value *val) {
    char *end = NULL;
    errno = 0;
    long long number = strtoll(param->value, &end, DECIMAL);
    if (param->value[0] == '\0' || *end != '\0' || errno != 0) {
        return TMPL_ERR(tmpl, "cone_check: parameter '%s' needs an integer, got '%s'", param->name,
                        param->value);
    }
    val->i = number;
    return ODIN3_OK;
}

/* BITS: the text is MSB first, characters 0 and 1 (x and z are refused); LSB first inside. */
static odin3_status parse_bits(cc_tmpl *tmpl, const cc_param *param, odin3_value *val) {
    size_t len = strlen(param->value);
    uint8_t *bits = len > 0 ? odin3_arena_alloc(tmpl->arena, len) : NULL;
    if (len == 0 || bits == NULL) {
        return len == 0 ? TMPL_ERR(tmpl, "cone_check: parameter '%s' needs bits, got '%s'",
                                   param->name, param->value)
                        : ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < len; i++) {
        char ch = (char)tolower((unsigned char)param->value[len - 1 - i]);
        const char *found = ch != '\0' ? strchr("01xz", ch) : NULL;
        if (found == NULL) {
            return TMPL_ERR(tmpl, "cone_check: parameter '%s': '%s' is not a bit string",
                            param->name, param->value);
        }
        if (ch == 'x' || ch == 'z') {
            return TMPL_ERR(tmpl,
                            "cone_check: parameter '%s': '%s' has x or z: the proof reads x and z "
                            "as 0, so the template must not use them",
                            param->name, param->value);
        }
        bits[i] = (uint8_t)(found - "01xz");
    }
    val->bits = bits;
    val->len = (uint32_t)len;
    return ODIN3_OK;
}

static odin3_status set_param(cc_tmpl *tmpl, const cc_param *param, odin3_value *val) {
    switch (val->kind) {
    case ODIN3_VAL_INT:
        return parse_int(tmpl, param, val);
    case ODIN3_VAL_BITS:
        return parse_bits(tmpl, param, val);
    case ODIN3_VAL_STRING:
        return odin3_design_intern(tmpl->design, odin3_bytes_cstr(param->value), &val->str);
    default:
        return TMPL_ERR(tmpl, "cone_check: parameter '%s' of cell type '%s' cannot be set",
                        param->name, tmpl->def->name);
    }
}

static odin3_status apply_params(cc_tmpl *tmpl, const cc_opts *opts) {
    odin3_value *vals = tmpl->params.data;
    const cc_param *given = opts->params.data;
    for (size_t i = 0; i < opts->params.len; i++) {
        uint32_t at = 0;
        while (at < tmpl->def->n_params && strcmp(tmpl->def->params[at].name, given[i].name) != 0) {
            at++;
        }
        if (at == tmpl->def->n_params) {
            return TMPL_ERR(tmpl, "cone_check: cell type '%s' has no parameter '%s'",
                            tmpl->def->name, given[i].name);
        }
        odin3_status st = set_param(tmpl, &given[i], &vals[at]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static odin3_status default_params(cc_tmpl *tmpl) {
    for (uint32_t i = 0; i < tmpl->def->n_params; i++) {
        odin3_value *slot = odin3_vec_push(&tmpl->params);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = tmpl->def->params[i].dflt;
    }
    return ODIN3_OK;
}

/* --- the port bits ------------------------------------------------------------------------- */

static odin3_status push_u32(odin3_vec *vec, uint32_t value) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

static odin3_status layout_port(cc_tmpl *tmpl, uint32_t port, uint32_t *total) {
    const odin3_port_def *def = &tmpl->def->ports[port];
    uint32_t width = odin3_celltype_port_width(tmpl->design, tmpl->type, tmpl->params.data, port);
    if (width == 0 || def->dir == ODIN3_DIR_INOUT) {
        return TMPL_ERR(tmpl,
                        "cone_check: port '%s' of cell type '%s' has no usable width or is "
                        "an inout; give the parameters that size it",
                        def->name, tmpl->def->name);
    }
    odin3_status st = push_u32(&tmpl->first, *total);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < width; bit++) {
        uint32_t *count = def->dir == ODIN3_DIR_IN ? &tmpl->n_in : &tmpl->n_out;
        st = push_u32(&tmpl->ord, (*count)++);
    }
    *total += width;
    return st;
}

/* --- the constants ------------------------------------------------------------------------- */

typedef enum const_err { CONST_OK = 0, CONST_SYNTAX, CONST_WIDTH } const_err;

/* `0b` binary text into bits (LSB first, zero-extended); a one above the width is CONST_WIDTH. */
static const_err decode_binary(const char *digits, uint32_t width, uint8_t *bits) {
    size_t len = strlen(digits);
    if (len == 0) {
        return CONST_SYNTAX;
    }
    for (size_t i = 0; i < len; i++) {
        if (digits[i] != '0' && digits[i] != '1') {
            return CONST_SYNTAX;
        }
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t bit = (uint8_t)(digits[len - 1 - i] - '0');
        if (i < width) {
            bits[i] = bit;
        } else if (bit != 0) {
            return CONST_WIDTH;
        }
    }
    return CONST_OK;
}

static const_err decode_decimal(const char *text, uint32_t width, uint8_t *bits) {
    char *end = NULL;
    if (text[0] < '0' || text[0] > '9') {
        return CONST_SYNTAX;
    }
    errno = 0;
    unsigned long long value = strtoull(text, &end, DECIMAL);
    if (*end != '\0') {
        return CONST_SYNTAX;
    }
    if (errno == ERANGE) {
        return CONST_WIDTH; /* a digit string beyond 2^64-1 fits no port */
    }
    for (uint32_t i = 0; i < width && i < CONST_MAX_BITS; i++) {
        bits[i] = (uint8_t)((value >> i) & 1U);
    }
    return width < CONST_MAX_BITS && (value >> width) != 0 ? CONST_WIDTH : CONST_OK;
}

static const_err decode_const(const char *text, uint32_t width, uint8_t *bits) {
    memset(bits, 0, width);
    if (text[0] == '0' && text[1] == 'b') {
        return decode_binary(text + 2, width, bits);
    }
    return decode_decimal(text, width, bits);
}

odin3_status cc_const_bits(const char *text, uint32_t width, uint8_t *bits) {
    const_err err = decode_const(text, width, bits);
    if (err == CONST_SYNTAX) {
        odin3_log(ODIN3_LOG_ERROR,
                  "cone_check: '%s' is not a constant (decimal up to 2^64-1 or 0b binary)", text);
    } else if (err == CONST_WIDTH) {
        odin3_log(ODIN3_LOG_ERROR, "cone_check: constant '%s' does not fit %u bits", text,
                  (unsigned)width);
    }
    return err == CONST_OK ? ODIN3_OK : ODIN3_ERR_INVALID_ARG;
}

static odin3_status set_const_bits(cc_tmpl *tmpl, uint32_t port, const cc_const *given) {
    const uint32_t *first = tmpl->first.data;
    uint32_t width = first[port + 1] - first[port];
    uint8_t *bits = odin3_arena_alloc(tmpl->arena, width);
    uint8_t *cbit = tmpl->cbit.data;
    if (bits == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    const_err err = decode_const(given->value, width, bits);
    if (err == CONST_SYNTAX) {
        return TMPL_ERR(tmpl,
                        "cone_check: -const %s=%s: '%s' is not a constant (decimal up to "
                        "2^64-1 or 0b binary)",
                        given->port, given->value, given->value);
    }
    if (err == CONST_WIDTH) {
        return TMPL_ERR(tmpl, "cone_check: -const %s=%s: value does not fit port '%s' (%u bits)",
                        given->port, given->value, given->port, (unsigned)width);
    }
    for (uint32_t bit = 0; bit < width; bit++) {
        cbit[first[port] + bit] = bits[bit] != 0 ? CC_BIT_CONST1 : CC_BIT_CONST0;
    }
    return ODIN3_OK;
}

/* One `-const`: finds its port (an input, given once) and sets its bits. */
static odin3_status apply_const(cc_tmpl *tmpl, const cc_const *given, const cc_const *earlier,
                                size_t n_earlier) {
    uint32_t port = 0;
    while (port < tmpl->def->n_ports && strcmp(tmpl->def->ports[port].name, given->port) != 0) {
        port++;
    }
    if (port == tmpl->def->n_ports) {
        return TMPL_ERR(tmpl, "cone_check: -const: cell type '%s' has no port '%s'",
                        tmpl->def->name, given->port);
    }
    if (tmpl->def->ports[port].dir != ODIN3_DIR_IN) {
        return TMPL_ERR(tmpl, "cone_check: -const: port '%s' is not an input", given->port);
    }
    for (size_t i = 0; i < n_earlier; i++) {
        if (strcmp(earlier[i].port, given->port) == 0) {
            return TMPL_ERR(tmpl, "cone_check: -const: port '%s' is given twice", given->port);
        }
    }
    return set_const_bits(tmpl, port, given);
}

static odin3_status apply_consts(cc_tmpl *tmpl, const cc_opts *opts, uint32_t total) {
    for (uint32_t bit = 0; bit < total; bit++) {
        uint8_t *slot = odin3_vec_push(&tmpl->cbit);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = CC_BIT_NET;
    }
    const cc_const *given = opts->consts.data;
    for (size_t i = 0; i < opts->consts.len; i++) {
        odin3_status st = apply_const(tmpl, &given[i], given, i);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

odin3_status cc_tmpl_build(cc_tmpl *tmpl, odin3_design *design, const cc_opts *opts) {
    tmpl->design = design;
    tmpl->arena = odin3_arena_create(0);
    odin3_status st = tmpl->arena != NULL ? find_type(tmpl, opts->cell) : ODIN3_ERR_NO_MEMORY;
    st = st == ODIN3_OK ? default_params(tmpl) : st;
    st = st == ODIN3_OK ? apply_params(tmpl, opts) : st;
    uint32_t total = 0;
    for (uint32_t port = 0; st == ODIN3_OK && port < tmpl->def->n_ports; port++) {
        st = layout_port(tmpl, port, &total);
    }
    st = st == ODIN3_OK ? push_u32(&tmpl->first, total) : st;
    return st == ODIN3_OK ? apply_consts(tmpl, opts, total) : st;
}

/* The port of flat bit `bit`. */
static uint32_t port_of(const cc_tmpl *tmpl, uint32_t bit) {
    const uint32_t *first = tmpl->first.data;
    uint32_t port = 0;
    while (first[port + 1] <= bit && port + 1 < tmpl->first.len - 1) {
        port++;
    }
    return port;
}

odin3_dir cc_tmpl_dir(const cc_tmpl *tmpl, uint32_t bit) {
    return tmpl->def->ports[port_of(tmpl, bit)].dir;
}

const char *cc_tmpl_name(const cc_tmpl *tmpl, uint32_t bit, char *buf, size_t size) {
    uint32_t port = port_of(tmpl, bit);
    const uint32_t *first = tmpl->first.data;
    (void)snprintf(buf, size, "%s[%" PRIu32 "]", tmpl->def->ports[port].name, bit - first[port]);
    return buf;
}

/* --- the binding --------------------------------------------------------------------------- */

/* Parses PORT[BIT] (or PORT for a one-bit port) into a flat bit; NULL, with why, on success. */
static const char *parse_ref(const cc_tmpl *tmpl, const char *text, uint32_t *flat, char *why) {
    const char *bracket = strchr(text, '[');
    size_t name_len = bracket != NULL ? (size_t)(bracket - text) : strlen(text);
    uint32_t port = 0;
    while (port < tmpl->def->n_ports &&
           (strlen(tmpl->def->ports[port].name) != name_len ||
            strncmp(tmpl->def->ports[port].name, text, name_len) != 0)) {
        port++;
    }
    if (port == tmpl->def->n_ports) {
        (void)snprintf(why, WHY_BYTES, "cell type '%s' has no port '%.*s'", tmpl->def->name,
                       (int)name_len, text);
        return why;
    }
    const uint32_t *first = tmpl->first.data;
    uint32_t width = first[port + 1] - first[port];
    char *end = NULL;
    unsigned long bit = bracket != NULL ? strtoul(bracket + 1, &end, DECIMAL) : 0;
    if (bracket != NULL && (bracket[1] == '\0' || end == NULL || end[0] != ']' || end[1] != '\0')) {
        (void)snprintf(why, WHY_BYTES, "expected PORT[BIT]");
        return why;
    }
    if (bracket == NULL ? width != 1 : bit >= width) {
        (void)snprintf(why, WHY_BYTES, "port '%s' has %u bits, bit %s is out of range",
                       tmpl->def->ports[port].name, (unsigned)width,
                       bracket != NULL ? bracket : "(none)");
        return why;
    }
    *flat = first[port] + (uint32_t)bit;
    return NULL;
}

typedef struct cc_bind_state {
    odin3_vec owner; /* uint32_t per flat bit: the net bound to it, 0 for none */
} cc_bind_state;

/* One boundary net: parse, direction, once. Logs; ODIN3_ERR_INVALID_ARG on a problem. */
static odin3_status bind_net(const cc_tmpl *tmpl, const cc_group *group, cc_bnet *entry,
                             cc_bind_state *state) {
    char label[LABEL_BYTES];
    char other[LABEL_BYTES];
    char why[WHY_BYTES];
    char name[LABEL_BYTES];
    odin3_net_id net = {entry->net};
    const char *text = odin3_strtab_get(odin3_design_strtab(group->design), entry->bind);
    odin3_prov_id prov = odin3_net_prov(group->module, net);
    uint32_t flat = 0;
    const char *bad = parse_ref(tmpl, text, &flat, why);
    if (bad != NULL) {
        return cc_error(group->design, prov, "cone_check: net '%s': binding '%s': %s",
                        cc_net_label(group, net, label, sizeof label), text, bad);
    }
    odin3_dir want = entry->dir == CC_NET_IN ? ODIN3_DIR_IN : ODIN3_DIR_OUT;
    if (cc_tmpl_dir(tmpl, flat) != want) {
        return cc_error(group->design, prov,
                        "cone_check: net '%s': binding '%s' is %s bit of the cell but the net is "
                        "a group %s (direction mismatch)",
                        cc_net_label(group, net, label, sizeof label), text,
                        want == ODIN3_DIR_IN ? "an output" : "an input",
                        want == ODIN3_DIR_IN ? "input" : "output");
    }
    if (((const uint8_t *)tmpl->cbit.data)[flat] != CC_BIT_NET) {
        return cc_error(group->design, prov,
                        "cone_check: net '%s': binding '%s': port '%s' is constant (-const)",
                        cc_net_label(group, net, label, sizeof label), text,
                        tmpl->def->ports[port_of(tmpl, flat)].name);
    }
    uint32_t *owner = odin3_vec_at(&state->owner, flat);
    if (*owner != 0) {
        return cc_error(group->design, prov,
                        "cone_check: template bit %s is bound twice (nets '%s' and '%s')",
                        cc_tmpl_name(tmpl, flat, name, sizeof name),
                        cc_net_label(group, (odin3_net_id){*owner}, other, sizeof other),
                        cc_net_label(group, net, label, sizeof label));
    }
    *owner = entry->net;
    entry->bit = (int32_t)flat;
    return ODIN3_OK;
}

odin3_status cc_tmpl_bind(const cc_tmpl *tmpl, cc_group *group) {
    cc_bind_state state;
    odin3_vec_init(&state.owner, sizeof(uint32_t));
    uint32_t bits = ((const uint32_t *)tmpl->first.data)[tmpl->first.len - 1];
    odin3_status st = ODIN3_OK;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < bits; bit++) {
        st = push_u32(&state.owner, 0);
    }
    odin3_status worst = st;
    cc_bnet *nets = group->nets.data;
    for (size_t i = 0; st == ODIN3_OK && i < group->nets.len; i++) {
        if (nets[i].dir != CC_NET_INTERNAL && bind_net(tmpl, group, &nets[i], &state) != ODIN3_OK) {
            worst = ODIN3_ERR_INVALID_ARG;
        }
    }
    const uint32_t *owner = state.owner.data;
    odin3_prov_id first =
        odin3_node_prov(group->module, (odin3_node_id){*(const uint32_t *)group->nodes.data});
    for (uint32_t bit = 0; st == ODIN3_OK && bit < bits; bit++) {
        char name[LABEL_BYTES];
        if (owner[bit] == 0 && ((const uint8_t *)tmpl->cbit.data)[bit] == CC_BIT_NET) {
            worst = cc_error(group->design, first, "cone_check: template bit %s is not bound",
                             cc_tmpl_name(tmpl, bit, name, sizeof name));
        }
    }
    odin3_vec_free(&state.owner);
    return st != ODIN3_OK ? st : worst;
}

/* --- the canonical text of the template ---------------------------------------------------- */

/* Moves the entry at index at down to its place among the sorted entries before it (stable). */
static void sort_in(cc_param *items, size_t at) {
    cc_param item = items[at];
    while (at > 0 && strcmp(items[at - 1].name, item.name) > 0) {
        items[at] = items[at - 1];
        at--;
    }
    items[at] = item;
}

/* ` -const PORT=V` per constant, sorted by port (the same insertion as the parameters). */
static odin3_status append_consts(const cc_opts *opts, odin3_strbuf *out) {
    odin3_vec sorted;
    odin3_vec_init(&sorted, sizeof(cc_param));
    const cc_const *given = opts->consts.data;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < opts->consts.len; i++) {
        cc_param *slot = odin3_vec_push(&sorted);
        if (slot == NULL) {
            st = ODIN3_ERR_NO_MEMORY;
        } else {
            *slot = (cc_param){given[i].port, given[i].value};
            sort_in(sorted.data, i);
        }
    }
    const cc_param *items = sorted.data;
    for (size_t i = 0; st == ODIN3_OK && i < sorted.len; i++) {
        st = odin3_strbuf_appendf(out, " -const %s=%s", items[i].name, items[i].value);
    }
    odin3_vec_free(&sorted);
    return st;
}

odin3_status cc_cell_text(const cc_opts *opts, odin3_strbuf *out) {
    odin3_vec sorted;
    odin3_vec_init(&sorted, sizeof(cc_param));
    odin3_status st = odin3_strbuf_appendf(out, "%s", opts->cell);
    const cc_param *given = opts->params.data;
    for (size_t i = 0; st == ODIN3_OK && i < opts->params.len; i++) {
        cc_param *slot = odin3_vec_push(&sorted);
        if (slot == NULL) {
            st = ODIN3_ERR_NO_MEMORY;
        } else {
            *slot = given[i];
            sort_in(sorted.data, i);
        }
    }
    const cc_param *items = sorted.data;
    for (size_t i = 0; st == ODIN3_OK && i < sorted.len; i++) {
        st = odin3_strbuf_appendf(out, " %s=%s", items[i].name, items[i].value);
    }
    odin3_vec_free(&sorted);
    return st == ODIN3_OK ? append_consts(opts, out) : st;
}
