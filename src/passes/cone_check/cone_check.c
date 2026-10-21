/*
 * cone_check.c — one module's run of `cone_check` (Phase 7 spec 3.3): the group is checked and its
 * binding validated, then copied with the template cell into a temporary design (miter.c, which
 * the real design never sees), proved there (prove.c) and the verdict written as module
 * attributes (C-4). The module's nodes, nets and wires are not touched.
 */
#include "ir/module.h"
#include "passes/cone_check/cone_check_int.h"
#include "util/log.h"
#include "util/str.h"

#include <stdio.h>
#include <string.h>

static const char *const k_verdicts[] = {"equivalent", "differs", "unproven"};

static void job_init(cc_job *job, odin3_pass_ctx *ctx, odin3_module *module, const cc_opts *opts) {
    memset(job, 0, sizeof *job);
    job->ctx = ctx;
    job->src = module;
    job->opts = opts;
    cc_group_init(&job->group, module);
    cc_tmpl_init(&job->tmpl);
    odin3_vec_init(&job->map, sizeof(uint32_t));
    odin3_vec_init(&job->in_ids, sizeof(odin3_node_id));
    odin3_vec_init(&job->t_ids, sizeof(odin3_node_id));
    odin3_vec_init(&job->g_ids, sizeof(odin3_node_id));
}

static void job_free(cc_job *job) {
    cc_group_free(&job->group);
    cc_tmpl_free(&job->tmpl);
    odin3_vec_free(&job->map);
    odin3_vec_free(&job->in_ids);
    odin3_vec_free(&job->t_ids);
    odin3_vec_free(&job->g_ids);
    if (job->tmp != NULL) {
        odin3_design_destroy(job->tmp);
    }
}

/* --- the result ---------------------------------------------------------------------------- */

static odin3_objref module_ref(const odin3_module *module) {
    return (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(module).v};
}

static odin3_status set_text(odin3_module *module, const char *key, const char *text) {
    odin3_design *design = odin3_module_design(module);
    uint32_t key_str = 0;
    odin3_value value = {ODIN3_VAL_STRING, 0, NULL, 0, 0, 0};
    odin3_status st = odin3_design_intern(design, odin3_bytes_cstr(key), &key_str);
    st = st == ODIN3_OK ? odin3_design_intern(design, odin3_bytes_cstr(text), &value.str) : st;
    return st == ODIN3_OK ? odin3_attr_set(module, module_ref(module), key_str, &value) : st;
}

static odin3_status clear_attr(odin3_module *module, const char *key) {
    uint32_t key_str = 0;
    if (!odin3_strtab_find(odin3_design_strtab(odin3_module_design(module)), odin3_bytes_cstr(key),
                           &key_str)) {
        return ODIN3_OK;
    }
    return odin3_attr_remove(module, module_ref(module), key_str);
}

/* Sets the attributes of the result one after another; the first failure stops. */
static odin3_status set_all(odin3_module *module, const char *const keys[],
                            const char *const texts[], size_t count) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < count; i++) {
        st = set_text(module, keys[i], texts[i]);
    }
    return st;
}

/* All or nothing: every text is computed first; when a set fails, the keys of this run go again. */
static odin3_status write_result(const cc_job *job, const cc_proof *proof) {
    odin3_module *module = job->src;
    const cc_opts *opts = job->opts;
    char hash[ODIN3_CONE_HASH_BYTES];
    char group[ODIN3_LOG_BUF / 4];
    (void)snprintf(group, sizeof group, "%s=%s", opts->key, opts->value);
    odin3_status st = odin3_cone_hash(module, opts->key, opts->value, hash);
    if (st != ODIN3_OK) {
        return st;
    }
    const char *keys[] = {CC_KEY_CHECK, CC_KEY_GROUP, CC_KEY_HASH, CC_KEY_CEX};
    const char *texts[] = {k_verdicts[proof->verdict], group, hash, proof->cex.data};
    bool has_cex = proof->cex.len > 0;
    size_t count = has_cex ? 4U : 3U;
    st = set_all(module, keys, texts, count);
    if (st == ODIN3_OK && !has_cex) {
        st = clear_attr(module, CC_KEY_CEX);
    }
    if (st != ODIN3_OK) {
        for (size_t i = 0; i < 4U; i++) {
            (void)clear_attr(module, keys[i]);
        }
    }
    return st;
}

/* The group has a constant x or z driver: the proof reads those as 0, so it proves nothing. */
static bool group_has_xz(const cc_group *group) {
    const uint32_t *nodes = group->nodes.data;
    for (size_t i = 0; i < group->nodes.len; i++) {
        odin3_node_id node = {nodes[i]};
        const odin3_celltype_def *def =
            odin3_celltype_get(group->design, odin3_node_type(group->module, node));
        odin3_const val = def->const_value != NULL
                              ? def->const_value(odin3_node_param(group->module, node, 0))
                              : ODIN3_CONST_NONE;
        if (val == ODIN3_CONST_X || val == ODIN3_CONST_Z) {
            return true;
        }
    }
    return false;
}

/* --- the run ------------------------------------------------------------------------------- */

static odin3_status prepare(cc_job *job) {
    const cc_opts *opts = job->opts;
    odin3_status st = cc_group_check(&job->group, opts->key, opts->value);
    job->tmp = st == ODIN3_OK ? odin3_design_create() : NULL;
    st = st == ODIN3_OK && job->tmp == NULL ? ODIN3_ERR_NO_MEMORY : st;
    st = st == ODIN3_OK ? cc_tmpl_build(&job->tmpl, job->tmp, opts) : st;
    st = st == ODIN3_OK ? cc_tmpl_bind(&job->tmpl, &job->group) : st;
    return st == ODIN3_OK ? cc_miter_build(job) : st;
}

odin3_status cc_run_module(odin3_pass_ctx *ctx, odin3_module *module, const cc_opts *opts,
                           bool *found) {
    cc_job job;
    cc_proof proof;
    job_init(&job, ctx, module, opts);
    cc_proof_init(&proof);
    odin3_status st = cc_group_collect(&job.group, opts);
    *found = st == ODIN3_OK && job.group.nodes.len > 0;
    st = *found && st == ODIN3_OK ? prepare(&job) : st;
    if (*found && st == ODIN3_OK && group_has_xz(&job.group)) {
        odin3_log(ODIN3_LOG_WARN,
                  "cone_check: group '%s=%s': x/z constant in the cone: proof is modulo x=0",
                  opts->key, opts->value);
        proof.verdict = CC_UNPROVEN;
    } else {
        st = *found && st == ODIN3_OK ? cc_prove(&job, &proof) : st;
    }
    st = *found && st == ODIN3_OK ? write_result(&job, &proof) : st;
    if (*found && st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO, "cone_check: module '%s': group '%s=%s' vs %s: %s",
                  odin3_strtab_get(odin3_design_strtab(odin3_module_design(module)),
                                   odin3_module_name(module)),
                  opts->key, opts->value, opts->cell, k_verdicts[proof.verdict]);
    }
    cc_proof_free(&proof);
    job_free(&job);
    return st;
}
