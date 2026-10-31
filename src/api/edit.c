/*
 * edit.c — the public ABI's netlist-editing group (spec docs/specs/2026-10-31-netlist-edit-sim-
 * design.md section 2): thin, checked wrappers over the IR mutation primitives of src/ir/module.h.
 * Every object an edit makes carries an IMPORTED leaf record in a pass run named "edit" whose
 * arguments are the caller's tag (section 3.1).
 */
#include "api/api.h"

#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A $sop's parameters, in definition order (src/ir/cells/sop.c). */
enum { SOP_WIDTH = 0, SOP_COVER = 1, SOP_PARAMS = 2 };

static const char K_EDIT_RUN[] = "edit";

/* A mutable module of the design for an edit call; NULL (logged) when the edit is refused. */
static odin3_module *edit_module(odin3_design *design, uint32_t module, const char *fn) {
    if (odin3_api_module(design, module) == NULL) {
        (void)odin3_api_invalid(fn);
        return NULL;
    }
    if (odin3_design_sim_holds(design) > 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s: the design has %u open simulation(s); close them first",
                  fn, odin3_design_sim_holds(design));
        return NULL;
    }
    return odin3_module_get(design, (odin3_module_id){module});
}

static odin3_status intern_text(odin3_design *design, const char *text, uint32_t *str) {
    return odin3_design_intern(design, (odin3_bytes){text, strlen(text)}, str);
}

/* The latest run named "edit" with arguments tag_str, 0 when there is none. */
static odin3_passrun_id find_edit_run(const odin3_design *design, uint32_t run_str,
                                      uint32_t tag_str) {
    for (uint32_t run = odin3_passrun_end(design); run > 1; run--) {
        odin3_passrun_id id = {run - 1};
        if (odin3_passrun_name(design, id) == run_str &&
            odin3_passrun_args(design, id) == tag_str) {
            return id;
        }
    }
    return (odin3_passrun_id){0};
}

/* The provenance record of an edit under tag: one per (tag, run), shared by its objects. */
static odin3_status edit_prov(odin3_design *design, const char *tag, odin3_prov_id *out) {
    if (tag == NULL || tag[0] == '\0') {
        return odin3_api_invalid_why("edit", "the edit tag must be a non-empty string");
    }
    uint32_t run_str = 0;
    uint32_t tag_str = 0;
    odin3_status st = intern_text(design, K_EDIT_RUN, &run_str);
    if (st == ODIN3_OK) {
        st = intern_text(design, tag, &tag_str);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_pass_ctx ctx = {design, find_edit_run(design, run_str, tag_str), 0};
    if (ctx.run.v == 0) {
        st = odin3_pass_run_begin_args(design, run_str, tag_str, &ctx);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    odin3_prov_begin_op(&ctx);
    const odin3_prov_origin origin = {NULL, 0, 0, 0};
    return odin3_prov_imported(&ctx, &origin, out);
}

/* Debug builds: the module must pass the fast check after every edit. */
static odin3_status after_edit(odin3_module *module, const char *fn) {
#ifndef NDEBUG
    odin3_check_opts opts = {ODIN3_CHECK_FAST, ODIN3_VIEW_NONE};
    if (odin3_check_module(module, opts) != ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "%s: the edit leaves the module failing check", fn);
        return ODIN3_ERR_CHECK;
    }
#else
    (void)module;
    (void)fn;
#endif
    return ODIN3_OK;
}

/* name (NULL or empty: unnamed) as a strtab ID. */
static odin3_status name_str(odin3_design *design, const char *name, uint32_t *str) {
    *str = 0;
    if (name == NULL || name[0] == '\0') {
        return ODIN3_OK;
    }
    return intern_text(design, name, str);
}

ODIN3_EXPORT odin3_status odin3_edit_add_net(odin3_design *design, uint32_t module,
                                             const char *name, const char *tag, odin3_ref *net) {
    odin3_module *mod = edit_module(design, module, "odin3_edit_add_net");
    if (mod == NULL || net == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_prov_id prov = {0};
    uint32_t name_id = 0;
    odin3_net_id id = {0};
    odin3_status st = edit_prov(design, tag, &prov);
    if (st == ODIN3_OK) {
        st = name_str(design, name, &name_id);
    }
    if (st == ODIN3_OK) {
        st = odin3_net_create(mod, name_id, prov, &id);
    }
    if (st == ODIN3_OK) {
        st = after_edit(mod, "odin3_edit_add_net");
    }
    if (st == ODIN3_OK) {
        *net = (odin3_ref){module, id.v};
    }
    return st;
}

/* The cell type named type_name, resolved in the design. */
static odin3_status find_type(odin3_design *design, const char *type_name, odin3_celltype_id *out) {
    uint32_t str = 0;
    if (type_name == NULL || intern_text(design, type_name, &str) != ODIN3_OK ||
        !odin3_celltype_find(design, str, out)) {
        return odin3_api_invalid_why("odin3_edit_add_node", "unknown cell type");
    }
    return ODIN3_OK;
}

static odin3_status make_node(odin3_design *design, odin3_module *mod, odin3_node_spec spec,
                              const char *name, const char *tag, odin3_node_id *out) {
    odin3_status st = edit_prov(design, tag, &spec.prov);
    if (st == ODIN3_OK) {
        st = name_str(design, name, &spec.name);
    }
    if (st == ODIN3_OK) {
        st = odin3_node_create(mod, &spec, out);
    }
    return st;
}

ODIN3_EXPORT odin3_status odin3_edit_add_node(odin3_design *design, uint32_t module,
                                              const char *cell_type, const char *name,
                                              const char *tag, odin3_ref *node) {
    odin3_module *mod = edit_module(design, module, "odin3_edit_add_node");
    odin3_node_spec spec = {0};
    odin3_node_id id = {0};
    if (mod == NULL || node == NULL || find_type(design, cell_type, &spec.type) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = make_node(design, mod, spec, name, tag, &id);
    if (st == ODIN3_OK) {
        st = after_edit(mod, "odin3_edit_add_node");
    }
    if (st == ODIN3_OK) {
        *node = (odin3_ref){module, id.v};
    }
    return st;
}

/*
 * Parses BLIF cover text into the COVER encoding: per row, n_inputs chars of 0/1/-, optional
 * blanks, the output char 0/1. Rows end at a newline; blank lines are skipped. *bits is
 * odin3_util_malloc'd (the caller frees), *len its size.
 */
static odin3_status parse_cover(const char *text, uint32_t n_inputs, uint8_t **bits,
                                uint32_t *len) {
    *bits = NULL;
    *len = 0;
    if (text == NULL) {
        return odin3_api_invalid_why("odin3_edit_cover", "the cover text is NULL");
    }
    size_t cap = strlen(text) + 1;
    uint8_t *out = odin3_util_malloc(cap);
    if (out == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t used = 0;
    size_t row_pos = 0;
    size_t row_len = (size_t)n_inputs + 1;
    for (const char *chr = text;; chr++) {
        if (*chr == '\n' || *chr == '\0') {
            if (row_pos != 0 && row_pos != row_len) {
                odin3_util_free(out);
                return odin3_api_invalid_why("odin3_edit_cover", "a cover row has the wrong width");
            }
            row_pos = 0;
            if (*chr == '\0') {
                break;
            }
        } else if (*chr != ' ' && *chr != '\t' && *chr != '\r') {
            if (row_pos == row_len) {
                odin3_util_free(out);
                return odin3_api_invalid_why("odin3_edit_cover", "a cover row has the wrong width");
            }
            out[used++] = (uint8_t)*chr;
            row_pos++;
        }
    }
    *bits = out;
    *len = (uint32_t)used;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_edit_add_sop(odin3_design *design, uint32_t module,
                                             uint32_t n_inputs, const char *cover,
                                             const char *name, const char *tag, odin3_ref *node) {
    odin3_module *mod = edit_module(design, module, "odin3_edit_add_sop");
    uint8_t *bits = NULL;
    uint32_t len = 0;
    if (mod == NULL || node == NULL || parse_cover(cover, n_inputs, &bits, &len) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_node_spec spec = {0};
    odin3_node_id id = {0};
    odin3_value params[SOP_PARAMS] = {odin3_value_int(n_inputs), {.kind = ODIN3_VAL_COVER}};
    params[SOP_COVER].bits = bits;
    params[SOP_COVER].len = len;
    params[SOP_COVER].cover_inputs = n_inputs;
    spec.params = params;
    spec.n_params = SOP_PARAMS;
    odin3_status st = find_type(design, "$sop", &spec.type);
    if (st == ODIN3_OK) {
        st = make_node(design, mod, spec, name, tag, &id);
    }
    odin3_util_free(bits);
    if (st == ODIN3_OK) {
        st = after_edit(mod, "odin3_edit_add_sop");
    }
    if (st == ODIN3_OK) {
        *node = (odin3_ref){module, id.v};
    }
    return st;
}

/* --- connections ---------------------------------------------------------------------------- */

/* The module of pin and net (both in one module), mutable; NULL (logged) when refused. */
static odin3_module *edit_pin_net(odin3_design *design, odin3_ref pin, const odin3_ref *net,
                                  const char *fn) {
    odin3_module *mod = edit_module(design, pin.module, fn);
    if (mod == NULL) {
        return NULL;
    }
    bool ok = odin3_api_ref(design, pin, ODIN3_API_PIN) != NULL &&
              odin3_pin_live(mod, (odin3_pin_id){pin.id});
    if (net != NULL) {
        ok = ok && net->module == pin.module &&
             odin3_api_ref(design, *net, ODIN3_API_NET) != NULL &&
             odin3_net_live(mod, (odin3_net_id){net->id});
    }
    if (!ok) {
        (void)odin3_api_invalid_why(fn, "a dead or unknown pin or net, or not in one module");
        return NULL;
    }
    return mod;
}

/* The driver rule: an output pin may not join a net that has a driver other than itself. */
static bool driver_conflict(const odin3_module *mod, odin3_pin_id pin, odin3_net_id net) {
    return odin3_pin_drives(mod, pin) && odin3_net_driver_count(mod, net) > 0 &&
           odin3_pin_net(mod, pin).v != net.v;
}

ODIN3_EXPORT odin3_status odin3_edit_connect(odin3_design *design, odin3_ref pin, odin3_ref net) {
    odin3_module *mod = edit_pin_net(design, pin, &net, "odin3_edit_connect");
    if (mod == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (driver_conflict(mod, (odin3_pin_id){pin.id}, (odin3_net_id){net.id})) {
        return odin3_api_invalid_why("odin3_edit_connect", "the net already has a driver");
    }
    odin3_status st = odin3_pin_connect(mod, (odin3_pin_id){pin.id}, (odin3_net_id){net.id});
    return st == ODIN3_OK ? after_edit(mod, "odin3_edit_connect") : st;
}

ODIN3_EXPORT odin3_status odin3_edit_disconnect(odin3_design *design, odin3_ref pin) {
    odin3_module *mod = edit_pin_net(design, pin, NULL, "odin3_edit_disconnect");
    if (mod == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = odin3_pin_disconnect(mod, (odin3_pin_id){pin.id});
    return st == ODIN3_OK ? after_edit(mod, "odin3_edit_disconnect") : st;
}

ODIN3_EXPORT odin3_status odin3_edit_rewire(odin3_design *design, odin3_ref pin, odin3_ref net) {
    odin3_module *mod = edit_pin_net(design, pin, &net, "odin3_edit_rewire");
    if (mod == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_pin_id pid = {pin.id};
    odin3_net_id nid = {net.id};
    odin3_net_id old = odin3_pin_net(mod, pid);
    if (old.v == nid.v) {
        return ODIN3_OK;
    }
    if (driver_conflict(mod, pid, nid)) {
        return odin3_api_invalid_why("odin3_edit_rewire", "the net already has a driver");
    }
    odin3_status st = odin3_pin_disconnect(mod, pid);
    if (st == ODIN3_OK) {
        st = odin3_pin_connect(mod, pid, nid);
        if (st != ODIN3_OK && old.v != 0) {
            (void)odin3_pin_connect(mod, pid, old); /* restore; the disconnect freed its slot */
        }
    }
    return st == ODIN3_OK ? after_edit(mod, "odin3_edit_rewire") : st;
}

/* --- removal -------------------------------------------------------------------------------- */

ODIN3_EXPORT odin3_status odin3_edit_remove_node(odin3_design *design, odin3_ref node) {
    odin3_module *mod = edit_module(design, node.module, "odin3_edit_remove_node");
    if (mod == NULL || odin3_api_ref(design, node, ODIN3_API_NODE) == NULL ||
        !odin3_node_live(mod, (odin3_node_id){node.id})) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = odin3_node_delete(mod, (odin3_node_id){node.id});
    return st == ODIN3_OK ? after_edit(mod, "odin3_edit_remove_node") : st;
}

ODIN3_EXPORT odin3_status odin3_edit_remove_net(odin3_design *design, odin3_ref net) {
    odin3_module *mod = edit_module(design, net.module, "odin3_edit_remove_net");
    if (mod == NULL || odin3_api_ref(design, net, ODIN3_API_NET) == NULL ||
        !odin3_net_live(mod, (odin3_net_id){net.id})) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = odin3_net_delete(mod, (odin3_net_id){net.id});
    return st == ODIN3_OK ? after_edit(mod, "odin3_edit_remove_net") : st;
}

/* --- parameters: replace the node by a twin with one parameter changed ------------------------ */

/*
 * Makes a twin of old with params, takes over its pins (odin3_node_replace) and its name. The
 * twin carries the edit record: the old node's original provenance is not kept (spec 3.1).
 */
static odin3_status replace_node(odin3_design *design, odin3_module *mod, odin3_node_id old,
                                 const odin3_value *params, uint32_t n_params, const char *tag) {
    odin3_node_spec spec = {odin3_node_type(mod, old), 0, {0}, params, n_params};
    odin3_node_id fresh = {0};
    uint32_t old_name = odin3_node_name(mod, old);
    odin3_status st = edit_prov(design, tag, &spec.prov);
    if (st == ODIN3_OK) {
        st = odin3_node_create(mod, &spec, &fresh);
    }
    if (st == ODIN3_OK) {
        st = odin3_node_replace(mod, (odin3_node_pair){old, fresh});
        if (st != ODIN3_OK) {
            (void)odin3_node_delete(mod, fresh);
        }
    }
    if (st == ODIN3_OK && old_name != 0) {
        st = odin3_node_rename(mod, fresh, old_name);
    }
    return st;
}

/* Copies the node's parameters (shallow: the old node outlives the call) into out. */
static bool copy_node_params(const odin3_module *mod, odin3_node_id node, uint32_t index,
                             odin3_value *out, uint32_t *n_params) {
    const odin3_celltype_def *def =
        odin3_celltype_get(odin3_module_design(mod), odin3_node_type(mod, node));
    if (def == NULL || def->n_params == 0 || def->n_params > SOP_PARAMS || index >= def->n_params) {
        return false;
    }
    *n_params = def->n_params;
    for (uint32_t i = 0; i < def->n_params; i++) {
        out[i] = *odin3_node_param(mod, node, i);
    }
    return true;
}

ODIN3_EXPORT odin3_status odin3_edit_set_cover(odin3_design *design, odin3_ref node,
                                               const char *cover, const char *tag) {
    odin3_module *mod = edit_module(design, node.module, "odin3_edit_set_cover");
    odin3_value params[SOP_PARAMS];
    uint32_t n_params = 0;
    uint8_t *bits = NULL;
    uint32_t len = 0;
    odin3_node_id nid = {node.id};
    if (mod == NULL || odin3_api_ref(design, node, ODIN3_API_NODE) == NULL ||
        !odin3_node_live(mod, nid) || !copy_node_params(mod, nid, SOP_COVER, params, &n_params) ||
        params[SOP_COVER].kind != ODIN3_VAL_COVER) {
        return odin3_api_invalid_why("odin3_edit_set_cover", "not a live node with a COVER");
    }
    uint32_t width = (uint32_t)params[SOP_WIDTH].i;
    if (parse_cover(cover, width, &bits, &len) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    params[SOP_COVER].bits = bits;
    params[SOP_COVER].len = len;
    odin3_status st = replace_node(design, mod, nid, params, n_params, tag);
    odin3_util_free(bits);
    return st == ODIN3_OK ? after_edit(mod, "odin3_edit_set_cover") : st;
}

ODIN3_EXPORT odin3_status odin3_edit_set_param_int(odin3_design *design, odin3_ref node,
                                                   uint32_t index, int64_t value,
                                                   const char *tag) {
    odin3_module *mod = edit_module(design, node.module, "odin3_edit_set_param_int");
    odin3_value params[SOP_PARAMS];
    uint32_t n_params = 0;
    odin3_node_id nid = {node.id};
    if (mod == NULL || odin3_api_ref(design, node, ODIN3_API_NODE) == NULL ||
        !odin3_node_live(mod, nid) || !copy_node_params(mod, nid, index, params, &n_params) ||
        params[index].kind != ODIN3_VAL_INT) {
        return odin3_api_invalid_why("odin3_edit_set_param_int", "not an integer parameter");
    }
    params[index] = odin3_value_int(value);
    odin3_status st = replace_node(design, mod, nid, params, n_params, tag);
    return st == ODIN3_OK ? after_edit(mod, "odin3_edit_set_param_int") : st;
}

/* --- reading the tag ------------------------------------------------------------------------ */

ODIN3_EXPORT odin3_status odin3_edit_get_tag(const odin3_design *design, odin3_obj obj,
                                             const char **tag) {
    odin3_objref ref;
    const odin3_module *mod = tag == NULL ? NULL : odin3_api_obj(design, obj, &ref);
    if (mod == NULL) {
        return odin3_api_invalid("odin3_edit_get_tag");
    }
    *tag = NULL;
    odin3_prov_id prov = {0};
    switch (ref.kind) {
    case ODIN3_OBJ_NODE:
        prov = odin3_node_prov(mod, (odin3_node_id){ref.id});
        break;
    case ODIN3_OBJ_NET:
        prov = odin3_net_prov(mod, (odin3_net_id){ref.id});
        break;
    case ODIN3_OBJ_WIRE:
        prov = odin3_wire_prov(mod, (odin3_wire_id){ref.id});
        break;
    default:
        return ODIN3_OK;
    }
    const odin3_prov_record *rec = odin3_prov_get(design, prov);
    if (rec == NULL || rec->kind != ODIN3_PROV_IMPORTED || rec->run.v == 0) {
        return ODIN3_OK;
    }
    if (strcmp(odin3_api_str(design, odin3_passrun_name(design, rec->run)), K_EDIT_RUN) == 0) {
        *tag = odin3_api_str(design, odin3_passrun_args(design, rec->run));
    }
    return ODIN3_OK;
}
