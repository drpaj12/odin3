/*
 * levelize.c — the levelize pass (Phase 6 spec 6.4, V-1 to V-8): counts the levels of logic from
 * the combinational point of view and writes them as attributes. `levelize [--key NAME]`, default
 * key `odin3_level`: every cell (port nodes excluded) and every net of every module gets the INT
 * attribute NAME; a cell cut to break a combinational loop also gets `NAME_cut` = 1.
 *
 * Levels only: no delays, no timing arcs, no criticality (Peter, 2026-10-20). The definition and
 * the loop cut are in sweep.c. Structure and provenance never change, only attributes are
 * written, so a second run gives the same result.
 *
 * The key guard (V-1): a NAME that starts with `odin3_` is refused unless it is exactly
 * `odin3_level`, and so is any existing attribute NAME or `NAME_cut` on a cell or net that is not
 * an INT, since levelize only writes INTs. The word view is refused (a `$dff` and the like carry
 * no sequential flag in the registry, so a word-view result would be wrong).
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/levelize/levelize_int.h"
#include "passes/manager.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char k_default_key[] = "odin3_level";
static const char k_reserved[] = "odin3_";
static const char k_cut_suffix[] = "_cut";

enum { NAME_BUF = 32 };

/* The interned keys of a run. */
typedef struct lv_keys {
    uint32_t level;
    uint32_t cut;
} lv_keys;

/* What a run adds up over the modules. */
typedef struct lv_totals {
    uint32_t modules;
    uint64_t cells;
    uint32_t max_level;
    uint32_t cuts;
} lv_totals;

/* --- options --------------------------------------------------------------------------------- */

/* Parses `[--key NAME]` into key (a NUL-terminated copy). INVALID_ARG (logged) on a bad option. */
static odin3_status parse_args(odin3_bytes args, odin3_strbuf *key) {
    odin3_status st = odin3_strbuf_append(key, odin3_bytes_cstr(k_default_key));
    for (odin3_bytes word = odin3_pass_arg_next(&args); st == ODIN3_OK && word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        odin3_bytes value =
            odin3_pass_arg_is(word, "--key") ? odin3_pass_arg_next(&args) : (odin3_bytes){NULL, 0};
        if (!odin3_pass_arg_is(word, "--key") || value.ptr == NULL || value.len == 0) {
            odin3_log(ODIN3_LOG_ERROR, "levelize: usage: levelize [--key NAME]");
            return ODIN3_ERR_INVALID_ARG;
        }
        odin3_strbuf_clear(key);
        st = odin3_strbuf_append(key, value);
    }
    return st;
}

static odin3_status check_key(const char *key) {
    bool reserved = strncmp(key, k_reserved, strlen(k_reserved)) == 0;
    if (reserved && strcmp(key, k_default_key) != 0) {
        odin3_log(ODIN3_LOG_ERROR,
                  "levelize: --key '%s': the prefix 'odin3_' is reserved for Odin III keys "
                  "(only '%s' is allowed)",
                  key, k_default_key);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

static odin3_status intern_keys(odin3_design *design, const char *key, lv_keys *keys) {
    odin3_strbuf cut;
    odin3_strbuf_init(&cut);
    odin3_status st = odin3_strbuf_appendf(&cut, "%s%s", key, k_cut_suffix);
    st = st == ODIN3_OK ? odin3_design_intern(design, odin3_bytes_cstr(key), &keys->level) : st;
    st = st == ODIN3_OK ? odin3_design_intern(design, odin3_bytes_cstr(cut.data), &keys->cut) : st;
    odin3_strbuf_free(&cut);
    return st;
}

/* --- the checks before anything is written (V-1) ------------------------------------------------
 */

static const char *module_text(const odin3_module *module) {
    return odin3_strtab_get(odin3_design_strtab(odin3_module_design(module)),
                            odin3_module_name(module));
}

/* True when obj carries a key attribute that levelize did not write (not an INT). */
static bool foreign_attr(const odin3_module *module, odin3_objref obj, uint32_t key) {
    const odin3_value *val = odin3_attr_get(module, obj, key);
    return val != NULL && val->kind != ODIN3_VAL_INT;
}

static odin3_status refuse_attr(const odin3_module *module, odin3_objref obj, const char *key) {
    const odin3_strtab *tab = odin3_design_strtab(odin3_module_design(module));
    char fallback[NAME_BUF];
    uint32_t name = 0;
    if (obj.kind == ODIN3_OBJ_NODE) {
        name = odin3_node_name(module, (odin3_node_id){obj.id});
        (void)snprintf(fallback, sizeof fallback, "$c%u", (unsigned)obj.id);
    } else {
        name = odin3_net_name(module, (odin3_net_id){obj.id});
        (void)snprintf(fallback, sizeof fallback, "$n%u", (unsigned)obj.id);
    }
    odin3_log(ODIN3_LOG_ERROR,
              "levelize: module '%s': %s '%s' already has the attribute '%s' (not written by "
              "levelize); use another --key",
              module_text(module), obj.kind == ODIN3_OBJ_NODE ? "cell" : "net",
              name != 0 ? odin3_strtab_get(tab, name) : fallback, key);
    return ODIN3_ERR_INVALID_ARG;
}

static odin3_status check_node(const odin3_module *module, uint32_t id, const lv_keys *keys,
                               const char *key) {
    const odin3_design *design = odin3_module_design(module);
    const odin3_celltype_def *def =
        odin3_celltype_get(design, odin3_node_type(module, (odin3_node_id){id}));
    if (def == NULL || def->gran == ODIN3_GRAN_PORT) {
        return ODIN3_OK;
    }
    if (def->gran == ODIN3_GRAN_WORD) {
        odin3_log(ODIN3_LOG_ERROR,
                  "levelize: module '%s' has the word-level cell '%s'; run proc and lower first",
                  module_text(module), def->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_objref obj = {ODIN3_OBJ_NODE, id};
    return foreign_attr(module, obj, keys->level) || foreign_attr(module, obj, keys->cut)
               ? refuse_attr(module, obj, key)
               : ODIN3_OK;
}

static odin3_status check_module(const odin3_module *module, const lv_keys *keys, const char *key) {
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_module_node_end(module); id++) {
        st = odin3_node_live(module, (odin3_node_id){id}) ? check_node(module, id, keys, key) : st;
    }
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_module_net_end(module); id++) {
        odin3_objref obj = {ODIN3_OBJ_NET, id};
        bool bad = odin3_net_live(module, (odin3_net_id){id}) &&
                   (foreign_attr(module, obj, keys->level) || foreign_attr(module, obj, keys->cut));
        st = bad ? refuse_attr(module, obj, key) : st;
    }
    return st;
}

/* --- the attributes ----------------------------------------------------------------------------
 */

/* One INT attribute to write. */
typedef struct lv_attr {
    uint32_t key;
    int64_t value;
} lv_attr;

static odin3_status write_level(odin3_module *module, odin3_objref obj, lv_attr attr) {
    odin3_value val = odin3_value_int(attr.value);
    return odin3_attr_set(module, obj, attr.key, &val);
}

static odin3_status write_cell(const lv_mod *lv, uint32_t id, const lv_keys *keys) {
    odin3_objref obj = {ODIN3_OBJ_NODE, id};
    odin3_status st = write_level(lv->module, obj, (lv_attr){keys->level, lv->clevel[id]});
    if (st == ODIN3_OK && (lv->state[id] & LV_CUT) != 0) {
        st = write_level(lv->module, obj, (lv_attr){keys->cut, 1});
    } else if (st == ODIN3_OK) {
        st = odin3_attr_remove(lv->module, obj, keys->cut);
    }
    return st;
}

static odin3_status write_module(const lv_mod *lv, const lv_keys *keys) {
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < lv->n_nodes; id++) {
        st = lv->role[id] != LV_SKIP ? write_cell(lv, id, keys) : st;
    }
    for (uint32_t id = 1; st == ODIN3_OK && id < lv->n_nets; id++) {
        st = odin3_net_live(lv->module, (odin3_net_id){id})
                 ? write_level(lv->module, (odin3_objref){ODIN3_OBJ_NET, id},
                               (lv_attr){keys->level, lv->nlevel[id]})
                 : st;
    }
    return st;
}

/* --- the run -----------------------------------------------------------------------------------
 */

static odin3_status level_module(odin3_module *module, const lv_keys *keys, lv_totals *tot) {
    lv_mod lv;
    odin3_status st = lv_open(&lv, odin3_module_design(module), module);
    st = st == ODIN3_OK ? lv_sweep(&lv) : st;
    st = st == ODIN3_OK ? write_module(&lv, keys) : st;
    if (st == ODIN3_OK && lv.n_inst > 0) {
        odin3_log(ODIN3_LOG_INFO,
                  "levelize: module '%s': %u instance(s) of design modules count as one "
                  "combinational cell each; run flatten first for true depths",
                  module_text(module), (unsigned)lv.n_inst);
    }
    tot->modules++;
    tot->cells += lv.n_cells;
    tot->cuts += lv.n_cut;
    tot->max_level = lv.max_level > tot->max_level ? lv.max_level : tot->max_level;
    lv_free(&lv);
    return st;
}

static odin3_status levelize_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                 void *user) {
    (void)ctx;
    (void)user;
    odin3_strbuf key;
    odin3_strbuf_init(&key);
    lv_keys keys = {0, 0};
    lv_totals tot = {0, 0, 0, 0};
    odin3_status st = parse_args(args, &key);
    st = st == ODIN3_OK ? check_key(key.data) : st;
    st = st == ODIN3_OK ? intern_keys(design, key.data, &keys) : st;
    uint32_t end = odin3_design_module_end(design);
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        st = check_module(odin3_module_get(design, (odin3_module_id){id}), &keys, key.data);
    }
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        st = level_module(odin3_module_get(design, (odin3_module_id){id}), &keys, &tot);
    }
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO, "levelize: %u modules, %llu cells, max level %u, %u loops cut",
                  (unsigned)tot.modules, (unsigned long long)tot.cells, (unsigned)tot.max_level,
                  (unsigned)tot.cuts);
    }
    odin3_strbuf_free(&key);
    return st;
}

const odin3_pass_def odin3_pass_levelize = {
    "levelize",
    "levelize [--key NAME]: count the levels of combinational logic of every module as the "
    "attribute NAME (default odin3_level) on every cell and net (attributes only)",
    levelize_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
