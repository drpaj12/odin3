/*
 * proc_emit.c — the module context of the proc pass and its shared constant nets (2E spec §3,
 * §17; IR-4). Task 3 adds the cell helper, the net map, merges and orphans.
 */
#include "passes/proc/proc_internal.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/proc/pw.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The constant cell types and the constants they drive, indexed by bit & ~ODIN3_PW_CONST. */
static const char *const k_const_types[ODIN3_PROC_N_CONST_NETS] = {"$_CONST0_", "$_CONST1_",
                                                                   "$_CONSTX_", "$_CONSTZ_"};
static const odin3_const k_const_values[ODIN3_PROC_N_CONST_NETS] = {ODIN3_CONST_0, ODIN3_CONST_1,
                                                                    ODIN3_CONST_X, ODIN3_CONST_Z};

odin3_status odin3_proc_mctx_init(odin3_proc_mctx *mctx, odin3_pass_ctx *ctx,
                                  odin3_module *module) {
    if (mctx == NULL || ctx == NULL || module == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "proc: mctx_init: no context, pass run or module");
        return ODIN3_ERR_INVALID_ARG;
    }
    memset(mctx, 0, sizeof *mctx);
    mctx->ctx = ctx;
    mctx->design = odin3_module_design(module);
    mctx->module = module;
    odin3_vec_init(&mctx->pws, sizeof(odin3_pw));
    odin3_vec_init(&mctx->scratch, sizeof(uint32_t));
    odin3_vec_init(&mctx->touched, sizeof(uint32_t));
    return ODIN3_OK;
}

void odin3_proc_mctx_free(odin3_proc_mctx *mctx) {
    if (mctx == NULL) {
        return;
    }
    for (size_t i = 0; i < mctx->pws.len; i++) {
        odin3_pw_free(odin3_vec_at(&mctx->pws, i));
    }
    odin3_vec_free(&mctx->pws);
    odin3_vec_free(&mctx->scratch);
    odin3_vec_free(&mctx->touched);
}

/* The cell type named name (a built-in, so always registered). */
static odin3_status type_named(const odin3_design *design, const char *name,
                               odin3_celltype_id *out) {
    uint32_t str = 0;
    if (odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(name), &str) &&
        odin3_celltype_find(design, str, out)) {
        return ODIN3_OK;
    }
    odin3_log(ODIN3_LOG_ERROR, "proc: cell type '%s' is not registered", name);
    return ODIN3_ERR_INVALID_ARG;
}

/* The net on the Y pin of the first live node of type in ID order whose pin is connected. */
static odin3_net_id first_const_net(const odin3_module *module, odin3_celltype_id type) {
    uint32_t end = odin3_module_node_end(module);
    for (uint32_t i = 1; i < end; i++) {
        odin3_node_id node = {i};
        if (!odin3_node_live(module, node) || odin3_node_type(module, node).v != type.v) {
            continue;
        }
        odin3_net_id net = odin3_pin_net(module, odin3_node_port(module, node, 0).first);
        if (odin3_net_valid(net)) {
            return net;
        }
    }
    return (odin3_net_id){0};
}

/* A new node of constant type `type` driving a new unnamed net, both with the module's record. */
static odin3_status new_const_net(odin3_module *module, odin3_celltype_id type, odin3_net_id *out) {
    odin3_prov_id prov = odin3_module_prov(module);
    odin3_net_id net = {0};
    odin3_status st = odin3_net_create(module, 0, prov, &net);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_node_spec spec = {type, 0, prov, NULL, 0};
    odin3_netvec port = {&net, 1};
    odin3_node_id node = {0};
    st = odin3_node_create_connected(module, &spec, &port, &node);
    if (st != ODIN3_OK) {
        (void)odin3_net_delete(module, net); /* pinless and wireless: never fails */
        return st;
    }
    *out = net;
    return ODIN3_OK;
}

odin3_status odin3_proc_const_net(odin3_proc_mctx *mctx, odin3_pw_bit code, odin3_net_id *out) {
    if (code < ODIN3_PW_S0 || code > ODIN3_PW_SZ) {
        odin3_log(ODIN3_LOG_ERROR, "proc: const_net: 0x%08x is not 0, 1, x or z", (unsigned)code);
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t idx = code & ~ODIN3_PW_CONST;
    odin3_net_id cached = mctx->const_net[idx];
    if (odin3_net_valid(cached) && odin3_net_live(mctx->module, cached) &&
        odin3_net_const_value(mctx->module, cached) == k_const_values[idx]) {
        *out = cached;
        return ODIN3_OK;
    }
    odin3_celltype_id type = {0};
    odin3_status st = type_named(mctx->design, k_const_types[idx], &type);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_net_id net = first_const_net(mctx->module, type);
    if (!odin3_net_valid(net)) {
        st = new_const_net(mctx->module, type, &net);
    }
    if (st == ODIN3_OK) {
        mctx->const_net[idx] = net;
        *out = net;
    }
    return st;
}
