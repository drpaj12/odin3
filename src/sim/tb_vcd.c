/* tb_vcd.c — the `sim_tb --vcd` writer: an IEEE 1364 VCD (0, 1, x, z) of the testbench's scalar
 * regs and wires and of every DUT instance port that is connected to an expression (with
 * --vcd-all also every net of every DUT instance, in a nested scope tree). A change is
 * written once per time step, with the value the signal has when the step ends (a glitch inside a
 * step is not shown). Arrays and parameters are not dumped. */
#include "sim/tb_internal.h"
#include "sim/vcd.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>

typedef struct vcd_sig {
    char code[ODIN3_SIM_VCD_CODE_MAX];
    const tb_var *var;             /* a testbench variable, or */
    const tb_conn *conn;           /* a DUT port connection, or */
    const odin3_sim_signals *nets; /* a net of a DUT (--vcd-all): the table and */
    uint32_t net;                  /* the signal's ID in it */
    uint32_t width;
    tbv last;
} vcd_sig;

/* The net table of one DUT instance (--vcd-all). */
typedef struct nets_ref {
    odin3_sim_signals *tab;
} nets_ref;

struct tb_vcd {
    FILE *fp;
    vcd_sig *sigs;
    uint32_t nsigs;
    bool first;
    uint64_t last_time; /* the last #<time> written */
    nets_ref *nets;     /* --vcd-all: the net table of every instance, in order */
    uint32_t nnets;
};

static bool dumpable(const tb_var *var) {
    return !var->is_param && var->depth == 0 && var->width >= 1 && var->width <= TB_MAX_WIDTH;
}

static void write_value(tb_vcd *vcd, const vcd_sig *sig, const tbv *val) {
    static const char CODES[] = "01xz";
    if (sig->width == 1) {
        (void)fprintf(vcd->fp, "%c%s\n", CODES[tbv_code(val, 0)], sig->code);
        return;
    }
    (void)fputc('b', vcd->fp);
    for (uint32_t bit = sig->width; bit > 0; bit--) {
        (void)fputc(CODES[tbv_code(val, bit - 1U)], vcd->fp);
    }
    (void)fprintf(vcd->fp, " %s\n", sig->code);
}

static void declare(tb_vcd *vcd, vcd_sig *sig, const char *kind, const char *name) {
    odin3_sim_vcd_code(vcd->nsigs, sig->code);
    (void)fprintf(vcd->fp, "$var %s %u %s %s $end\n", kind, sig->width, sig->code, name);
    vcd->nsigs++;
}

static uint32_t count_signals(const tb_module *tb, const tb_vcd *vcd) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < vcd->nnets; i++) {
        count += odin3_sim_signals_listed(vcd->nets[i].tab);
    }
    for (uint32_t i = 0; i < tb->nvars; i++) {
        count += dumpable(&tb->vars[i]) ? 1U : 0U;
    }
    for (uint32_t i = 0; i < tb->ninsts; i++) {
        count += tb->insts[i].nconns;
    }
    return count;
}

/* Declares every net of a DUT's table (the scope tree of its instances), after the port variables
 * of the instance. */
static void declare_nets(tb_vcd *vcd, const odin3_sim_signals *nets) {
    (void)odin3_sim_vcd_declare(vcd->fp, nets, vcd->nsigs);
    for (uint32_t id = 1; id <= odin3_sim_signals_listed(nets); id++) {
        vcd_sig *sig = &vcd->sigs[vcd->nsigs];
        sig->nets = nets;
        sig->net = id;
        sig->width = odin3_sim_signals_at(nets, id)->width;
        odin3_sim_vcd_code(vcd->nsigs, sig->code);
        vcd->nsigs++;
    }
}

/* The scope of DUT instance number idx: its connected ports, then (--vcd-all) its nets. */
static void write_inst(tb_vcd *vcd, const tb_inst *inst, uint32_t idx) {
    (void)fprintf(vcd->fp, "$scope module %s $end\n",
                  inst->name[0] != '\0' ? inst->name : inst->module);
    for (uint32_t k = 0; k < inst->nconns; k++) {
        if (inst->conns[k].expr == NULL) {
            continue; /* an unconnected port */
        }
        vcd_sig *sig = &vcd->sigs[vcd->nsigs];
        sig->conn = &inst->conns[k];
        sig->width = inst->conns[k].width;
        declare(vcd, sig, "wire", inst->conns[k].port);
    }
    if (vcd->nnets > 0) {
        declare_nets(vcd, vcd->nets[idx].tab);
    }
    (void)fputs("$upscope $end\n", vcd->fp);
}

static void write_header(tb_vcd *vcd, const tb_module *tb) {
    (void)fprintf(vcd->fp,
                  "$version odin3 sim_tb $end\n$timescale %s $end\n$scope module %s $end\n",
                  tb->timescale, tb->name);
    for (uint32_t i = 0; i < tb->nvars; i++) {
        const tb_var *var = &tb->vars[i];
        if (dumpable(var)) {
            vcd_sig *sig = &vcd->sigs[vcd->nsigs];
            sig->var = var;
            sig->width = var->width;
            declare(vcd, sig, var->is_wire ? "wire" : "reg", var->name);
        }
    }
    for (uint32_t i = 0; i < tb->ninsts; i++) {
        write_inst(vcd, &tb->insts[i], i);
    }
    (void)fputs("$upscope $end\n$enddefinitions $end\n", vcd->fp);
}

/* --vcd-all: builds the net table of every DUT instance (their simulators exist: setup ran). */
static bool open_nets(tb_vcd *vcd, const tb_module *tb, const tb_options *opts) {
    if (!opts->vcd_all || tb->ninsts == 0) {
        return true;
    }
    vcd->nets = calloc((size_t)tb->ninsts, sizeof *vcd->nets);
    if (vcd->nets == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < tb->ninsts; i++) {
        odin3_status st = tb->insts[i].sim != NULL
                              ? odin3_sim_signals_create_nets(tb->insts[i].sim, opts->namer,
                                                              opts->namer_ctx, &vcd->nets[i].tab)
                              : ODIN3_ERR_INVALID_ARG;
        if (st != ODIN3_OK) {
            return false;
        }
        vcd->nnets++;
    }
    return true;
}

static void free_nets(tb_vcd *vcd) {
    for (uint32_t i = 0; i < vcd->nnets; i++) {
        odin3_sim_signals_destroy(vcd->nets[i].tab);
    }
    free(vcd->nets);
    vcd->nets = NULL;
    vcd->nnets = 0;
}

tb_vcd *tb_vcd_open(const char *path, const tb_module *tb, const tb_options *opts) {
    tb_vcd *vcd = calloc(1, sizeof *vcd);
    if (vcd != NULL && !open_nets(vcd, tb, opts)) {
        free_nets(vcd);
        free(vcd);
        odin3_log(ODIN3_LOG_ERROR, "sim_tb: cannot list the DUT nets for --vcd-all");
        return NULL;
    }
    uint32_t total = vcd != NULL ? count_signals(tb, vcd) : 0U;
    if (vcd != NULL) {
        vcd->sigs = calloc((size_t)total + 1U, sizeof *vcd->sigs);
        vcd->fp = vcd->sigs != NULL ? fopen(path, "w") : NULL;
    }
    if (vcd == NULL || vcd->fp == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "sim_tb: cannot write the --vcd file `%s`", path);
        if (vcd != NULL) {
            free(vcd->sigs);
            free_nets(vcd);
        }
        free(vcd);
        return NULL;
    }
    vcd->first = true;
    write_header(vcd, tb);
    return vcd;
}

static void current(tb_ev *ev, const vcd_sig *sig, tbv *val) {
    if (sig->nets != NULL) {
        tb_sig_peek(sig->nets, sig->net, val);
        return;
    }
    if (sig->var != NULL) {
        tb_var_load(val, sig->var, 0);
        return;
    }
    /* Sampling must not change the run: a `$random` in the connection would advance the seed. */
    int32_t seed = ev->seed;
    tb_eval(ev, sig->conn->expr, sig->width, false, val);
    ev->seed = seed;
    tb_resize rs = {sig->width > sig->conn->expr->width ? sig->width : sig->conn->expr->width,
                    sig->width, false, false};
    tbv_extend(val, &rs);
}

void tb_vcd_sample(tb_vcd *vcd, tb_ev *ev, uint64_t time) {
    bool header = false;
    for (uint32_t i = 0; i < vcd->nsigs; i++) {
        vcd_sig *sig = &vcd->sigs[i];
        tbv val;
        current(ev, sig, &val);
        if (!vcd->first && tbv_same(&val, &sig->last, sig->width)) {
            continue;
        }
        if (!header) {
            (void)fprintf(vcd->fp, "#%llu\n%s", (unsigned long long)time,
                          vcd->first ? "$dumpvars\n" : "");
            vcd->last_time = time;
            header = true;
        }
        write_value(vcd, sig, &val);
        tbv_copy(&sig->last, &val, sig->width);
    }
    if (header && vcd->first) {
        (void)fputs("$end\n", vcd->fp);
    }
    vcd->first = false;
}

bool tb_vcd_close(tb_vcd *vcd, uint64_t end_time) {
    if (vcd == NULL) {
        return true;
    }
    if (end_time != vcd->last_time) {
        (void)fprintf(vcd->fp, "#%llu\n", (unsigned long long)end_time); /* the run's end */
    }
    bool ok = ferror(vcd->fp) == 0;
    ok = fclose(vcd->fp) == 0 && ok;
    free(vcd->sigs);
    free_nets(vcd);
    free(vcd);
    return ok;
}
