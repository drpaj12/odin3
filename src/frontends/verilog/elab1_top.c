/*
 * elab1_top.c — stage 1's top selection (2D spec §3.2, EL-7), the definitions reached by the
 * specialization queue (EL-6: the taken-name errors when a definition is reached) and the info
 * line naming the modules left out.
 */
#include "frontends/verilog/elab1.h"

#include "ast/ast.h"
#include "ast/srcman.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The low half of a by_name value: the first definition of the chain + 1. */
static const uint64_t E1_LOW32 = UINT32_MAX;

static const char *text_of(const odin3_elab1 *e1, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(e1->design), str);
}

static odin3_elab1_defx *defx_at(odin3_elab1 *e1, uint32_t idx) {
    return odin3_vec_at(&e1->defx, idx);
}

static bool is_module(const odin3_elab_def *def) {
    return def->kind == ODIN3_DEF_MODULE || def->kind == ODIN3_DEF_BLACKBOX;
}

/* The first definition of name's chain (1-based, 0 = none). */
static uint32_t chain_head(const odin3_elab1 *e1, uint32_t name) {
    uint64_t ends = 0;
    return odin3_u64map_get(e1->by_name, name, &ends) ? (uint32_t)(ends & E1_LOW32) : 0;
}

static void latch(odin3_elab1 *e1, odin3_status st) {
    if (st != ODIN3_OK && e1->fatal == ODIN3_OK) {
        e1->fatal = st;
    }
}

/* --- a given top ----------------------------------------------------------------------------- */

/* A top as given: a module name and its library (0: any library). */
typedef struct e1_top_name {
    uint32_t module;
    uint32_t library;
} e1_top_name;

/* The modules a top names: the first design unit, the first library unit (1-based, 0 = none),
 * and how many design units match. */
typedef struct e1_named {
    uint32_t design;
    uint32_t libunit;
    uint32_t n_design;
} e1_named;

static e1_named find_named(odin3_elab1 *e1, e1_top_name want) {
    e1_named found = {0, 0, 0};
    for (uint32_t at = chain_head(e1, want.module); at != 0; at = defx_at(e1, at - 1)->next) {
        const odin3_elab_def *def = odin3_elab1_def(e1, at - 1);
        if (!is_module(def) || (want.library != 0 && def->library != want.library)) {
            continue;
        }
        if (!def->library_unit) {
            found.design = found.design != 0 ? found.design : at;
            found.n_design++;
        } else if (found.libunit == 0) {
            found.libunit = at;
        }
    }
    return found;
}

/* "[lib.]mod" split at the first '.' when both pieces are known strings (otherwise zero: the
 * whole text is then looked up as a module name). */
static e1_top_name split_top(const odin3_elab1 *e1, const char *text) {
    e1_top_name split = {0, 0};
    const char *dot = strchr(text, '.');
    if (dot == NULL || dot == text || dot[1] == '\0') {
        return split;
    }
    const odin3_strtab *tab = odin3_design_strtab(e1->design);
    uint32_t lib = 0;
    uint32_t mod = 0;
    if (odin3_strtab_find(tab, (odin3_bytes){text, (size_t)(dot - text)}, &lib) &&
        odin3_strtab_find(tab, odin3_bytes_cstr(dot + 1), &mod)) {
        split = (e1_top_name){mod, lib};
    }
    return split;
}

/* ambiguous_top for a given name defined in several libraries: 'lib.mod' for each. PARSE once
 * reported; NO_MEMORY (latched, nothing logged) when the list cannot be built. */
static odin3_status ambiguous_given(odin3_elab1 *e1, uint32_t name, const char *text) {
    odin3_strbuf list;
    odin3_strbuf_init(&list);
    odin3_status st = ODIN3_OK;
    uint32_t count = 0;
    for (uint32_t at = chain_head(e1, name); st == ODIN3_OK && at != 0;
         at = defx_at(e1, at - 1)->next) {
        const odin3_elab_def *def = odin3_elab1_def(e1, at - 1);
        if (is_module(def) && !def->library_unit) {
            st = odin3_strbuf_appendf(&list, "%s'%s.%s'", count++ > 0 ? ", " : "",
                                      text_of(e1, def->library), text);
        }
    }
    latch(e1, st);
    if (st == ODIN3_OK) {
        odin3_elab1_error(e1, e1->opts.top_at,
                          "ambiguous_top: '%s' is defined in several libraries: %s", text,
                          list.data);
    }
    odin3_strbuf_free(&list);
    return st != ODIN3_OK ? st : ODIN3_ERR_PARSE;
}

static odin3_status given_top(odin3_elab1 *e1, uint32_t *def) {
    const char *text = text_of(e1, e1->opts.top);
    e1_top_name split = split_top(e1, text);
    e1_named found = split.library != 0 ? find_named(e1, split) : (e1_named){0, 0, 0};
    if (found.design == 0 && found.libunit == 0) {
        found = find_named(e1, (e1_top_name){e1->opts.top, 0});
        if (found.n_design > 1) {
            return ambiguous_given(e1, e1->opts.top, text);
        }
    }
    uint32_t at = found.design != 0 ? found.design : found.libunit;
    if (at == 0) {
        odin3_elab1_error(e1, e1->opts.top_at, "unknown_top: '%s' is not a defined module", text);
        return ODIN3_ERR_PARSE;
    }
    if (defx_at(e1, at - 1)->shadows_cell) {
        /* the IR names a module and a cell type alike (IR-11): the unit has no name to give */
        odin3_elab1_error(e1, e1->opts.top_at,
                          "unknown_top: '%s' is a library unit with the name of a registered cell "
                          "type, so it cannot be the top; rename it",
                          text);
        return ODIN3_ERR_PARSE;
    }
    if (odin3_elab1_def(e1, at - 1)->kind == ODIN3_DEF_BLACKBOX) {
        /* EL-4: a black box is never a MODULE, so it cannot head the UNIT (review m1). */
        odin3_elab1_error(e1, e1->opts.top_at,
                          "unknown_top: '%s' is a black box ((* blackbox *)), not a module to "
                          "elaborate",
                          text);
        return ODIN3_ERR_PARSE;
    }
    *def = at - 1;
    return ODIN3_OK;
}

/* --- the automatic top ----------------------------------------------------------------------- */

/* The candidates (design-unit modules no module instantiates) as 'lib.mod' in definition
 * order; *first the first one, *modules how many design-unit modules there are. */
typedef struct e1_candidates {
    odin3_strbuf list;
    uint32_t count;
    uint32_t first;
    uint32_t modules;
} e1_candidates;

static odin3_status gather_candidates(const odin3_elab1 *e1, e1_candidates *cand) {
    odin3_status st = ODIN3_OK;
    uint32_t defs = odin3_elab1_def_count(e1);
    for (uint32_t idx = 0; st == ODIN3_OK && idx < defs; idx++) {
        const odin3_elab_def *def = odin3_elab1_def(e1, idx);
        if (def->kind != ODIN3_DEF_MODULE || def->library_unit) {
            continue;
        }
        cand->modules++;
        if (odin3_u64map_get(e1->instantiated, def->name, NULL)) {
            continue;
        }
        st = odin3_strbuf_appendf(&cand->list, "%s'%s.%s'", cand->count > 0 ? ", " : "",
                                  text_of(e1, def->library), text_of(e1, def->name));
        cand->first = cand->count == 0 ? idx : cand->first;
        cand->count++;
    }
    return st;
}

static odin3_status auto_top(odin3_elab1 *e1, uint32_t *def) {
    e1_candidates cand = {.count = 0, .first = 0, .modules = 0};
    odin3_strbuf_init(&cand.list);
    odin3_status st = gather_candidates(e1, &cand);
    const odin3_loc none = {0};
    if (st == ODIN3_OK && cand.count == 1) {
        *def = cand.first;
        odin3_elab1_info(e1, none,
                         "top module %s chosen: the only module no other module instantiates",
                         cand.list.data);
    } else if (st == ODIN3_OK && cand.count > 1) {
        odin3_elab1_error(e1, none,
                          "ambiguous_top: no top given and several modules are not instantiated: "
                          "%s",
                          cand.list.data);
        st = ODIN3_ERR_PARSE;
    } else if (st == ODIN3_OK) {
        odin3_elab1_error(e1, none, "no_top: no top given and %s",
                          cand.modules > 0 ? "every module is instantiated by another"
                                           : "no module is defined");
        st = ODIN3_ERR_PARSE;
    }
    odin3_strbuf_free(&cand.list);
    return st;
}

odin3_status odin3_elab1_select_top(odin3_elab1 *e1, uint32_t *def) {
    *def = 0;
    return e1->opts.top != 0 ? given_top(e1, def) : auto_top(e1, def);
}

/* --- reachability ---------------------------------------------------------------------------- */

/* The run of the earlier read that made the module named name (0 when unknown). */
static uint32_t module_run(odin3_elab1 *e1, uint32_t name) {
    uint32_t end = odin3_design_module_end(e1->design);
    for (uint32_t id = 1; id < end; id++) {
        const odin3_module *module = odin3_module_get(e1->design, (odin3_module_id){id});
        if (module != NULL && odin3_module_name(module) == name) {
            const odin3_prov_record *rec = odin3_prov_get(e1->design, odin3_module_prov(module));
            return rec != NULL ? rec->run.v : 0;
        }
    }
    return 0;
}

/* The taken-name error of a reached MODULE (EL-6), at its MODULE. */
static void report_taken(odin3_elab1 *e1, const odin3_elab_def *def, uint8_t taken) {
    odin3_loc at = odin3_ast_loc(e1->parsed, def->node);
    const char *name = text_of(e1, def->name);
    if (taken == ODIN3_TAKEN_MODULE) {
        odin3_elab1_error(e1, at,
                          "duplicate_module: module '%s' is already defined by an earlier read "
                          "(run %" PRIu32 ")",
                          name, module_run(e1, def->name));
    } else if (taken == ODIN3_TAKEN_BUILTIN) {
        odin3_elab1_error(e1, at,
                          "module '%s' has the name of built-in cell type '%s'; rename the "
                          "module",
                          name, name);
    } else {
        odin3_elab1_error(e1, at,
                          "module '%s' has the name of cell type '%s' registered by an earlier "
                          "read (%s); rename the module",
                          name, name, taken == ODIN3_TAKEN_HARD ? "hard cell" : "black box");
    }
}

bool odin3_elab1_reach_def(odin3_elab1 *e1, uint32_t def) {
    const odin3_elab_def *rec = odin3_elab1_def(e1, def);
    odin3_elab1_defx *ext = defx_at(e1, def);
    ext->reached = true;
    if (ext->taken != ODIN3_TAKEN_NONE) {
        if (!ext->reported) {
            ext->reported = true;
            report_taken(e1, rec, ext->taken);
        }
        return false; /* what depends on it is skipped silently (EL-44) */
    }
    /* a black box is specialized too (EL-11: its ports per specialization; its body ignored) */
    return rec->kind == ODIN3_DEF_MODULE || rec->kind == ODIN3_DEF_BLACKBOX;
}

odin3_status odin3_elab1_not_elaborated(odin3_elab1 *e1) {
    odin3_strbuf list;
    odin3_strbuf_init(&list);
    odin3_status st = ODIN3_OK;
    uint32_t defs = odin3_elab1_def_count(e1);
    uint32_t count = 0;
    for (uint32_t idx = 0; st == ODIN3_OK && idx < defs; idx++) {
        const odin3_elab_def *def = odin3_elab1_def(e1, idx);
        if (is_module(def) && !def->library_unit && !defx_at(e1, idx)->reached) {
            st = odin3_strbuf_appendf(&list, "%s'%s'", count++ > 0 ? ", " : "",
                                      text_of(e1, def->name));
        }
    }
    if (st == ODIN3_OK && count > 0) {
        odin3_elab1_info(e1, (odin3_loc){0}, "not instantiated under the top, not elaborated: %s",
                         list.data);
    }
    odin3_strbuf_free(&list);
    return st;
}
