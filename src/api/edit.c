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
#include "ir/edit_events.h"
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

/* sizeof(odin3_edit_new) of the first release of the struct (up to and including index). */
enum { EDIT_NEW_V1_SIZE = 64, MAX_INIT_PARAMS = 8 };

static const char K_DEFAULT_TAG[] = "edit";

/*
 * The edit struct as the caller compiled it, widened to this library's: its size covers at least
 * the first release, fields past its size read as zero, reserved words are zero.
 */
static odin3_status check_what(const odin3_edit_new *in, odin3_edit_new *norm, const char *fn) {
    if (in == NULL) {
        return odin3_api_invalid(fn);
    }
    if (in->size < EDIT_NEW_V1_SIZE) {
        return odin3_api_invalid_why(fn, "odin3_edit_new.size is smaller than the struct");
    }
    memset(norm, 0, sizeof *norm);
    memcpy(norm, in, in->size < sizeof *norm ? in->size : sizeof *norm);
    norm->size = (uint32_t)sizeof *norm;
    if (norm->reserved != 0 || norm->reserved2 != 0) {
        return odin3_api_invalid_why(fn, "odin3_edit_new.reserved must be zero");
    }
    return ODIN3_OK;
}

/* A usable tag: non-empty, no newline (events are line based). */
static odin3_status check_tag(const char *tag, const char *fn) {
    if (tag == NULL || tag[0] == '\0') {
        return odin3_api_invalid_why(fn, "the edit tag must be a non-empty string");
    }
    if (strchr(tag, '\n') != NULL) {
        return odin3_api_invalid_why(fn, "the edit tag must not contain a newline");
    }
    return ODIN3_OK;
}

/* A mutable module of the design for an edit call; NULL (logged) when the edit is refused. */
static odin3_module *edit_module(odin3_design *design, uint32_t module, const char *fn) {
    if (odin3_api_module(design, module) == NULL) {
        (void)odin3_api_invalid(fn);
        return NULL;
    }
    if (odin3_design_sim_holds(design) > 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s: the design has %u open simulation(s); close them first", fn,
                  odin3_design_sim_holds(design));
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
    uint32_t run_str = 0;
    uint32_t tag_str = 0;
    odin3_status st = check_tag(tag, "edit");
    st = st == ODIN3_OK ? intern_text(design, K_EDIT_RUN, &run_str) : st;
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
    const odin3_prov_origin origin = {NULL, 0, 0, 0};
    return odin3_prov_imported(&ctx, &origin, out);
}

/*
 * Debug builds: the module is checked (fast) after every edit. The edit is already applied when
 * the check runs, so a failure (ODIN3_ERR_CHECK, logged) leaves the edit in place; the check is
 * not a pre-commit gate. Release builds never check.
 */
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
                                             const odin3_edit_new *in, odin3_ref *net) {
    odin3_edit_new norm;
    const odin3_edit_new *what = &norm;
    odin3_module *mod = edit_module(design, module, "odin3_edit_add_net");
    if (mod == NULL || net == NULL || check_what(in, &norm, "odin3_edit_add_net") != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_prov_id prov = {0};
    uint32_t name_id = 0;
    odin3_net_id id = {0};
    odin3_status st = edit_prov(design, what->tag, &prov);
    if (st == ODIN3_OK) {
        st = name_str(design, what->name, &name_id);
    }
    if (st == ODIN3_OK) {
        st = odin3_net_create(mod, name_id, prov, &id);
    }
    if (st == ODIN3_OK) {
        st = odin3_edit_event_add(mod, (odin3_objref){ODIN3_OBJ_NET, id.v}, ODIN3_EDIT_ADDED,
                                  what->tag);
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
                              const odin3_edit_new *what, odin3_node_id *out) {
    odin3_status st = edit_prov(design, what->tag, &spec.prov);
    if (st == ODIN3_OK) {
        st = name_str(design, what->name, &spec.name);
    }
    if (st == ODIN3_OK) {
        st = odin3_node_create(mod, &spec, out);
    }
    if (st == ODIN3_OK) {
        st = odin3_edit_event_add(mod, (odin3_objref){ODIN3_OBJ_NODE, out->v}, ODIN3_EDIT_ADDED,
                                  what->tag);
    }
    return st;
}

/* The def parameter named name, as an index; -1 when there is none or it is not an INT. */
static int find_int_param(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; name != NULL && i < def->n_params; i++) {
        if (strcmp(def->params[i].name, name) == 0) {
            return def->params[i].kind == ODIN3_VAL_INT ? (int)i : -1;
        }
    }
    return -1;
}

/* The type's default parameters with what's named integer parameters set; INVALID_ARG (logged). */
static odin3_status initial_params(const odin3_celltype_def *def, const odin3_edit_new *what,
                                   odin3_value *params, uint32_t *n_params) {
    const char *fn = "odin3_edit_add_node";
    bool seen[MAX_INIT_PARAMS] = {false};
    *n_params = 0;
    if (what->n_params == 0) {
        return ODIN3_OK;
    }
    if (what->param_names == NULL || what->param_values == NULL ||
        what->n_params > MAX_INIT_PARAMS || def->n_params > MAX_INIT_PARAMS) {
        return odin3_api_invalid_why(fn, "bad parameter list (NULL, or more than 8 parameters)");
    }
    for (uint32_t i = 0; i < def->n_params; i++) {
        params[i] = def->params[i].dflt;
    }
    for (uint32_t i = 0; i < what->n_params; i++) {
        int found = find_int_param(def, what->param_names[i]);
        size_t index = found < 0 ? 0 : (size_t)found;
        if (found < 0 || seen[index]) {
            return odin3_api_invalid_why(fn, "not an integer parameter of the type, or repeated");
        }
        seen[index] = true;
        params[index] = odin3_value_int(what->param_values[i]);
    }
    *n_params = def->n_params;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_edit_add_node(odin3_design *design, uint32_t module,
                                              const odin3_edit_new *in, odin3_ref *node) {
    odin3_edit_new norm;
    const odin3_edit_new *what = &norm;
    odin3_module *mod = edit_module(design, module, "odin3_edit_add_node");
    odin3_node_spec spec = {0};
    odin3_value params[MAX_INIT_PARAMS];
    odin3_node_id id = {0};
    if (mod == NULL || node == NULL || check_what(in, &norm, "odin3_edit_add_node") != ODIN3_OK ||
        find_type(design, what->cell_type, &spec.type) != ODIN3_OK ||
        initial_params(odin3_celltype_get(design, spec.type), what, params, &spec.n_params) !=
            ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    spec.params = spec.n_params > 0 ? params : NULL;
    odin3_status st = make_node(design, mod, spec, what, &id);
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
static bool cover_blank(char chr) {
    return chr == ' ' || chr == '\t' || chr == '\r';
}

/* Copies the cover chars of text into out; false when a row is not row_len chars wide. */
static bool cover_rows(const char *text, size_t row_len, uint8_t *out, size_t *used) {
    size_t row_pos = 0;
    *used = 0;
    for (const char *chr = text;; chr++) {
        if (*chr == '\n' || *chr == '\0') {
            if (row_pos != 0 && row_pos != row_len) {
                return false;
            }
            row_pos = 0;
            if (*chr == '\0') {
                return true;
            }
        } else if (!cover_blank(*chr)) {
            if (row_pos == row_len) {
                return false;
            }
            out[(*used)++] = (uint8_t)*chr;
            row_pos++;
        }
    }
}

static odin3_status parse_cover(const char *text, uint32_t n_inputs, uint8_t **bits,
                                uint32_t *len) {
    *bits = NULL;
    *len = 0;
    if (text == NULL) {
        return odin3_api_invalid_why("odin3_edit_cover", "the cover text is NULL");
    }
    uint8_t *out = odin3_util_malloc(strlen(text) + 1);
    if (out == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t used = 0;
    if (!cover_rows(text, (size_t)n_inputs + 1, out, &used)) {
        odin3_util_free(out);
        return odin3_api_invalid_why("odin3_edit_cover", "a cover row has the wrong width");
    }
    *bits = out;
    *len = (uint32_t)used;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_edit_add_sop(odin3_design *design, uint32_t module,
                                             const odin3_edit_new *in, odin3_ref *node) {
    odin3_edit_new norm;
    const odin3_edit_new *what = &norm;
    odin3_module *mod = edit_module(design, module, "odin3_edit_add_sop");
    uint8_t *bits = NULL;
    uint32_t len = 0;
    if (mod == NULL || node == NULL || check_what(in, &norm, "odin3_edit_add_sop") != ODIN3_OK ||
        parse_cover(what->cover, what->n_inputs, &bits, &len) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t n_inputs = what->n_inputs;
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
        st = make_node(design, mod, spec, what, &id);
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

/* Marks the node of pin with an edit event of kind and tag. */
static odin3_status mark_pin_node(odin3_module *mod, odin3_pin_id pin, const char *kind,
                                  const char *tag) {
    odin3_node_id node = odin3_pin_node(mod, pin);
    return odin3_edit_event_add(mod, (odin3_objref){ODIN3_OBJ_NODE, node.v}, kind, tag);
}

static odin3_status disconnect_pin(odin3_design *design, odin3_ref pin, const char *tag) {
    odin3_module *mod = edit_pin_net(design, pin, NULL, "odin3_edit_disconnect");
    if (mod == NULL || check_tag(tag, "odin3_edit_disconnect") != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_pin_id pid = {pin.id};
    bool connected = odin3_net_valid(odin3_pin_net(mod, pid));
    odin3_status st = odin3_pin_disconnect(mod, pid);
    if (st == ODIN3_OK && connected) {
        st = mark_pin_node(mod, pid, ODIN3_EDIT_REMOVED, tag);
    }
    return st == ODIN3_OK ? after_edit(mod, "odin3_edit_disconnect") : st;
}

ODIN3_EXPORT odin3_status odin3_edit_disconnect(odin3_design *design, odin3_ref pin) {
    return disconnect_pin(design, pin, K_DEFAULT_TAG);
}

ODIN3_EXPORT odin3_status odin3_edit_disconnect_tagged(odin3_design *design, odin3_ref pin,
                                                       const char *tag) {
    return disconnect_pin(design, pin, tag);
}

static odin3_status rewire_pin(odin3_design *design, odin3_ref pin, odin3_ref net,
                               const char *tag) {
    odin3_module *mod = edit_pin_net(design, pin, &net, "odin3_edit_rewire");
    if (mod == NULL || check_tag(tag, "odin3_edit_rewire") != ODIN3_OK) {
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
    /*
     * Rollback order: disconnect from the old net, connect to the new one. If the connect fails
     * the pin is reconnected to the old net, so the connection is as before except that the pin
     * now sits at the end of the old net's pin array (pin order on a net is not part of the
     * netlist's meaning, but a writer may list the pins in the new order).
     */
    odin3_status st = odin3_pin_disconnect(mod, pid);
    if (st == ODIN3_OK) {
        st = odin3_pin_connect(mod, pid, nid);
        if (st != ODIN3_OK && old.v != 0) {
            (void)odin3_pin_connect(mod, pid, old);
        }
    }
    st = st == ODIN3_OK ? mark_pin_node(mod, pid, ODIN3_EDIT_REWIRED, tag) : st;
    return st == ODIN3_OK ? after_edit(mod, "odin3_edit_rewire") : st;
}

ODIN3_EXPORT odin3_status odin3_edit_rewire(odin3_design *design, odin3_ref pin, odin3_ref net) {
    return rewire_pin(design, pin, net, K_DEFAULT_TAG);
}

ODIN3_EXPORT odin3_status odin3_edit_rewire_tagged(odin3_design *design, odin3_ref pin,
                                                   odin3_ref net, const char *tag) {
    return rewire_pin(design, pin, net, tag);
}

/* --- removal -------------------------------------------------------------------------------- */

/* True when an output pin of node sits on a net that has other pins (removal would orphan them). */
static bool drives_shared_net(const odin3_module *mod, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(mod, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        odin3_net_id net = odin3_pin_net(mod, pin);
        if (net.v != 0 && odin3_pin_drives(mod, pin) && odin3_net_pins(mod, net).count > 1) {
            return true;
        }
    }
    return false;
}

ODIN3_EXPORT odin3_status odin3_edit_remove_node(odin3_design *design, odin3_ref node,
                                                 int allow_undriven) {
    odin3_module *mod = edit_module(design, node.module, "odin3_edit_remove_node");
    if (mod == NULL || odin3_api_ref(design, node, ODIN3_API_NODE) == NULL ||
        !odin3_node_live(mod, (odin3_node_id){node.id})) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (allow_undriven == 0 && drives_shared_net(mod, (odin3_node_id){node.id})) {
        return odin3_api_invalid_why("odin3_edit_remove_node",
                                     "the node drives a net with other pins; disconnect them or "
                                     "pass allow_undriven");
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

typedef struct attr_copy {
    uint32_t *keys;
    uint32_t count;
} attr_copy;

static odin3_status attr_count_visit(void *ctx, uint32_t key_str, const odin3_value *value) {
    (void)key_str;
    (void)value;
    ((attr_copy *)ctx)->count++;
    return ODIN3_OK;
}

static odin3_status attr_key_visit(void *ctx, uint32_t key_str, const odin3_value *value) {
    (void)value;
    attr_copy *copy = ctx;
    copy->keys[copy->count++] = key_str;
    return ODIN3_OK;
}

/* Copies every attribute of node old onto fresh (odin3_node_replace does not carry them). */
static odin3_status copy_attrs(odin3_module *mod, odin3_node_id old, odin3_node_id fresh) {
    const odin3_objref from = {ODIN3_OBJ_NODE, old.v};
    const odin3_objref to = {ODIN3_OBJ_NODE, fresh.v};
    attr_copy copy = {NULL, 0};
    (void)odin3_attr_foreach(mod, from, attr_count_visit, &copy);
    if (copy.count == 0) {
        return ODIN3_OK;
    }
    copy.keys = odin3_util_malloc((size_t)copy.count * sizeof(uint32_t));
    if (copy.keys == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    copy.count = 0;
    (void)odin3_attr_foreach(mod, from, attr_key_visit, &copy);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < copy.count; i++) {
        const odin3_value *value = odin3_attr_get(mod, from, copy.keys[i]);
        st = value == NULL ? ODIN3_OK : odin3_attr_set(mod, to, copy.keys[i], value);
    }
    odin3_util_free(copy.keys);
    return st;
}

/* The class of a twin's record: a 1:1 rewrite of the node (same level before and after). */
static const odin3_opclass twin_class = {ODIN3_DIR_REWRITE, ODIN3_LEVEL_WORD, ODIN3_LEVEL_WORD,
                                         false, 0};

/*
 * The provenance of a twin: a DERIVED record, in a new run named "edit" with the tag as its
 * arguments, whose parents are the old node's record (first, so the backward walk still reaches
 * the HDL source and its construct chain) and the edit's own IMPORTED record. A new run per twin
 * gives the twin its own operation. old_prov 0 (an object without provenance) leaves the edit
 * record as the only parent.
 */
static odin3_status twin_prov(odin3_design *design, odin3_prov_id old_prov, const char *tag,
                              odin3_prov_id *out) {
    uint32_t run_str = 0;
    uint32_t tag_str = 0;
    odin3_status st = check_tag(tag, "edit");
    st = st == ODIN3_OK ? intern_text(design, K_EDIT_RUN, &run_str) : st;
    if (st == ODIN3_OK) {
        st = intern_text(design, tag, &tag_str);
    }
    odin3_pass_ctx ctx = {design, {0}, 0};
    if (st == ODIN3_OK) {
        st = odin3_pass_run_begin_args(design, run_str, tag_str, &ctx);
    }
    const odin3_prov_origin origin = {NULL, 0, 0, 0};
    odin3_prov_id parents[2] = {old_prov, {0}};
    if (st == ODIN3_OK) {
        st = odin3_prov_imported(&ctx, &origin, &parents[1]);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_prov_begin_op_class(&ctx, &twin_class);
    odin3_prov_list list =
        old_prov.v == 0 ? (odin3_prov_list){&parents[1], 1} : (odin3_prov_list){parents, 2};
    return odin3_prov_derive(&ctx, list, out);
}

/*
 * Makes a twin of old with params, takes over its pins (odin3_node_replace), its name and its
 * attributes. The twin's provenance is derived from the old node's and the edit's (twin_prov).
 * *twin is set as soon as old has been replaced, so a caller keeps a handle even when the name or
 * attribute step then fails (the status is that failure).
 */
typedef struct twin_spec {
    const odin3_value *params;
    uint32_t n_params;
    const char *tag;
} twin_spec;

static odin3_status replace_node(odin3_module *mod, odin3_node_id old, const twin_spec *edit,
                                 odin3_ref *twin) {
    odin3_design *design = odin3_module_design(mod);
    odin3_node_spec spec = {odin3_node_type(mod, old), 0, {0}, edit->params, edit->n_params};
    odin3_node_id fresh = {0};
    uint32_t old_name = odin3_node_name(mod, old);
    odin3_status st = twin_prov(design, odin3_node_prov(mod, old), edit->tag, &spec.prov);
    if (st == ODIN3_OK) {
        st = odin3_node_create(mod, &spec, &fresh);
    }
    if (st == ODIN3_OK) {
        st = odin3_node_replace(mod, (odin3_node_pair){old, fresh});
        if (st != ODIN3_OK) {
            (void)odin3_node_delete(mod, fresh);
            return st;
        }
        *twin = (odin3_ref){odin3_module_id_of(mod).v, fresh.v};
    }
    if (st == ODIN3_OK && old_name != 0) {
        st = odin3_node_rename(mod, fresh, old_name);
    }
    st = st == ODIN3_OK ? copy_attrs(mod, old, fresh) : st;
    return st == ODIN3_OK ? odin3_edit_event_add(mod, (odin3_objref){ODIN3_OBJ_NODE, fresh.v},
                                                 ODIN3_EDIT_PARAM, edit->tag)
                          : st;
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
                                               const odin3_edit_new *in, odin3_ref *twin) {
    odin3_edit_new norm;
    const odin3_edit_new *what = &norm;
    odin3_module *mod = edit_module(design, node.module, "odin3_edit_set_cover");
    odin3_value params[SOP_PARAMS];
    uint32_t n_params = 0;
    uint8_t *bits = NULL;
    uint32_t len = 0;
    odin3_node_id nid = {node.id};
    if (twin == NULL || check_what(in, &norm, "odin3_edit_set_cover") != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    *twin = (odin3_ref){0, 0};
    if (mod == NULL || odin3_api_ref(design, node, ODIN3_API_NODE) == NULL ||
        !odin3_node_live(mod, nid) || !copy_node_params(mod, nid, SOP_COVER, params, &n_params) ||
        params[SOP_COVER].kind != ODIN3_VAL_COVER) {
        return odin3_api_invalid_why("odin3_edit_set_cover", "not a live node with a COVER");
    }
    uint32_t width = (uint32_t)params[SOP_WIDTH].i;
    if (parse_cover(what->cover, width, &bits, &len) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    params[SOP_COVER].bits = bits;
    params[SOP_COVER].len = len;
    odin3_status st = replace_node(mod, nid, &(twin_spec){params, n_params, what->tag}, twin);
    odin3_util_free(bits);
    return st == ODIN3_OK ? after_edit(mod, "odin3_edit_set_cover") : st;
}

ODIN3_EXPORT odin3_status odin3_edit_set_param_int(odin3_design *design, odin3_ref node,
                                                   const odin3_edit_new *in, odin3_ref *twin) {
    odin3_edit_new norm;
    const odin3_edit_new *what = &norm;
    odin3_module *mod = edit_module(design, node.module, "odin3_edit_set_param_int");
    odin3_value params[SOP_PARAMS];
    uint32_t n_params = 0;
    odin3_node_id nid = {node.id};
    if (twin == NULL || check_what(in, &norm, "odin3_edit_set_param_int") != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    *twin = (odin3_ref){0, 0};
    if (mod == NULL || odin3_api_ref(design, node, ODIN3_API_NODE) == NULL ||
        !odin3_node_live(mod, nid) || !copy_node_params(mod, nid, what->index, params, &n_params) ||
        params[what->index].kind != ODIN3_VAL_INT) {
        return odin3_api_invalid_why("odin3_edit_set_param_int", "not an integer parameter");
    }
    params[what->index] = odin3_value_int(what->value);
    odin3_status st = replace_node(mod, nid, &(twin_spec){params, n_params, what->tag}, twin);
    return st == ODIN3_OK ? after_edit(mod, "odin3_edit_set_param_int") : st;
}

/* --- reading the tag and the events ---------------------------------------------------------- */

static odin3_prov_id obj_prov(const odin3_module *mod, odin3_objref ref) {
    switch (ref.kind) {
    case ODIN3_OBJ_NODE:
        return odin3_node_prov(mod, (odin3_node_id){ref.id});
    case ODIN3_OBJ_NET:
        return odin3_net_prov(mod, (odin3_net_id){ref.id});
    case ODIN3_OBJ_WIRE:
        return odin3_wire_prov(mod, (odin3_wire_id){ref.id});
    default:
        return (odin3_prov_id){0};
    }
}

/* The tag of an event of obj ("added" first), as the design's own NUL-terminated string. */
static const char *event_tag(const odin3_design *design, const odin3_module *mod,
                             odin3_objref ref) {
    odin3_bytes tag = {NULL, 0};
    uint32_t str = 0;
    if (!odin3_edit_events_first_tag(mod, ref, &tag) ||
        !odin3_strtab_find(odin3_design_strtab(design), tag, &str)) {
        return NULL;
    }
    return odin3_strtab_get(odin3_design_strtab(design), str);
}

ODIN3_EXPORT odin3_status odin3_edit_get_tag(const odin3_design *design, odin3_obj obj,
                                             const char **tag) {
    odin3_objref ref;
    const odin3_module *mod = tag == NULL ? NULL : odin3_api_obj(design, obj, &ref);
    if (mod == NULL) {
        return odin3_api_invalid("odin3_edit_get_tag");
    }
    *tag = NULL;
    const odin3_prov_record *rec = odin3_prov_get(design, obj_prov(mod, ref));
    if (rec != NULL && rec->kind != ODIN3_PROV_SOURCE && rec->run.v != 0 &&
        strcmp(odin3_api_str(design, odin3_passrun_name(design, rec->run)), K_EDIT_RUN) == 0) {
        *tag = odin3_api_str(design, odin3_passrun_args(design, rec->run));
    } else if (ref.kind == ODIN3_OBJ_NODE || ref.kind == ODIN3_OBJ_NET) {
        *tag = event_tag(design, mod, ref); /* a design read back from an export */
    }
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_edit_get_events(const odin3_design *design, odin3_obj obj,
                                                const char **text) {
    odin3_objref ref;
    const odin3_module *mod = text == NULL ? NULL : odin3_api_obj(design, obj, &ref);
    if (mod == NULL) {
        return odin3_api_invalid("odin3_edit_get_events");
    }
    *text = odin3_edit_events_raw(mod, ref);
    return ODIN3_OK;
}
