/*
 * register.c — the `submod` pass definition (Phase 7 spec 3.1): option parsing, the groups of
 * every module (sorted by value, so the run is deterministic) and one extraction per group.
 */
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/manager.h"
#include "passes/submod/submod_int.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { SM_INT_TEXT = 32 };

static const char k_default_key[] = "submod";

/* --- options ------------------------------------------------------------------------------- */

typedef struct sm_opts {
    odin3_strbuf key;
    odin3_strbuf name;
    odin3_strbuf bus;
    bool has_name;
    bool has_bus;
} sm_opts;

static odin3_status take_value(odin3_bytes *args, const char *option, odin3_strbuf *into) {
    odin3_bytes value = odin3_pass_arg_next(args);
    if (value.ptr == NULL || value.len == 0) {
        odin3_log(ODIN3_LOG_ERROR,
                  "submod: %s needs a value (usage: submod [-attr KEY] [-name N] [-bus BUSKEY])",
                  option);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_strbuf_clear(into);
    return odin3_strbuf_append(into, value);
}

static odin3_status parse_args(odin3_bytes args, sm_opts *opts) {
    odin3_status st = odin3_strbuf_append(&opts->key, odin3_bytes_cstr(k_default_key));
    for (odin3_bytes word = odin3_pass_arg_next(&args); st == ODIN3_OK && word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        if (odin3_pass_arg_is(word, "-attr")) {
            st = take_value(&args, "-attr", &opts->key);
        } else if (odin3_pass_arg_is(word, "-name")) {
            st = take_value(&args, "-name", &opts->name);
            opts->has_name = st == ODIN3_OK;
        } else if (odin3_pass_arg_is(word, "-bus")) {
            st = take_value(&args, "-bus", &opts->bus);
            opts->has_bus = st == ODIN3_OK;
        } else {
            odin3_log(ODIN3_LOG_ERROR,
                      "submod: unknown option '%.*s' (options: -attr KEY, -name N, -bus BUSKEY)",
                      (int)word.len, (const char *)word.ptr);
            st = ODIN3_ERR_INVALID_ARG;
        }
    }
    return st;
}

/* --- the groups of one module -------------------------------------------------------------- */

/* The distinct group values of a module: NUL-terminated texts in pool, offsets in `order`, kept
 * sorted by text (insertion by binary search). */
typedef struct sm_groups {
    odin3_strbuf pool;
    odin3_vec order; /* size_t offsets into pool */
} sm_groups;

static int compare_at(const sm_groups *groups, size_t at, const char *text) {
    return strcmp(groups->pool.data + at, text);
}

/* Adds text to the set (once); a duplicate is a no-op. */
static odin3_status add_group(sm_groups *groups, const char *text) {
    size_t lo = 0;
    size_t hi = groups->order.len;
    const size_t *offs = groups->order.data;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = compare_at(groups, offs[mid], text);
        if (cmp == 0) {
            return ODIN3_OK;
        }
        lo = cmp < 0 ? mid + 1 : lo;
        hi = cmp < 0 ? hi : mid;
    }
    size_t at = groups->pool.len;
    if (odin3_strbuf_append(&groups->pool, odin3_bytes_cstr(text)) != ODIN3_OK ||
        odin3_strbuf_append(&groups->pool, (odin3_bytes){"", 1}) != ODIN3_OK ||
        odin3_vec_push(&groups->order) == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    size_t *list = groups->order.data;
    memmove(list + lo + 1, list + lo, (groups->order.len - 1 - lo) * sizeof *list);
    list[lo] = at;
    return ODIN3_OK;
}

static odin3_status find_groups(const odin3_module *module, const char *key, sm_groups *groups) {
    const odin3_design *design = odin3_module_design(module);
    uint32_t key_str = 0;
    if (!odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(key), &key_str)) {
        return ODIN3_OK;
    }
    for (uint32_t nid = 1; nid < odin3_module_node_end(module); nid++) {
        const odin3_value *val =
            odin3_node_live(module, (odin3_node_id){nid})
                ? odin3_attr_get(module, (odin3_objref){ODIN3_OBJ_NODE, nid}, key_str)
                : NULL;
        char buf[SM_INT_TEXT];
        const char *text = NULL;
        if (val != NULL && val->kind == ODIN3_VAL_STRING) {
            text = odin3_strtab_get(odin3_design_strtab(design), val->str);
        } else if (val != NULL && val->kind == ODIN3_VAL_INT) {
            (void)snprintf(buf, sizeof buf, "%" PRId64, val->i);
            text = buf;
        }
        if (text == NULL) {
            continue;
        }
        odin3_status st = text[0] == '\0' ? ODIN3_OK : add_group(groups, text);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* --- the run ------------------------------------------------------------------------------- */

/* The new module names taken so far in the check phase: NUL-terminated texts. */
static bool name_taken(const odin3_strbuf *claimed, const char *name) {
    for (size_t at = 0; at < claimed->len; at += strlen(claimed->data + at) + 1) {
        if (strcmp(claimed->data + at, name) == 0) {
            return true;
        }
    }
    return false;
}

/* Checks a group and claims its module name; two groups of one run with one name are refused. */
static odin3_status check_group(odin3_pass_ctx *ctx, odin3_module_id mid,
                                const odin3_submod_group *group, odin3_strbuf *claimed) {
    odin3_status st = odin3_submod_check_named(ctx, mid, group);
    if (st == ODIN3_OK && name_taken(claimed, group->name)) {
        odin3_log(ODIN3_LOG_ERROR,
                  "submod: module name '%s' is already in use (by another group of this run)",
                  group->name);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (st == ODIN3_OK) {
        st = odin3_strbuf_append(claimed, odin3_bytes_cstr(group->name));
        st = st == ODIN3_OK ? odin3_strbuf_append(claimed, (odin3_bytes){"", 1}) : st;
    }
    return st;
}

/* The groups of module mid: with claimed set only checked (nothing changes), else moved. */
static odin3_status extract_groups(odin3_pass_ctx *ctx, odin3_module_id mid, const sm_opts *opts,
                                   uint32_t *moved, odin3_strbuf *claimed) {
    const odin3_module *module = odin3_module_get(ctx->design, mid);
    sm_groups groups;
    odin3_strbuf_init(&groups.pool);
    odin3_vec_init(&groups.order, sizeof(size_t));
    odin3_status st = find_groups(module, opts->key.data, &groups);
    const size_t *offs = groups.order.data;
    for (size_t i = 0; st == ODIN3_OK && i < groups.order.len; i++) {
        const char *value = groups.pool.data + offs[i];
        odin3_submod_group group = {opts->key.data, value, opts->has_name ? opts->name.data : value,
                                    opts->has_bus ? opts->bus.data : NULL};
        if (claimed != NULL) {
            st = check_group(ctx, mid, &group, claimed);
        } else {
            st = odin3_submod_extract_named(ctx, mid, &group, NULL);
            *moved += st == ODIN3_OK ? 1U : 0U;
        }
    }
    odin3_vec_free(&groups.order);
    odin3_strbuf_free(&groups.pool);
    return st;
}

/* The number of groups over the first `end` modules, for the single-group rule of -name. */
static odin3_status count_groups(odin3_design *design, uint32_t end, const char *key,
                                 size_t *total) {
    *total = 0;
    for (uint32_t mid = 1; mid < end; mid++) {
        sm_groups groups;
        odin3_strbuf_init(&groups.pool);
        odin3_vec_init(&groups.order, sizeof(size_t));
        odin3_status st =
            find_groups(odin3_module_get(design, (odin3_module_id){mid}), key, &groups);
        *total += groups.order.len;
        odin3_vec_free(&groups.order);
        odin3_strbuf_free(&groups.pool);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static odin3_status check_single(odin3_design *design, uint32_t end, const sm_opts *opts) {
    size_t total = 0;
    odin3_status st = count_groups(design, end, opts->key.data, &total);
    if (st == ODIN3_OK && total == 0) {
        odin3_log(ODIN3_LOG_ERROR, "submod: group '%s' is empty: no live node has attribute '%s'",
                  opts->name.data, opts->key.data);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (st == ODIN3_OK && total > 1) {
        odin3_log(ODIN3_LOG_ERROR,
                  "submod: -name takes a single group but attribute '%s' has %zu values",
                  opts->key.data, total);
        return ODIN3_ERR_INVALID_ARG;
    }
    return st;
}

static odin3_status submod_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                    void *user) {
    (void)user;
    sm_opts opts;
    odin3_strbuf_init(&opts.key);
    odin3_strbuf_init(&opts.name);
    odin3_strbuf_init(&opts.bus);
    opts.has_name = false;
    opts.has_bus = false;
    uint32_t end = odin3_design_module_end(design);
    uint32_t moved = 0;
    odin3_status st = parse_args(args, &opts);
    st = st == ODIN3_OK && opts.has_name ? check_single(design, end, &opts) : st;
    /* Every group is checked before any moves, so a refusal leaves the IR unchanged. */
    odin3_strbuf claimed;
    odin3_strbuf_init(&claimed);
    for (uint32_t mid = 1; st == ODIN3_OK && mid < end; mid++) {
        st = extract_groups(ctx, (odin3_module_id){mid}, &opts, &moved, &claimed);
    }
    odin3_strbuf_free(&claimed);
    for (uint32_t mid = 1; st == ODIN3_OK && mid < end; mid++) {
        st = extract_groups(ctx, (odin3_module_id){mid}, &opts, &moved, NULL);
    }
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO, "submod: %" PRIu32 " groups moved into new modules", moved);
    }
    odin3_strbuf_free(&opts.key);
    odin3_strbuf_free(&opts.name);
    odin3_strbuf_free(&opts.bus);
    return st;
}

const odin3_pass_def odin3_pass_submod = {
    "submod",        "submod [-attr KEY] [-name N] [-bus BUSKEY]",
    submod_pass_run, NULL,
    false,           ODIN3_PASS_PROCESSES_REFUSE};
