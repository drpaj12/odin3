/*
 * builtin.c — the pass table and the BLIF, techlib, check, compact, stats, hierarchy and
 * write_verilog_ast passes (the project passes are in project_pass.c).
 */
#include "passes/builtin.h"
#include "ast/ast.h"
#include "ast/print.h"
#include "ast/srcman.h"
#include "ast/symtab.h"
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
#include "techlib/reader.h"
#include "util/alloc.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- argument helpers ----------------------------------------------------------------------- */

/* The single non-empty word of args, NUL-terminated in buf. INVALID_ARG (logged) otherwise. */
static odin3_status one_path(const char *pass, odin3_bytes args, odin3_strbuf *buf) {
    odin3_bytes word = odin3_pass_arg_next(&args);
    if (word.len == 0 || odin3_pass_arg_next(&args).ptr != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "%s: expects one path", pass);
        return ODIN3_ERR_INVALID_ARG;
    }
    return odin3_strbuf_append(buf, word);
}

static odin3_status no_args(const char *pass, odin3_bytes args) {
    if (odin3_pass_arg_next(&args).ptr != NULL) {
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

/* Sets the top to the module named name; who prefixes the log lines. INVALID_ARG (logged) when
 * there is none. */
static odin3_status select_named(odin3_design *design, odin3_bytes name, const char *who) {
    uint32_t str = 0;
    bool known = odin3_strtab_find(odin3_design_strtab(design), name, &str);
    for (uint32_t i = 1; known && i < odin3_design_module_end(design); i++) {
        if (odin3_module_name(odin3_module_get(design, (odin3_module_id){i})) == str) {
            odin3_log(ODIN3_LOG_INFO, "%s: top %s", who, str_of(design, str));
            return odin3_design_set_top(design, (odin3_module_id){i});
        }
    }
    odin3_log(ODIN3_LOG_ERROR, "%s: no module named '%.*s'", who, (int)name.len,
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

static odin3_status read_blif_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                  void *user) {
    (void)user;
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    odin3_status st = one_path("read_blif", args, &path);
    if (st == ODIN3_OK) {
        st = odin3_blif_read_in(ctx, path.data);
    }
    const char *top = odin3_pass_get_options().top;
    if (st == ODIN3_OK && top != NULL) {
        st = select_named(design, odin3_bytes_cstr(top), "read_blif: --top");
    }
    odin3_strbuf_free(&path);
    return st;
}

/* write_blif's arguments (spec 3E §4.3): one path and the options, in any order. */
typedef struct write_blif_args {
    odin3_strbuf path;
    odin3_blif_write_opts opts;
    uint8_t seen; /* the WB_* bits of the options given so far */
} write_blif_args;

enum {
    WB_VTR = 1U << 0,
    WB_LUT_K = 1U << 1,
    WB_NAME_STYLE = 1U << 2,
    WB_DECIMAL = 10,
    WB_MAX_DIGITS = 9, /* more digits than this are out of range anyway (no overflow) */
};

/* The value word of an option: "" when the option was the last word. */
static odin3_bytes wb_value(odin3_bytes *rest) {
    odin3_bytes value = odin3_pass_arg_next(rest);
    return value.ptr != NULL ? value : odin3_bytes_cstr("");
}

static odin3_status wb_lut_k(odin3_bytes value, write_blif_args *out) {
    const char *text = (const char *)value.ptr;
    bool ok = value.len > 0 && value.len <= WB_MAX_DIGITS;
    uint32_t k = 0;
    for (size_t i = 0; ok && i < value.len; i++) {
        ok = text[i] >= '0' && text[i] <= '9';
        k = ok ? k * WB_DECIMAL + (uint32_t)(text[i] - '0') : k;
    }
    if (!ok || k < ODIN3_BLIF_VTR_MIN_K || k > ODIN3_BLIF_VTR_MAX_K) {
        odin3_log(ODIN3_LOG_ERROR, "write_blif: --lut-k expects an integer in %u..%u, got '%.*s'",
                  ODIN3_BLIF_VTR_MIN_K, ODIN3_BLIF_VTR_MAX_K, (int)value.len, text);
        return ODIN3_ERR_INVALID_ARG;
    }
    out->opts.lut_k = k;
    return ODIN3_OK;
}

static odin3_status wb_name_style(odin3_bytes value, write_blif_args *out) {
    if (odin3_pass_arg_is(value, "short")) {
        out->opts.name_style = ODIN3_BLIF_NAMES_SHORT;
    } else if (odin3_pass_arg_is(value, "provenance")) {
        out->opts.name_style = ODIN3_BLIF_NAMES_PROVENANCE;
    } else {
        odin3_log(ODIN3_LOG_ERROR,
                  "write_blif: --name-style expects short or provenance, got '%.*s'",
                  (int)value.len, (const char *)value.ptr);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

/* Option word (its value read from rest): unknown, given twice, or parsed into out. */
static odin3_status wb_option(odin3_bytes word, odin3_bytes *rest, write_blif_args *out) {
    uint8_t bit = 0;
    if (odin3_pass_arg_is(word, "--vtr")) {
        bit = WB_VTR;
    } else if (odin3_pass_arg_is(word, "--lut-k")) {
        bit = WB_LUT_K;
    } else if (odin3_pass_arg_is(word, "--name-style")) {
        bit = WB_NAME_STYLE;
    } else {
        return unknown_arg("write_blif", word);
    }
    if ((out->seen & bit) != 0) {
        odin3_log(ODIN3_LOG_ERROR, "write_blif: option '%.*s' given twice", (int)word.len,
                  (const char *)word.ptr);
        return ODIN3_ERR_INVALID_ARG;
    }
    out->seen |= bit;
    if (bit == WB_VTR) {
        out->opts.dialect = ODIN3_BLIF_DIALECT_VTR;
        return ODIN3_OK;
    }
    odin3_bytes value = wb_value(rest);
    return bit == WB_LUT_K ? wb_lut_k(value, out) : wb_name_style(value, out);
}

/* True for an option word: `--` and at least one more character. */
static bool wb_is_option(odin3_bytes word) {
    return word.len > 2 && memcmp(word.ptr, "--", 2) == 0;
}

/*
 * Plan 3E point (l), the words left to right: an option word (`--x`) is an option, and unknown or
 * repeated options fail at once; a lone `--` ends the options; any other word is a path, so a path
 * starting with one `-` is written as before 3E. Then exactly one non-empty path, then --lut-k and
 * --name-style only with --vtr. Logged, ODIN3_ERR_INVALID_ARG.
 */
static odin3_status write_blif_parse(odin3_bytes args, write_blif_args *out) {
    uint32_t paths = 0;
    bool options = true;
    odin3_status st = ODIN3_OK;
    for (odin3_bytes word = odin3_pass_arg_next(&args); st == ODIN3_OK && word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        if (options && odin3_pass_arg_is(word, "--")) {
            options = false;
        } else if (options && wb_is_option(word)) {
            st = wb_option(word, &args, out);
        } else if (++paths == 1) {
            st = odin3_strbuf_append(&out->path, word);
        }
    }
    if (st != ODIN3_OK) {
        return st;
    }
    if (paths != 1 || out->path.len == 0) {
        odin3_log(ODIN3_LOG_ERROR, "write_blif: expects one path");
        return ODIN3_ERR_INVALID_ARG;
    }
    if ((out->seen & (WB_LUT_K | WB_NAME_STYLE)) != 0 && (out->seen & WB_VTR) == 0) {
        odin3_log(ODIN3_LOG_ERROR, "write_blif: --lut-k and --name-style need --vtr");
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

static odin3_status write_blif_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                   void *user) {
    (void)user;
    (void)ctx;
    write_blif_args parsed = {.opts = {ODIN3_BLIF_DIALECT_PLAIN, ODIN3_BLIF_NAMES_SHORT, 0},
                              .seen = 0};
    odin3_strbuf_init(&parsed.path);
    odin3_status st = write_blif_parse(args, &parsed);
    odin3_module_id top = odin3_design_top(design);
    if (st == ODIN3_OK && odin3_module_valid(top)) {
        odin3_log(ODIN3_LOG_DEBUG, "write_blif: %s: top %s %s", parsed.path.data,
                  module_name(design, top),
                  parsed.opts.dialect == ODIN3_BLIF_DIALECT_VTR ? "alone" : "first");
    }
    if (st == ODIN3_OK) {
        st = odin3_blif_write_with(design, parsed.path.data, &parsed.opts);
    }
    odin3_strbuf_free(&parsed.path);
    return st;
}

/* --- check, compact -------------------------------------------------------------------------- */

static odin3_status check_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                              void *user) {
    (void)user;
    (void)ctx;
    odin3_check_opts opts = {ODIN3_CHECK_FULL, ODIN3_VIEW_NONE};
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        if (!odin3_pass_arg_is(word, "--fast")) {
            return unknown_arg("check", word);
        }
        opts.level = ODIN3_CHECK_FAST;
    }
    return odin3_check_design(design, opts);
}

/* After module was compacted: every held store's symbol table follows its wire renumbering (EL-2:
 * a symbol's ir_wire is module-local). */
static void remap_symtabs(const odin3_design *design, odin3_module_id module,
                          const odin3_compact_map *map) {
    for (uint32_t i = 0; i < odin3_design_ast_count(design); i++) {
        odin3_symtab *st = odin3_ast_symtab(odin3_design_ast_at(design, i));
        if (st != NULL) {
            odin3_symtab_remap(st, module, map);
        }
    }
}

odin3_status odin3_design_compact(odin3_pass_ctx *ctx, odin3_design *design) {
    (void)ctx;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 1; st == ODIN3_OK && i < odin3_design_module_end(design); i++) {
        odin3_module_id id = {i};
        odin3_compact_map map = {0};
        st = odin3_module_compact(odin3_module_get(design, id), &map);
        if (st == ODIN3_OK) {
            remap_symtabs(design, id, &map);
        }
        odin3_compact_map_free(&map);
    }
    return st;
}

static odin3_status compact_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                void *user) {
    (void)user;
    odin3_status st = no_args("compact", args);
    return st == ODIN3_OK ? odin3_design_compact(ctx, design) : st;
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

static odin3_status stats_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                              void *user) {
    (void)user;
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
    odin3_bytes top; /* ptr NULL: not given */
    bool auto_top;
} hier_args;

static odin3_status parse_hier_args(odin3_bytes args, hier_args *out) {
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        if (odin3_pass_arg_is(word, "-auto")) {
            out->auto_top = true;
        } else if (!odin3_pass_arg_is(word, "--top")) {
            return unknown_arg("hierarchy", word);
        } else {
            out->top = odin3_pass_arg_next(&args);
            if (out->top.ptr == NULL) {
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
static odin3_status hierarchy_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                  void *user) {
    (void)user;
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
    if (parsed.top.ptr == NULL && option_top != NULL) {
        parsed.top = odin3_bytes_cstr(option_top);
    }
    if (parsed.top.ptr != NULL) {
        return select_named(design, parsed.top, "hierarchy");
    }
    odin3_module_id top = odin3_design_top(design);
    if (odin3_module_valid(top) && !parsed.auto_top) {
        odin3_log(ODIN3_LOG_INFO, "hierarchy: top %s", module_name(design, top));
        return ODIN3_OK;
    }
    return select_auto(design);
}

/* --- read_techlib ---------------------------------------------------------------------------- */

/* Reads a tech library (1G): each cell becomes a local cell type of the design, so a netlist read
 * afterwards instantiates it (a BLIF .subckt of the cell's name). */
static odin3_status read_techlib_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                     void *user) {
    (void)ctx;
    (void)user;
    odin3_strbuf path;
    odin3_strbuf_init(&path);
    odin3_status st = one_path("read_techlib", args, &path);
    if (st == ODIN3_OK) {
        st = odin3_techlib_read(design, path.data);
    }
    odin3_strbuf_free(&path);
    return st;
}

/* --- write_verilog_ast ---------------------------------------------------------------------- */

/* The latest read run with a held AST store (the highest run ID), 0 when none is held. */
static uint32_t latest_ast_run(const odin3_design *design) {
    uint32_t latest = 0;
    for (uint32_t i = 0; i < odin3_design_ast_count(design); i++) {
        uint32_t run = odin3_ast_run(odin3_design_ast_at(design, i)).v;
        latest = run > latest ? run : latest;
    }
    return latest;
}

/*
 * Appends every UNIT of ast as Verilog after a comment naming its run and form, each UNIT after a
 * comment naming it and where its file is (the resolved path, through the source manager).
 */
static odin3_status print_store(const odin3_srcman *sm, const odin3_ast *ast, odin3_strbuf *out) {
    const char *form = odin3_ast_form_of(ast) == ODIN3_AST_FORM_PARSED ? "parsed" : "elaborated";
    odin3_status st = odin3_strbuf_appendf(out, "// odin3 write_verilog_ast: run %u, %s AST\n",
                                           odin3_ast_run(ast).v, form);
    for (uint32_t i = 0; st == ODIN3_OK && i < odin3_ast_root_count(ast); i++) {
        odin3_ast_id unit = odin3_ast_root(ast, i);
        st = odin3_strbuf_appendf(out, "// unit %s at ", odin3_ast_name_str(ast, unit));
        if (st == ODIN3_OK) {
            st = odin3_srcman_format(sm, odin3_ast_loc(ast, unit), out, ODIN3_SRCFMT_RESOLVED);
        }
        if (st == ODIN3_OK) {
            st = odin3_strbuf_append(out, odin3_bytes_cstr("\n"));
        }
        if (st == ODIN3_OK) {
            st = odin3_ast_print(ast, unit, out);
        }
    }
    return st;
}

/* The held stores of the latest run (parsed first) as Verilog, in out. */
static odin3_status print_latest(odin3_design *design, odin3_strbuf *out) {
    odin3_passrun_id run = {latest_ast_run(design)};
    if (run.v == 0) {
        odin3_log(ODIN3_LOG_ERROR, "write_verilog_ast: the design holds no AST (a read keeps one "
                                   "with --keep-ast, or when a script names this pass)");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_srcman *sm = NULL;
    odin3_status st = odin3_design_get_srcman(design, &sm);
    static const odin3_ast_form k_forms[] = {ODIN3_AST_FORM_PARSED, ODIN3_AST_FORM_ELABORATED};
    for (size_t i = 0; st == ODIN3_OK && i < sizeof k_forms / sizeof k_forms[0]; i++) {
        odin3_ast *ast = NULL;
        st = odin3_design_find_ast(design, run, k_forms[i], &ast);
        if (st == ODIN3_OK && ast != NULL) {
            st = print_store(sm, ast, out);
        }
    }
    return st;
}

odin3_status odin3_pass_write_text(const char *path, const odin3_strbuf *text) {
    odin3_atomic_file file;
    odin3_status st = odin3_atomic_file_open(&file, path);
    if (st != ODIN3_OK) {
        return st;
    }
    if (text->len > 0 && fwrite(text->data, 1, text->len, file.fp) != text->len) {
        odin3_log(ODIN3_LOG_ERROR, "%s: write failed", path);
        st = ODIN3_ERR_IO;
    }
    return odin3_atomic_file_close(&file, st);
}

static odin3_status write_verilog_ast_run(odin3_pass_ctx *ctx, odin3_design *design,
                                          odin3_bytes args, void *user) {
    (void)ctx;
    (void)user;
    odin3_strbuf path;
    odin3_strbuf text;
    odin3_strbuf_init(&path);
    odin3_strbuf_init(&text);
    odin3_status st = one_path("write_verilog_ast", args, &path);
    if (st == ODIN3_OK) {
        st = print_latest(design, &text);
    }
    if (st == ODIN3_OK) {
        st = odin3_pass_write_text(path.data, &text);
    }
    odin3_strbuf_free(&text);
    odin3_strbuf_free(&path);
    return st;
}

/* --- the table ------------------------------------------------------------------------------- */

static const odin3_pass_def READ_BLIF = {
    "read_blif",   "read_blif <file>: read a BLIF netlist into an empty design (first model = top)",
    read_blif_run, NULL,
    false,         ODIN3_PASS_PROCESSES_OK};
static const odin3_pass_def READ_TECHLIB = {
    "read_techlib",
    "read_techlib <file>: read a tech library (.o3lib); its cells become cell types of the design",
    read_techlib_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_OK};
static const odin3_pass_def WRITE_BLIF = {
    "write_blif",
    "write_blif [--vtr] [--lut-k K] [--name-style short|provenance] <file>: write the design as "
    "BLIF (the top first); --vtr writes the top alone in the dialect VPR reads",
    write_blif_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
static const odin3_pass_def CHECK = {
    "check",   "check [--fast]: check the IR invariants (IR.md section 9)",
    check_run, NULL,
    false,     ODIN3_PASS_PROCESSES_OK};
static const odin3_pass_def COMPACT = {
    "compact",
    "compact: renumber each module densely, freeing dead objects (IR-6; atomic per module only)",
    compact_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_OK};
static const odin3_pass_def STATS = {
    "stats",   "stats: log modules, top, and per module ports/nodes/nets/wires and cell types",
    stats_run, NULL,
    false,     ODIN3_PASS_PROCESSES_OK};
static const odin3_pass_def HIERARCHY = {
    "hierarchy",
    "hierarchy [--top <name>] [-auto]: set the top module (named, or the one no module "
    "instantiates)",
    hierarchy_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_OK};
static const odin3_pass_def WRITE_VERILOG_AST = {
    "write_verilog_ast",
    "write_verilog_ast <file>: print the ASTs the latest read kept (parsed, then elaborated) as "
    "Verilog",
    write_verilog_ast_run,
    NULL,
    true,
    ODIN3_PASS_PROCESSES_OK};

/* Registry order. */
const odin3_pass_def *const odin3_builtin_passes[] = {&READ_BLIF,
                                                      &READ_TECHLIB,
                                                      &WRITE_BLIF,
                                                      &odin3_pass_write_json,
                                                      &odin3_pass_write_verilog,
                                                      &odin3_pass_write_dot,
                                                      &CHECK,
                                                      &COMPACT,
                                                      &STATS,
                                                      &odin3_pass_sim,
                                                      &HIERARCHY,
                                                      &WRITE_VERILOG_AST,
                                                      &odin3_pass_read_project,
                                                      &odin3_pass_dump_project,
                                                      &odin3_pass_preprocess,
                                                      &odin3_pass_write_rtlil,
                                                      &odin3_pass_proc,
                                                      &odin3_pass_opt,
                                                      &odin3_pass_opt_expr,
                                                      &odin3_pass_opt_merge,
                                                      &odin3_pass_opt_muxtree,
                                                      &odin3_pass_opt_reduce,
                                                      &odin3_pass_opt_share,
                                                      &odin3_pass_opt_dff,
                                                      &odin3_pass_opt_clean,
                                                      &odin3_pass_abc,
                                                      &odin3_pass_wreduce,
                                                      &odin3_pass_flatten,
                                                      &odin3_pass_lower,
                                                      &odin3_pass_partial_map,
                                                      &odin3_pass_hardmap,
                                                      &odin3_pass_lpmmap,
                                                      &odin3_read_verilog_pass,
                                                      &odin3_pass_read_arch,
                                                      &odin3_pass_write_techlib};
const uint32_t odin3_builtin_pass_count =
    (uint32_t)(sizeof odin3_builtin_passes / sizeof odin3_builtin_passes[0]);
