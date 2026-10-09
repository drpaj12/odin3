/* builtin.c — the built-in passes: read_blif, write_blif, check, compact, stats, hierarchy. */
#include "backends/blif/writer.h"
#include "frontends/blif/reader.h"
#include "ir/celltype.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* --- argument helpers ----------------------------------------------------------------------- */

/* The single word of args, NUL-terminated in buf. INVALID_ARG (logged) unless exactly one. */
static odin3_status one_path(const char *pass, odin3_bytes args, odin3_strbuf *buf) {
    odin3_bytes word = odin3_pass_arg_next(&args);
    if (word.len == 0 || odin3_pass_arg_next(&args).len > 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s: expects one path", pass);
        return ODIN3_ERR_INVALID_ARG;
    }
    return odin3_strbuf_append(buf, word);
}

static odin3_status no_args(const char *pass, odin3_bytes args) {
    if (odin3_pass_arg_next(&args).len > 0) {
        odin3_log(ODIN3_LOG_ERROR, "%s: takes no arguments", pass);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

static odin3_status unknown_arg(const char *pass, odin3_bytes word) {
    odin3_log(ODIN3_LOG_ERROR, "%s: unknown argument '%.*s'", pass, (int)word.len,
              (const char *)word.ptr);
    return ODIN3_ERR_INVALID_ARG;
}

static const char *str_of(const odin3_design *design, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(design), str);
}

static const char *module_name(odin3_design *design, odin3_module_id id) {
    return str_of(design, odin3_module_name(odin3_module_get(design, id)));
}

/* --- top selection (DESIGN §4.0, PHASE1 #18) ------------------------------------------------ */

/* Sets the top to the module named name. INVALID_ARG (logged) when there is none. */
static odin3_status select_named(odin3_design *design, odin3_bytes name) {
    uint32_t str = 0;
    bool known = odin3_strtab_find(odin3_design_strtab(design), name, &str);
    for (uint32_t i = 1; known && i < odin3_design_module_end(design); i++) {
        if (odin3_module_name(odin3_module_get(design, (odin3_module_id){i})) == str) {
            odin3_log(ODIN3_LOG_INFO, "hierarchy: top %s", str_of(design, str));
            return odin3_design_set_top(design, (odin3_module_id){i});
        }
    }
    odin3_log(ODIN3_LOG_ERROR, "hierarchy: no module named '%.*s'", (int)name.len,
              (const char *)name.ptr);
    return ODIN3_ERR_INVALID_ARG;
}

/* The highest cell-type ID of a module (the size of the type -> module map). */
static uint32_t max_module_type(odin3_design *design) {
    uint32_t max = 0;
    for (uint32_t i = 1; i < odin3_design_module_end(design); i++) {
        odin3_celltype_id type =
            odin3_module_celltype(odin3_module_get(design, (odin3_module_id){i}));
        max = type.v > max ? type.v : max;
    }
    return max;
}

/* Marks used[m] for every module m that a live node of some module instantiates. */
static void mark_instantiated(odin3_design *design, const uint32_t *owner, uint32_t n_types,
                              bool *used) {
    for (uint32_t i = 1; i < odin3_design_module_end(design); i++) {
        const odin3_module *module = odin3_module_get(design, (odin3_module_id){i});
        for (uint32_t j = 1; j < odin3_module_node_end(module); j++) {
            odin3_node_id node = {j};
            uint32_t type = odin3_node_type(module, node).v;
            if (odin3_node_live(module, node) && type < n_types && owner[type] != 0) {
                used[owner[type]] = true;
            }
        }
    }
}

/* Logs the candidate list (zero or several modules no other module instantiates). */
static odin3_status report_candidates(odin3_design *design, const bool *used) {
    odin3_strbuf list;
    odin3_strbuf_init(&list);
    uint32_t count = 0;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_design_module_end(design); i++) {
        if (!used[i]) {
            st = odin3_strbuf_appendf(&list, "%s%s", count > 0 ? ", " : "",
                                      module_name(design, (odin3_module_id){i}));
            count++;
        }
    }
    if (st == ODIN3_OK && count == 0) {
        odin3_log(ODIN3_LOG_ERROR,
                  "hierarchy: 0 top candidates: every module is instantiated by another");
    } else if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "hierarchy: %u top candidates: %s (choose one with --top)",
                  count, list.data);
    }
    odin3_strbuf_free(&list);
    return st == ODIN3_OK ? ODIN3_ERR_INVALID_ARG : st;
}

/* The single module no other module instantiates, from the used[] marks; none otherwise. */
static odin3_module_id single_candidate(const odin3_design *design, const bool *used) {
    odin3_module_id found = {0};
    for (uint32_t i = 1; i < odin3_design_module_end(design); i++) {
        if (!used[i] && found.v != 0) {
            return (odin3_module_id){0};
        }
        found = used[i] ? found : (odin3_module_id){i};
    }
    return found;
}

/* Sets the top to the single uninstantiated module; zero or several is an error listing them. */
static odin3_status select_auto(odin3_design *design) {
    uint32_t n_types = max_module_type(design) + 1;
    uint32_t n_modules = odin3_design_module_end(design);
    uint32_t *owner = odin3_util_calloc((size_t)n_types * sizeof *owner);
    bool *used = odin3_util_calloc((size_t)n_modules * sizeof *used);
    odin3_status st = owner == NULL || used == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
    for (uint32_t i = 1; st == ODIN3_OK && i < n_modules; i++) {
        owner[odin3_module_celltype(odin3_module_get(design, (odin3_module_id){i})).v] = i;
    }
    if (st == ODIN3_OK) {
        mark_instantiated(design, owner, n_types, used);
        odin3_module_id top = single_candidate(design, used);
        if (odin3_module_valid(top)) {
            odin3_log(ODIN3_LOG_INFO, "hierarchy: top %s", module_name(design, top));
            st = odin3_design_set_top(design, top);
        } else {
            st = report_candidates(design, used);
        }
    }
    odin3_util_free(owner);
    odin3_util_free(used);
    return st;
}

/* --- read_blif, write_blif ------------------------------------------------------------------- */

static odin3_status read_blif_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args) {
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    odin3_status st = one_path("read_blif", args, &path);
    if (st == ODIN3_OK) {
        st = odin3_blif_read_in(ctx, path.data);
    }
    const char *top = odin3_pass_get_options().top;
    if (st == ODIN3_OK && top != NULL) {
        st = select_named(design, odin3_bytes_cstr(top));
    }
    odin3_strbuf_free(&path);
    return st;
}

static odin3_status write_blif_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args) {
    (void)ctx;
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    odin3_status st = one_path("write_blif", args, &path);
    odin3_module_id top = odin3_design_top(design);
    if (st == ODIN3_OK && odin3_module_valid(top) && top.v != 1) {
        odin3_log(ODIN3_LOG_WARN,
                  "write_blif: the top '%s' is not the first module; BLIF reads '%s' as the top",
                  module_name(design, top), module_name(design, (odin3_module_id){1}));
    }
    if (st == ODIN3_OK) {
        st = odin3_blif_write(design, path.data);
    }
    odin3_strbuf_free(&path);
    return st;
}

/* --- check, compact -------------------------------------------------------------------------- */

static odin3_status check_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args) {
    (void)ctx;
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.len > 0;
         word = odin3_pass_arg_next(&args)) {
        if (!odin3_pass_arg_is(word, "--fast")) {
            return unknown_arg("check", word);
        }
        opts.level = ODIN3_CHECK_FAST;
    }
    return odin3_check_design(design, opts);
}

static odin3_status compact_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args) {
    (void)ctx;
    odin3_status st = no_args("compact", args);
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_design_module_end(design); i++) {
        st = odin3_module_compact(odin3_module_get(design, (odin3_module_id){i}), NULL);
    }
    return st;
}

/* --- stats ----------------------------------------------------------------------------------- */

/* One row of a module's cell-type histogram. */
typedef struct stats_row {
    const char *type;
    uint32_t count;
} stats_row;

static int row_cmp(const void *lhs, const void *rhs) {
    return strcmp(((const stats_row *)lhs)->type, ((const stats_row *)rhs)->type);
}

/* counts[type] += 1 for every live node; grows counts as needed. */
static odin3_status count_types(const odin3_module *module, odin3_vec *counts) {
    for (uint32_t i = 1; i < odin3_module_node_end(module); i++) {
        odin3_node_id node = {i};
        uint32_t type = odin3_node_type(module, node).v;
        while (odin3_node_live(module, node) && counts->len <= type) {
            uint32_t *slot = odin3_vec_push(counts);
            if (slot == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            *slot = 0;
        }
        if (odin3_node_live(module, node)) {
            (*(uint32_t *)odin3_vec_at(counts, type))++;
        }
    }
    return ODIN3_OK;
}

/* Logs the module's cell-type histogram, by type name. */
static odin3_status log_histogram(const odin3_design *design, const char *name,
                                  const odin3_vec *counts, odin3_vec *rows) {
    odin3_vec_clear(rows);
    for (uint32_t i = 1; i < counts->len; i++) {
        uint32_t count = *(const uint32_t *)odin3_vec_cat(counts, i);
        stats_row *row = count > 0 ? odin3_vec_push(rows) : NULL;
        if (count > 0 && row == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        if (row != NULL) {
            *row = (stats_row){odin3_celltype_get(design, (odin3_celltype_id){i})->name, count};
        }
    }
    if (rows->len > 0) {
        qsort(rows->data, rows->len, sizeof(stats_row), row_cmp);
    }
    for (size_t i = 0; i < rows->len; i++) {
        const stats_row *row = odin3_vec_cat(rows, i);
        odin3_log(ODIN3_LOG_INFO, "stats: module %s: cell %s %u", name, row->type, row->count);
    }
    return ODIN3_OK;
}

static void log_module_counts(const odin3_module *module, const char *name) {
    uint32_t nodes = 0;
    uint32_t nets = 0;
    uint32_t wires = 0;
    for (uint32_t i = 1; i < odin3_module_node_end(module); i++) {
        nodes += odin3_node_live(module, (odin3_node_id){i}) ? 1 : 0;
    }
    for (uint32_t i = 1; i < odin3_module_net_end(module); i++) {
        nets += odin3_net_live(module, (odin3_net_id){i}) ? 1 : 0;
    }
    for (uint32_t i = 1; i < odin3_module_wire_end(module); i++) {
        wires += odin3_wire_live(module, (odin3_wire_id){i}) ? 1 : 0;
    }
    odin3_log(ODIN3_LOG_INFO, "stats: module %s: ports %u, nodes %u, nets %u, wires %u", name,
              odin3_module_port_count(module), nodes, nets, wires);
}

static odin3_status stats_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args) {
    (void)ctx;
    odin3_status st = no_args("stats", args);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_module_id top = odin3_design_top(design);
    odin3_log(ODIN3_LOG_INFO, "stats: design: modules %u, top %s",
              odin3_design_module_end(design) - 1,
              odin3_module_valid(top) ? module_name(design, top) : "(none)");
    odin3_vec counts;
    odin3_vec rows;
    odin3_vec_init(&counts, sizeof(uint32_t));
    odin3_vec_init(&rows, sizeof(stats_row));
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_design_module_end(design); i++) {
        const odin3_module *module = odin3_module_get(design, (odin3_module_id){i});
        const char *name = module_name(design, (odin3_module_id){i});
        log_module_counts(module, name);
        odin3_vec_clear(&counts);
        st = count_types(module, &counts);
        if (st == ODIN3_OK) {
            st = log_histogram(design, name, &counts, &rows);
        }
    }
    odin3_vec_free(&counts);
    odin3_vec_free(&rows);
    return st;
}

/* --- hierarchy ------------------------------------------------------------------------------- */

/* hierarchy's arguments: --top <name> and -auto. */
typedef struct hier_args {
    odin3_bytes top; /* len 0: not given */
    bool auto_top;
} hier_args;

static odin3_status parse_hier_args(odin3_bytes args, hier_args *out) {
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.len > 0;
         word = odin3_pass_arg_next(&args)) {
        if (odin3_pass_arg_is(word, "-auto")) {
            out->auto_top = true;
        } else if (!odin3_pass_arg_is(word, "--top")) {
            return unknown_arg("hierarchy", word);
        } else {
            out->top = odin3_pass_arg_next(&args);
            if (out->top.len == 0) {
                odin3_log(ODIN3_LOG_ERROR, "hierarchy: --top expects a module name");
                return ODIN3_ERR_INVALID_ARG;
            }
        }
    }
    return ODIN3_OK;
}

/*
 * Top selection: hierarchy's --top, else the CLI's --top option, else the top already set (unless
 * -auto), else the single module no other module instantiates.
 */
static odin3_status hierarchy_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args) {
    (void)ctx;
    hier_args parsed = {0};
    odin3_status st = parse_hier_args(args, &parsed);
    if (st != ODIN3_OK) {
        return st;
    }
    if (odin3_design_module_end(design) <= 1) {
        odin3_log(ODIN3_LOG_ERROR, "hierarchy: the design has no modules");
        return ODIN3_ERR_INVALID_ARG;
    }
    const char *option_top = odin3_pass_get_options().top;
    if (parsed.top.len == 0 && option_top != NULL) {
        parsed.top = odin3_bytes_cstr(option_top);
    }
    if (parsed.top.len > 0) {
        return select_named(design, parsed.top);
    }
    odin3_module_id top = odin3_design_top(design);
    if (odin3_module_valid(top) && !parsed.auto_top) {
        odin3_log(ODIN3_LOG_INFO, "hierarchy: top %s", module_name(design, top));
        return ODIN3_OK;
    }
    return select_auto(design);
}

/* --- the table ------------------------------------------------------------------------------- */

static const odin3_pass_def READ_BLIF = {
    "read_blif", "read_blif <file>: read a BLIF netlist into an empty design (first model = top)",
    read_blif_run};
static const odin3_pass_def WRITE_BLIF = {
    "write_blif", "write_blif <file>: write the design as BLIF", write_blif_run};
static const odin3_pass_def CHECK = {
    "check", "check [--fast]: check the IR invariants (IR.md section 9)", check_run};
static const odin3_pass_def COMPACT = {
    "compact", "compact: renumber every module densely, freeing dead objects (IR-6)", compact_run};
static const odin3_pass_def STATS = {
    "stats", "stats: log modules, top, and per module ports/nodes/nets/wires and cell types",
    stats_run};
static const odin3_pass_def HIERARCHY = {
    "hierarchy",
    "hierarchy [--top <name>] [-auto]: set the top module (named, or the one no module "
    "instantiates)",
    hierarchy_run};

/*
 * Registry order. read_techlib registers here when the 1G tech library merges (ruling in the 1D
 * plan ledger): it reads a .o3lib through the 1G reader, like read_blif reads through 1C's.
 */
const odin3_pass_def *const odin3_builtin_passes[] = {&READ_BLIF, &WRITE_BLIF, &CHECK,
                                                      &COMPACT,   &STATS,      &HIERARCHY};
const uint32_t odin3_builtin_pass_count =
    (uint32_t)(sizeof odin3_builtin_passes / sizeof odin3_builtin_passes[0]);
