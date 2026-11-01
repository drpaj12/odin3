/* tb_vcd.c — the `sim_tb --vcd` writer: an IEEE 1364 VCD (2-state) of the testbench's scalar regs
 * and wires and of every DUT instance port that is connected to an expression. A change is written
 * once per time step, with the value the signal has when the step ends (a glitch inside a step is
 * not shown). Arrays and parameters are not dumped. */
#include "sim/tb_internal.h"
#include "sim/vcd.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>

typedef struct vcd_sig {
    char code[ODIN3_SIM_VCD_CODE_MAX];
    const tb_var *var;   /* a testbench variable, or */
    const tb_conn *conn; /* a DUT port connection */
    uint32_t width;
    uint64_t last;
} vcd_sig;

struct tb_vcd {
    FILE *fp;
    vcd_sig *sigs;
    uint32_t nsigs;
    bool first;
};

static bool dumpable(const tb_var *var) {
    return !var->is_param && var->depth == 0 && var->width >= 1 && var->width <= TB_MAX_WIDTH;
}

static void write_value(tb_vcd *vcd, const vcd_sig *sig, uint64_t val) {
    if (sig->width == 1) {
        (void)fprintf(vcd->fp, "%c%s\n", (val & 1U) != 0 ? '1' : '0', sig->code);
        return;
    }
    (void)fputc('b', vcd->fp);
    for (uint32_t bit = sig->width; bit > 0; bit--) {
        (void)fputc(((val >> (bit - 1U)) & 1U) != 0 ? '1' : '0', vcd->fp);
    }
    (void)fprintf(vcd->fp, " %s\n", sig->code);
}

static void declare(tb_vcd *vcd, vcd_sig *sig, const char *kind, const char *name) {
    odin3_sim_vcd_code(vcd->nsigs, sig->code);
    (void)fprintf(vcd->fp, "$var %s %u %s %s $end\n", kind, sig->width, sig->code, name);
    vcd->nsigs++;
}

static uint32_t count_signals(const tb_module *tb) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < tb->nvars; i++) {
        count += dumpable(&tb->vars[i]) ? 1U : 0U;
    }
    for (uint32_t i = 0; i < tb->ninsts; i++) {
        count += tb->insts[i].nconns;
    }
    return count;
}

static void write_header(tb_vcd *vcd, const tb_module *tb) {
    (void)fprintf(vcd->fp, "$version odin3 sim_tb $end\n$timescale %s $end\n$scope module %s $end\n",
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
        const tb_inst *inst = &tb->insts[i];
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
        (void)fputs("$upscope $end\n", vcd->fp);
    }
    (void)fputs("$upscope $end\n$enddefinitions $end\n", vcd->fp);
}

tb_vcd *tb_vcd_open(const char *path, tb_module *tb) {
    tb_vcd *vcd = calloc(1, sizeof *vcd);
    uint32_t total = count_signals(tb);
    if (vcd != NULL) {
        vcd->sigs = calloc((size_t)total + 1U, sizeof *vcd->sigs);
        vcd->fp = vcd->sigs != NULL ? fopen(path, "w") : NULL;
    }
    if (vcd == NULL || vcd->fp == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "sim_tb: cannot write the --vcd file `%s`", path);
        if (vcd != NULL) {
            free(vcd->sigs);
        }
        free(vcd);
        return NULL;
    }
    vcd->first = true;
    write_header(vcd, tb);
    return vcd;
}

static uint64_t current(tb_ev *ev, const vcd_sig *sig) {
    if (sig->var != NULL) {
        return sig->var->val & tb_mask(sig->width);
    }
    return tb_eval(ev, sig->conn->expr, sig->width, false) & tb_mask(sig->width);
}

void tb_vcd_sample(tb_vcd *vcd, tb_ev *ev, uint64_t time) {
    bool header = false;
    for (uint32_t i = 0; i < vcd->nsigs; i++) {
        vcd_sig *sig = &vcd->sigs[i];
        uint64_t val = current(ev, sig);
        if (!vcd->first && val == sig->last) {
            continue;
        }
        if (!header) {
            (void)fprintf(vcd->fp, "#%llu\n%s", (unsigned long long)time,
                          vcd->first ? "$dumpvars\n" : "");
            header = true;
        }
        write_value(vcd, sig, val);
        sig->last = val;
    }
    if (header && vcd->first) {
        (void)fputs("$end\n", vcd->fp);
    }
    vcd->first = false;
}

bool tb_vcd_close(tb_vcd *vcd) {
    if (vcd == NULL) {
        return true;
    }
    bool ok = ferror(vcd->fp) == 0;
    ok = fclose(vcd->fp) == 0 && ok;
    free(vcd->sigs);
    free(vcd);
    return ok;
}
