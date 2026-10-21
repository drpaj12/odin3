/*
 * bind.c — the group of cone_check (Phase 7 spec 3.3, C-1 to C-3): the nodes marked KEY=V, the
 * boundary nets they touch, the refusals that come before any proof, located errors, and the hash
 * that `raise` (Task 8) compares. Nothing here changes the IR.
 */
#include "ast/srcman.h"
#include "ir/celltype.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "passes/cone_check/cone_check_int.h"
#include "util/log.h"
#include "util/sha256.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { INT_TEXT = 32, LINE_BYTES = 96 };

/* --- errors -------------------------------------------------------------------------------- */

static void first_leaf(void *user, odin3_prov_id leaf) {
    odin3_prov_id *found = user;
    if (found->v == 0) {
        *found = leaf;
    }
}

odin3_status cc_error(odin3_design *design, odin3_prov_id prov, const char *fmt, ...) {
    char msg[ODIN3_LOG_BUF];
    va_list args;
    va_start(args, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);
    odin3_prov_id leaf = {0};
    if (prov.v != 0) {
        (void)odin3_prov_sources(design, prov, first_leaf, &leaf);
    }
    const odin3_prov_record *rec = leaf.v != 0 ? odin3_prov_get(design, leaf) : NULL;
    if (rec != NULL && rec->n_locs > 0 && rec->locs[0].loc != 0) {
        odin3_diag(design, ODIN3_LOG_ERROR, (odin3_loc){rec->locs[0].loc}, "%s", msg);
    } else if (rec != NULL && rec->n_locs > 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s:%" PRIu32 ":%" PRIu32 ": error: %s",
                  odin3_strtab_get(odin3_design_strtab(design), rec->locs[0].file),
                  rec->locs[0].line, rec->locs[0].col, msg);
    } else {
        odin3_log(ODIN3_LOG_ERROR, "%s", msg);
    }
    return ODIN3_ERR_INVALID_ARG;
}

/* --- the group ----------------------------------------------------------------------------- */

void cc_group_init(cc_group *group, const odin3_module *module) {
    group->design = odin3_module_design(module);
    group->module = module;
    odin3_vec_init(&group->nodes, sizeof(uint32_t));
    odin3_vec_init(&group->inside, sizeof(uint8_t));
    odin3_vec_init(&group->nets, sizeof(cc_bnet));
}

void cc_group_free(cc_group *group) {
    odin3_vec_free(&group->nodes);
    odin3_vec_free(&group->inside);
    odin3_vec_free(&group->nets);
}

static odin3_status fill_zero(odin3_vec *vec, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint8_t *slot = odin3_vec_push(vec);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = 0;
    }
    return ODIN3_OK;
}

static bool value_is(const odin3_design *design, const odin3_value *val, const char *text) {
    char buf[INT_TEXT];
    if (val == NULL) {
        return false;
    }
    if (val->kind == ODIN3_VAL_STRING) {
        return strcmp(odin3_strtab_get(odin3_design_strtab(design), val->str), text) == 0;
    }
    if (val->kind != ODIN3_VAL_INT) {
        return false;
    }
    (void)snprintf(buf, sizeof buf, "%" PRId64, val->i);
    return strcmp(buf, text) == 0;
}

/* The live nodes whose attribute key_str (0: none) has the text value. */
static odin3_status collect_nodes(cc_group *group, uint32_t key_str, const char *value) {
    odin3_status st = fill_zero(&group->inside, odin3_module_node_end(group->module));
    if (st != ODIN3_OK || key_str == 0) {
        return st;
    }
    for (uint32_t nid = 1; nid < odin3_module_node_end(group->module); nid++) {
        if (!odin3_node_live(group->module, (odin3_node_id){nid}) ||
            !value_is(group->design,
                      odin3_attr_get(group->module, (odin3_objref){ODIN3_OBJ_NODE, nid}, key_str),
                      value)) {
            continue;
        }
        uint32_t *slot = odin3_vec_push(&group->nodes);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = nid;
        *(uint8_t *)odin3_vec_at(&group->inside, nid) = 1;
    }
    return ODIN3_OK;
}

/* What the pins of a net say about its role for the group. */
typedef struct cc_pins {
    uint32_t drivers, in_drivers, in_sinks, out_sinks;
    bool inout;
} cc_pins;

static cc_pins count_pins(const cc_group *group, odin3_net_id net) {
    cc_pins counts = {0, 0, 0, 0, false};
    odin3_pinlist list = odin3_net_pins(group->module, net);
    const uint8_t *inside = group->inside.data;
    for (uint32_t i = 0; i < list.count; i++) {
        odin3_pin_id pin = list.pins[i];
        uint32_t node = odin3_pin_node(group->module, pin).v;
        bool in = node < group->inside.len && inside[node] != 0;
        counts.inout = counts.inout || odin3_pin_dir(group->module, pin) == ODIN3_DIR_INOUT;
        if (odin3_pin_drives(group->module, pin)) {
            counts.drivers++;
            counts.in_drivers += in ? 1U : 0U;
        } else if (in) {
            counts.in_sinks++;
        } else {
            counts.out_sinks++;
        }
    }
    return counts;
}

static void classify(const cc_pins *counts, cc_bnet *out) {
    out->issue = CC_ISSUE_NONE;
    if (counts->inout) {
        out->issue = CC_ISSUE_INOUT;
    } else if (counts->drivers > 1) {
        out->issue = CC_ISSUE_MULTI;
    }
    if (counts->in_drivers > 0) {
        out->dir = counts->out_sinks > 0 ? CC_NET_OUT : CC_NET_INTERNAL;
    } else {
        out->dir = counts->in_sinks > 0 ? CC_NET_IN : CC_NET_INTERNAL;
    }
}

/* The strtab ID of net's odin3:bind text, 0 when it has none or is not a string. A net without
 * one takes the text of its primary wire when that wire has one bit: tools that can only
 * attribute wires (the JSON netnames of a port) bind their one-bit ports that way. */
static uint32_t bind_of(const cc_group *group, uint32_t key_str, odin3_net_id net) {
    if (key_str == 0) {
        return 0;
    }
    const odin3_value *val =
        odin3_attr_get(group->module, (odin3_objref){ODIN3_OBJ_NET, net.v}, key_str);
    odin3_wirebit wb = odin3_net_primary(group->module, net);
    if (val == NULL && odin3_wire_valid(wb.wire) && odin3_wire_width(group->module, wb.wire) == 1) {
        val = odin3_attr_get(group->module, (odin3_objref){ODIN3_OBJ_WIRE, wb.wire.v}, key_str);
    }
    return val != NULL && val->kind == ODIN3_VAL_STRING ? val->str : 0;
}

/* Marks every net a group pin is on (one uint8_t per net ID). */
static odin3_status mark_touched(const cc_group *group, odin3_vec *seen) {
    odin3_status st = fill_zero(seen, odin3_module_net_end(group->module));
    const uint32_t *nodes = group->nodes.data;
    for (size_t i = 0; st == ODIN3_OK && i < group->nodes.len; i++) {
        odin3_pinslice pins = odin3_node_pins(group->module, (odin3_node_id){nodes[i]});
        for (uint32_t pin = 0; pin < pins.count; pin++) {
            odin3_net_id net = odin3_pin_net(group->module, (odin3_pin_id){pins.first.v + pin});
            if (odin3_net_valid(net)) {
                *(uint8_t *)odin3_vec_at(seen, net.v) = 1;
            }
        }
    }
    return st;
}

static odin3_status collect_nets(cc_group *group) {
    odin3_vec seen;
    odin3_vec_init(&seen, sizeof(uint8_t));
    uint32_t key_str = 0;
    (void)odin3_strtab_find(odin3_design_strtab(group->design), odin3_bytes_cstr(CC_KEY_BIND),
                            &key_str);
    odin3_status st = mark_touched(group, &seen);
    for (uint32_t nid = 1; st == ODIN3_OK && nid < seen.len; nid++) {
        if (*(uint8_t *)odin3_vec_at(&seen, nid) == 0) {
            continue;
        }
        cc_bnet entry = {nid, 0, 0, bind_of(group, key_str, (odin3_net_id){nid}), -1};
        cc_pins counts = count_pins(group, (odin3_net_id){nid});
        classify(&counts, &entry);
        if (entry.dir == CC_NET_INTERNAL && entry.issue == CC_ISSUE_NONE) {
            continue;
        }
        cc_bnet *slot = odin3_vec_push(&group->nets);
        if (slot == NULL) {
            st = ODIN3_ERR_NO_MEMORY;
        } else {
            *slot = entry;
        }
    }
    odin3_vec_free(&seen);
    return st;
}

odin3_status cc_group_collect(cc_group *group, const cc_opts *opts) {
    uint32_t key_str = 0;
    (void)odin3_strtab_find(odin3_design_strtab(group->design), odin3_bytes_cstr(opts->key),
                            &key_str);
    odin3_status st = collect_nodes(group, key_str, opts->value);
    return st == ODIN3_OK && group->nodes.len > 0 ? collect_nets(group) : st;
}

/* --- labels -------------------------------------------------------------------------------- */

const char *cc_net_label(const cc_group *group, odin3_net_id net, char *buf, size_t size) {
    const odin3_strtab *tab = odin3_design_strtab(group->design);
    uint32_t name = odin3_net_name(group->module, net);
    if (name != 0) {
        return odin3_strtab_get(tab, name);
    }
    odin3_wirebit wb = odin3_net_primary(group->module, net);
    if (odin3_wire_valid(wb.wire)) {
        const char *wire = odin3_strtab_get(tab, odin3_wire_name(group->module, wb.wire));
        if (odin3_wire_width(group->module, wb.wire) > 1) {
            (void)snprintf(buf, size, "%s[%d]", wire,
                           (int)odin3_wire_index(group->module, wb.wire, wb.bit));
        } else {
            (void)snprintf(buf, size, "%s", wire);
        }
        return buf;
    }
    (void)snprintf(buf, size, "$n%" PRIu32, net.v);
    return buf;
}

static const char *node_label(const cc_group *group, odin3_node_id node, char *buf, size_t size) {
    uint32_t name = odin3_node_name(group->module, node);
    if (name != 0) {
        return odin3_strtab_get(odin3_design_strtab(group->design), name);
    }
    (void)snprintf(buf, size, "$%" PRIu32, node.v);
    return buf;
}

/* --- the refusals -------------------------------------------------------------------------- */

static odin3_status check_node(const cc_group *group, odin3_node_id node, const char *group_text) {
    const odin3_celltype_def *def =
        odin3_celltype_get(group->design, odin3_node_type(group->module, node));
    char buf[INT_TEXT];
    const char *label = node_label(group, node, buf, sizeof buf);
    odin3_prov_id prov = odin3_node_prov(group->module, node);
    if ((def->flags & (ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL)) != 0) {
        return cc_error(group->design, prov,
                        "cone_check: group '%s' contains a register (%s '%s'): v1.0 proves "
                        "combinational cones only",
                        group_text, def->name, label);
    }
    if ((def->flags & (ODIN3_CT_TRISTATE | ODIN3_CT_PROCESS)) != 0 ||
        (def->gran != ODIN3_GRAN_WORD && def->gran != ODIN3_GRAN_BIT)) {
        return cc_error(group->design, prov,
                        "cone_check: group '%s' contains %s '%s', which is not a combinational "
                        "logic cell",
                        group_text, def->name, label);
    }
    return ODIN3_OK;
}

static odin3_status check_net(const cc_group *group, const cc_bnet *entry, const char *group_text) {
    char buf[INT_TEXT];
    odin3_net_id net = {entry->net};
    const char *label = cc_net_label(group, net, buf, sizeof buf);
    odin3_prov_id prov = odin3_net_prov(group->module, net);
    if (entry->issue == CC_ISSUE_MULTI) {
        return cc_error(group->design, prov,
                        "cone_check: group '%s': net '%s' has more than one driver", group_text,
                        label);
    }
    if (entry->issue == CC_ISSUE_INOUT) {
        return cc_error(group->design, prov,
                        "cone_check: group '%s': net '%s' goes through an inout pin", group_text,
                        label);
    }
    if (entry->dir != CC_NET_INTERNAL && entry->bind == 0) {
        return cc_error(group->design, prov,
                        "cone_check: group '%s': boundary net '%s' has no " CC_KEY_BIND, group_text,
                        label);
    }
    return ODIN3_OK;
}

odin3_status cc_group_check(const cc_group *group, const char *key, const char *value) {
    char text[ODIN3_LOG_BUF / 4];
    (void)snprintf(text, sizeof text, "%s=%s", key, value);
    if (group->nodes.len == 0) {
        return cc_error(
            group->design, (odin3_prov_id){0},
            "cone_check: group '%s' is empty: no live node of module '%s' has it", text,
            odin3_strtab_get(odin3_design_strtab(group->design), odin3_module_name(group->module)));
    }
    const uint32_t *nodes = group->nodes.data;
    for (size_t i = 0; i < group->nodes.len; i++) {
        odin3_status st = check_node(group, (odin3_node_id){nodes[i]}, text);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    odin3_status worst = ODIN3_OK;
    const cc_bnet *nets = group->nets.data;
    for (size_t i = 0; i < group->nets.len; i++) {
        odin3_status st = check_net(group, &nets[i], text);
        worst = st != ODIN3_OK ? st : worst;
    }
    return worst;
}

/* --- the hash ------------------------------------------------------------------------------ */

/* Feeds the whole of text, however long. */
static void hash_str(odin3_sha256 *sha, const char *text) {
    odin3_sha256_update(sha, text, strlen(text));
}

static void hash_fmt(odin3_sha256 *sha, const char *fmt, ...) ODIN3_PRINTF(2, 3);

/* Feeds one formatted line of at most LINE_BYTES - 1 bytes (numbers and fixed text only). */
static void hash_fmt(odin3_sha256 *sha, const char *fmt, ...) {
    char line[LINE_BYTES];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    if (len > 0) {
        odin3_sha256_update(sha, line, (size_t)len < sizeof line ? (size_t)len : sizeof line - 1);
    }
}

/* The line `b<net>:<binding>` of a bound boundary net. */
static void hash_bind(odin3_sha256 *sha, uint32_t net, const char *binding) {
    hash_fmt(sha, "b%" PRIu32 ":", net);
    hash_str(sha, binding);
    hash_str(sha, "\n");
}

/* One parameter value: its kind and the whole of its payload. */
static void hash_value(odin3_sha256 *sha, const odin3_design *design, const odin3_value *val) {
    hash_fmt(sha, "p%d:%" PRId64 ":%" PRIu32 ":%" PRIu32 ":", (int)val->kind, val->i, val->len,
             val->cover_inputs);
    if (val->kind == ODIN3_VAL_STRING) {
        hash_str(sha, odin3_strtab_get(odin3_design_strtab(design), val->str));
    } else if (val->bits != NULL && val->len > 0) {
        odin3_sha256_update(sha, val->bits, val->len);
    }
    hash_str(sha, "\n");
}

/* A node: `n<ID>:<type>`, its parameters, then `c<port>.<bit>=<net>` for each pin (net 0: open). */
static void hash_node(odin3_sha256 *sha, const cc_group *group, uint32_t id) {
    const odin3_module *mod = group->module;
    odin3_node_id node = {id};
    const odin3_celltype_def *def = odin3_celltype_get(group->design, odin3_node_type(mod, node));
    hash_fmt(sha, "n%" PRIu32 ":", id);
    hash_str(sha, def->name);
    hash_str(sha, "\n");
    for (uint32_t k = 0; k < def->n_params; k++) {
        const odin3_value *val = odin3_node_param(mod, node, k);
        if (val != NULL) {
            hash_value(sha, group->design, val);
        }
    }
    odin3_pinslice pins = odin3_node_pins(mod, node);
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        hash_fmt(sha, "c%" PRIu32 ".%" PRIu32 "=%" PRIu32 "\n", odin3_pin_port(mod, pin),
                 odin3_pin_bit(mod, pin), odin3_pin_net(mod, pin).v);
    }
}

odin3_status odin3_cone_hash(const odin3_module *module, const char *key, const char *value,
                             char out[ODIN3_CONE_HASH_BYTES]) {
    if (module == NULL || key == NULL || value == NULL || out == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    cc_group group;
    cc_group_init(&group, module);
    cc_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.key = key;
    opts.value = value;
    odin3_status st = cc_group_collect(&group, &opts);
    st = st == ODIN3_OK && group.nodes.len == 0 ? ODIN3_ERR_INVALID_ARG : st;
    if (st == ODIN3_OK) {
        odin3_sha256 sha;
        odin3_sha256_init(&sha);
        hash_str(&sha, key);
        hash_str(&sha, "=");
        hash_str(&sha, value);
        hash_str(&sha, "\n");
        const uint32_t *nodes = group.nodes.data;
        for (size_t i = 0; i < group.nodes.len; i++) {
            hash_node(&sha, &group, nodes[i]);
        }
        const cc_bnet *nets = group.nets.data;
        for (size_t i = 0; i < group.nets.len; i++) {
            if (nets[i].dir != CC_NET_INTERNAL && nets[i].bind != 0) {
                hash_bind(&sha, nets[i].net,
                          odin3_strtab_get(odin3_design_strtab(group.design), nets[i].bind));
            }
        }
        uint8_t digest[ODIN3_SHA256_DIGEST_BYTES];
        odin3_sha256_final(&sha, digest);
        odin3_sha256_hex(digest, out);
    }
    cc_group_free(&group);
    return st;
}
