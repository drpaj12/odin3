/*
 * prove.c — the proof of cone_check (Phase 7 spec 3.3, C-3, C-4). The temporary design (miter.c)
 * is lowered to gates with the `lower` pass, its AIG is built as the abc pass builds it, a miter
 * (the OR over the output bits of group XOR template) is added, and the linked ABC's `&cec -m`
 * decides it under the time limit. For DIFFERS the counterexample is found here, by evaluating the
 * miter (ABC reports only a summary): every input pattern when there are at most 16 inputs, else
 * 65536 pseudo-random ones.
 */
#include "ir/module.h"
#include "passes/abc/aig.h"
#include "passes/abc/engine.h"
#include "passes/abc/region.h"
#include "passes/abc/run.h"
#include "passes/cone_check/cone_check_int.h"
#include "passes/manager.h"
#include "util/log.h"
#include "util/str.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

enum {
    EXHAUSTIVE_BITS = 16,
    RANDOM_PATTERNS = 1 << 16,
    CMD_BYTES = 64,
    NAME_BYTES = 64,
    XS_LEFT_1 = 13, /* xorshift64 shifts */
    XS_RIGHT = 7,
    XS_LEFT_2 = 17,
    WORD_BITS = 64
};

static const uint64_t k_seed = 0x9E3779B97F4A7C15ULL;

void cc_proof_init(cc_proof *proof) {
    proof->verdict = CC_UNPROVEN;
    odin3_strbuf_init(&proof->cex);
}

void cc_proof_free(cc_proof *proof) {
    odin3_strbuf_free(&proof->cex);
}

/* --- the AIG of the temporary module ------------------------------------------------------- */

typedef struct cc_aig {
    odin3_abc_run *run;
    odin3_vec domains;
    odin3_abc_region region;
    odin3_abc_aigb aig;
} cc_aig;

static void aig_release(cc_aig *ag) {
    odin3_abc_aig_free(&ag->aig);
    odin3_abc_region_free(&ag->region);
    odin3_abc_domains_free(&ag->domains);
    odin3_abc_run_destroy(ag->run);
}

static odin3_status aig_make(cc_job *job, cc_aig *ag) {
    odin3_abc_opts none;
    memset(&none, 0, sizeof none);
    odin3_status st = odin3_abc_run_create(job->ctx, &none, &ag->run);
    if (st != ODIN3_OK) {
        return st;
    }
    ag->run->design = job->tmp;
    ag->run->module = job->mod;
    ag->run->module_id = odin3_design_top(job->tmp);
    st = odin3_abc_domains(ag->run, job->mod, &ag->domains);
    if (st != ODIN3_OK) {
        return st;
    }
    ag->run->domain = ag->domains.data;
    st = odin3_abc_region_build(ag->run, ag->run->domain, &ag->region);
    ag->run->region = &ag->region;
    return st == ODIN3_OK ? odin3_abc_aig_build(ag->run, &ag->region, &ag->aig) : st;
}

/* --- the miter ----------------------------------------------------------------------------- */

typedef struct cc_pair {
    int32_t lit0, lit1;
} cc_pair;

typedef struct cc_mit {
    odin3_vec objs; /* cc_pair */
    bool oom;
} cc_mit;

static int32_t mit_and(cc_mit *mit, int32_t left, int32_t right) {
    int32_t lo = left < right ? left : right;
    int32_t hi = left < right ? right : left;
    if (lo == 0 || lo == (hi ^ 1)) {
        return 0;
    }
    if (lo == 1 || lo == hi) {
        return lo == 1 ? hi : lo;
    }
    cc_pair *slot = odin3_vec_push(&mit->objs);
    if (slot == NULL) {
        mit->oom = true;
        return 0;
    }
    *slot = (cc_pair){lo, hi};
    return ODIN3_ABC_AIG_LIT(mit->objs.len - 1);
}

static int32_t mit_xor(cc_mit *mit, int32_t left, int32_t right) {
    int32_t only_left = mit_and(mit, left, right ^ 1);
    int32_t only_right = mit_and(mit, left ^ 1, right);
    return mit_and(mit, only_left ^ 1, only_right ^ 1) ^ 1;
}

/* The literal the PO for net carries: its object is the last POs' block's entry at its CO index. */
static odin3_status po_lit(const cc_aig *ag, odin3_net_id net, int32_t *lit) {
    const uint32_t *cos = ag->region.cos.data;
    const cc_pair *objs = ag->aig.objs.data;
    size_t base = ag->aig.objs.len / 2U - ag->aig.n_pos;
    for (size_t i = 0; i < ag->region.cos.len; i++) {
        if (cos[i] == net.v) {
            *lit = objs[base + i].lit0;
            return ODIN3_OK;
        }
    }
    odin3_log(ODIN3_LOG_ERROR, "cone_check: internal error: output net %u has no AIG output",
              (unsigned)net.v);
    return ODIN3_ERR_CHECK;
}

/* The AIG's objects without its POs. */
static odin3_status copy_base(const cc_aig *ag, cc_mit *mit) {
    size_t base = ag->aig.objs.len / 2U - ag->aig.n_pos;
    const cc_pair *objs = ag->aig.objs.data;
    for (size_t i = 0; i < base; i++) {
        cc_pair *slot = odin3_vec_push(&mit->objs);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = objs[i];
    }
    return ODIN3_OK;
}

/* The literal of the OR over every output bit of (template XOR group). */
static odin3_status add_differences(const cc_job *job, const cc_aig *ag, cc_mit *mit,
                                    int32_t *diff) {
    const odin3_node_id *t_ids = job->t_ids.data;
    const odin3_node_id *g_ids = job->g_ids.data;
    *diff = 0;
    for (size_t j = 0; j < job->t_ids.len; j++) {
        int32_t from_template = 0;
        int32_t from_group = 0;
        odin3_status st = po_lit(ag, cc_port_net(job->mod, t_ids[j]), &from_template);
        st = st == ODIN3_OK ? po_lit(ag, cc_port_net(job->mod, g_ids[j]), &from_group) : st;
        if (st != ODIN3_OK) {
            return st;
        }
        *diff = mit_and(mit, mit_xor(mit, from_template, from_group) ^ 1, *diff ^ 1) ^ 1;
    }
    return ODIN3_OK;
}

/* The AIG objects without its POs, then the miter (an OR over the output pairs) as the only PO. */
static odin3_status miter_build(const cc_job *job, const cc_aig *ag, cc_mit *mit) {
    int32_t diff = 0;
    odin3_status st = copy_base(ag, mit);
    st = st == ODIN3_OK ? add_differences(job, ag, mit, &diff) : st;
    cc_pair *po = st == ODIN3_OK ? odin3_vec_push(&mit->objs) : NULL;
    if (st == ODIN3_OK && (po == NULL || mit->oom)) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (po != NULL) {
        *po = (cc_pair){diff, ODIN3_ABC_AIG_NULL};
    }
    return st;
}

/* --- ABC ----------------------------------------------------------------------------------- */

typedef struct cc_say {
    int verdict; /* a cc_verdict, -1 until ABC says */
} cc_say;

/* "Networks are equivalent." / "... NOT equivalent." / "... NOT EQUIVALENT. ..." / "... UNDECIDED."
 */
static void on_line(void *user, const char *line) {
    static const char lead[] = "Networks are ";
    cc_say *say = user;
    const char *at = strstr(line, lead);
    if (at == NULL) {
        return;
    }
    at += sizeof lead - 1;
    if (strncasecmp(at, "NOT", sizeof "NOT" - 1) == 0) {
        say->verdict = CC_DIFFERS;
    } else if (strncasecmp(at, "equivalent", sizeof "equivalent" - 1) == 0) {
        say->verdict = CC_EQUIVALENT;
    } else if (strncasecmp(at, "UNDECIDED", sizeof "UNDECIDED" - 1) == 0) {
        say->verdict = CC_UNPROVEN;
    }
}

static odin3_status ask_abc(const odin3_abc_aig *view, uint32_t timeout, cc_verdict *verdict) {
    const odin3_abc_engine *eng = odin3_abc_engine_get();
    if (eng == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "cone_check: linked ABC is not available (no engine "
                                   "registered; build with ODIN3_WITH_ABC=ON)");
        return ODIN3_ERR_ENGINE;
    }
    cc_say say = {-1};
    odin3_abc_fail fail;
    memset(&fail, 0, sizeof fail);
    char cmd[CMD_BYTES];
    (void)snprintf(cmd, sizeof cmd, "&cec -m -T %u", (unsigned)timeout);
    odin3_status st = eng->open(eng->self, on_line, &say, &fail);
    if (st == ODIN3_OK) {
        st = eng->load_aig(eng->self, view, &fail);
        st = st == ODIN3_OK ? eng->exec(eng->self, cmd, &fail) : st;
        eng->close(eng->self);
    }
    if (st == ODIN3_ERR_ENGINE) {
        odin3_log(ODIN3_LOG_ERROR, "cone_check: ABC failed (kind %u, code %d): %s",
                  (unsigned)fail.kind, (int)fail.code, fail.text);
    } else if (st == ODIN3_OK && say.verdict < 0) {
        odin3_log(ODIN3_LOG_ERROR, "cone_check: ABC gave no verdict for the miter");
        st = ODIN3_ERR_ENGINE;
    }
    *verdict = st == ODIN3_OK ? (cc_verdict)say.verdict : CC_UNPROVEN;
    return st;
}

/* --- the counterexample -------------------------------------------------------------------- */

/* Evaluates the miter on the PI values val[1 .. n_pi]; the last object is the PO. */
static bool eval_miter(const cc_pair *objs, size_t count, uint8_t *val) {
    for (size_t i = 1; i < count; i++) {
        if (objs[i].lit0 == ODIN3_ABC_AIG_NULL) {
            continue;
        }
        uint8_t left = (uint8_t)(val[objs[i].lit0 >> 1] ^ (objs[i].lit0 & 1));
        if (objs[i].lit1 == ODIN3_ABC_AIG_NULL) {
            return left != 0;
        }
        val[i] = (uint8_t)(left & (val[objs[i].lit1 >> 1] ^ (objs[i].lit1 & 1)));
    }
    return false;
}

static uint64_t next_random(uint64_t *state) {
    *state ^= *state << XS_LEFT_1;
    *state ^= *state >> XS_RIGHT;
    *state ^= *state << XS_LEFT_2;
    return *state;
}

/* Fills val[1 .. n_pi] with pattern number `number`: counting when exhaustive, else random. */
static void set_pattern(uint8_t *val, uint32_t n_pi, uint64_t number, uint64_t *rng) {
    uint64_t bits = n_pi <= EXHAUSTIVE_BITS ? number : next_random(rng);
    for (uint32_t pi = 0; pi < n_pi; pi++) {
        val[pi + 1U] = (uint8_t)((bits >> (pi % WORD_BITS)) & 1U);
    }
}

/* The first pattern the miter answers 1 on, in val; false when none of those tried does. */
static bool find_cex(const cc_mit *mit, uint32_t n_pi, odin3_vec *val) {
    size_t count = mit->objs.len;
    uint64_t total = n_pi <= EXHAUSTIVE_BITS ? (uint64_t)1 << n_pi : RANDOM_PATTERNS;
    uint64_t rng = k_seed;
    for (uint64_t number = 0; number < total; number++) {
        set_pattern(val->data, n_pi, number, &rng);
        if (eval_miter(mit->objs.data, count, val->data)) {
            return true;
        }
    }
    return false;
}

/* `PORT[i]=v,...` over the template's input bits in port order; a bit no PI carries reads 0. */
static odin3_status cex_text(const cc_job *job, const cc_aig *ag, const uint8_t *val,
                             odin3_strbuf *out) {
    const uint32_t *cis = ag->region.cis.data;
    const odin3_node_id *ins = job->in_ids.data;
    const uint32_t *first = job->tmpl.first.data;
    const uint32_t *ord = job->tmpl.ord.data;
    odin3_status st = ODIN3_OK;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < first[job->tmpl.def->n_ports]; bit++) {
        if (cc_tmpl_dir(&job->tmpl, bit) != ODIN3_DIR_IN) {
            continue;
        }
        uint32_t net = cc_port_net(job->mod, ins[ord[bit]]).v;
        unsigned value = 0;
        for (size_t pi = 0; pi < ag->region.cis.len; pi++) {
            value = cis[pi] == net ? val[pi + 1U] : value;
        }
        char name[NAME_BYTES];
        st = odin3_strbuf_appendf(out, "%s%s=%u", out->len > 0 ? "," : "",
                                  cc_tmpl_name(&job->tmpl, bit, name, sizeof name), value);
    }
    return st;
}

static odin3_status make_cex(const cc_job *job, const cc_aig *ag, const cc_mit *mit,
                             cc_proof *proof) {
    uint32_t n_pi = ag->aig.n_pis;
    odin3_vec val;
    odin3_vec_init(&val, sizeof(uint8_t));
    odin3_status st = odin3_vec_reserve(&val, mit->objs.len + 1U);
    for (size_t i = 0; st == ODIN3_OK && i < mit->objs.len + 1U; i++) {
        uint8_t *slot = odin3_vec_push(&val);
        st = slot == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
        if (slot != NULL) {
            *slot = 0;
        }
    }
    if (st == ODIN3_OK && find_cex(mit, n_pi, &val)) {
        st = cex_text(job, ag, val.data, &proof->cex);
    }
    odin3_vec_free(&val);
    return st;
}

/* --- the proof ----------------------------------------------------------------------------- */

static odin3_status decide(cc_job *job, cc_proof *proof) {
    cc_aig ag;
    memset(&ag, 0, sizeof ag);
    cc_mit mit;
    memset(&mit, 0, sizeof mit);
    odin3_vec_init(&mit.objs, sizeof(cc_pair));
    odin3_vec_init(&ag.domains, sizeof(odin3_abc_domain));
    odin3_status st = aig_make(job, &ag);
    st = st == ODIN3_OK ? miter_build(job, &ag, &mit) : st;
    if (st == ODIN3_OK) {
        odin3_abc_aig view = {(const int32_t *)mit.objs.data, (uint32_t)mit.objs.len, 0};
        st = ask_abc(&view, job->opts->timeout, &proof->verdict);
    }
    if (st == ODIN3_OK && proof->verdict == CC_DIFFERS) {
        st = make_cex(job, &ag, &mit, proof);
    }
    odin3_vec_free(&mit.objs);
    aig_release(&ag);
    return st;
}

odin3_status cc_prove(cc_job *job, cc_proof *proof) {
    proof->verdict = CC_UNPROVEN;
    if (job->opts->timeout == 0) {
        return ODIN3_OK; /* no time to prove anything: unproven, ABC not asked */
    }
    odin3_status st = odin3_pass_run(job->tmp, "lower", odin3_bytes_cstr(""));
    if (st != ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "cone_check: the group or the template could not be lowered "
                                   "to gates for the proof");
        return st;
    }
    return decide(job, proof);
}
