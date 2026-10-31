/*
 * sc_sigprob.c — signal probability of the scoap pass (spec section 5): the simulator runs random
 * vectors on the top module and counts, per net, the vectors in which it is 1.
 */
#include "passes/scoap/sc_opts.h"
#include "sim/prng.h"
#include "sim/signals.h"
#include "sim/sim.h"
#include "util/alloc.h"
#include "util/log.h"

enum { PPM = 1000000 };

/* Where a net is read from: a signal of the table and a bit of it. */
typedef struct sp_ref {
    uint32_t sig; /* signal ID, 0 none */
    uint32_t off; /* offset of the signal's bits in the read buffer */
    uint32_t bit;
} sp_ref;

typedef struct sp_ctx {
    const odin3_module *mod;
    odin3_sim_signals *sig;
    sp_ref *refs; /* per net */
    uint32_t *sig_off;
    uint32_t total_bits;
} sp_ctx;

static void ref_net(sp_ctx *ctx, odin3_net_id net, uint32_t sig, uint32_t off, uint32_t bit) {
    if (odin3_net_valid(net) && ctx->refs[net.v].sig == 0) {
        ctx->refs[net.v] = (sp_ref){sig, off, bit};
    }
}

/* Maps the nets of one top-scope signal. */
static void map_signal(sp_ctx *ctx, uint32_t id) {
    const odin3_sim_signal *sg = odin3_sim_signals_at(ctx->sig, id);
    if (sg == NULL || sg->scope != 0) {
        return;
    }
    ctx->sig_off[id] = ctx->total_bits;
    for (uint32_t bit = 0; bit < sg->width; bit++) {
        odin3_net_id net = {0};
        if (sg->obj_kind == ODIN3_OBJ_NET) {
            net.v = sg->obj_id;
        } else if (sg->obj_kind == ODIN3_OBJ_WIRE) {
            net = odin3_wire_net(ctx->mod, (odin3_wire_id){sg->obj_id}, sg->first_bit + bit);
        }
        ref_net(ctx, net, id, ctx->total_bits, bit);
    }
    ctx->total_bits += sg->width;
}

static void count_cycle(sp_ctx *ctx, uint32_t n_sig, uint8_t *bits, uint64_t *ones) {
    for (uint32_t id = 1; id <= n_sig; id++) {
        const odin3_sim_signal *sg = odin3_sim_signals_at(ctx->sig, id);
        if (sg != NULL && sg->scope == 0) {
            (void)odin3_sim_signals_read(ctx->sig, id, bits + ctx->sig_off[id], sg->width);
        }
    }
    uint32_t nets = odin3_module_net_end(ctx->mod);
    for (uint32_t net = 1; net < nets; net++) {
        if (ctx->refs[net].sig != 0) {
            ones[net] += bits[ctx->refs[net].off + ctx->refs[net].bit] != 0 ? 1U : 0U;
        }
    }
}

static odin3_status run_vectors(odin3_sim *sim, sp_ctx *ctx, const sc_opts *opts, int32_t *ppm) {
    uint32_t nets = odin3_module_net_end(ctx->mod);
    uint32_t n_sig = odin3_sim_signals_total(ctx->sig);
    uint8_t *bits = odin3_util_calloc((size_t)ctx->total_bits + 1);
    uint64_t *ones = odin3_util_calloc(((size_t)nets + 1) * sizeof *ones);
    odin3_prng prng;
    odin3_prng_seed(&prng, opts->seed);
    if (bits == NULL || ones == NULL) {
        odin3_util_free(bits);
        odin3_util_free(ones);
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint64_t vec = 0; vec < opts->vectors; vec++) {
        odin3_sim_drive_random(sim, &prng);
        odin3_sim_cycle(sim);
        count_cycle(ctx, n_sig, bits, ones);
    }
    for (uint32_t net = 1; net < nets; net++) {
        if (ctx->refs[net].sig != 0) {
            ppm[net] = (int32_t)((ones[net] * PPM + opts->vectors / 2) / opts->vectors);
        }
    }
    odin3_util_free(bits);
    odin3_util_free(ones);
    return ODIN3_OK;
}

odin3_status sc_sigprob(odin3_design *design, odin3_module_id top, const sc_opts *opts,
                        int32_t **ppm) {
    odin3_module *mod = odin3_module_get(design, top);
    uint32_t nets = odin3_module_net_end(mod);
    odin3_sim *sim = NULL;
    odin3_sim_signals *sig = NULL;
    *ppm = odin3_util_calloc(((size_t)nets + 1) * sizeof(int32_t));
    odin3_status st = *ppm != NULL ? odin3_sim_build(design, top, &sim) : ODIN3_ERR_NO_MEMORY;
    st = st == ODIN3_OK ? odin3_sim_signals_create(sim, &sig) : st;
    for (uint32_t net = 0; *ppm != NULL && net <= nets; net++) {
        (*ppm)[net] = -1;
    }
    if (st == ODIN3_OK) {
        uint32_t n_sig = odin3_sim_signals_total(sig);
        sp_ctx ctx = {mod, sig, NULL, NULL, 0};
        ctx.refs = odin3_util_calloc(((size_t)nets + 1) * sizeof *ctx.refs);
        ctx.sig_off = odin3_util_calloc(((size_t)n_sig + 1) * sizeof *ctx.sig_off);
        st = ctx.refs != NULL && ctx.sig_off != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        for (uint32_t id = 1; st == ODIN3_OK && id <= n_sig; id++) {
            map_signal(&ctx, id);
        }
        st = st == ODIN3_OK ? run_vectors(sim, &ctx, opts, *ppm) : st;
        odin3_util_free(ctx.refs);
        odin3_util_free(ctx.sig_off);
    }
    odin3_sim_signals_destroy(sig);
    odin3_sim_destroy(sim);
    if (st != ODIN3_OK) {
        odin3_util_free(*ppm);
        *ppm = NULL;
    }
    return st;
}
