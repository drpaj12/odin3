/* tb_run.c — the testbench scheduler: processes as explicit-stack interpreters, the event wheel,
 * the active/NBA/postponed regions (spec section 3) and the system tasks. */
#include "sim/tb_internal.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>

enum {
    STEP_LIMIT = 20000000,
    DEFAULT_DELTA_LIMIT = 100000, /* active-region rounds in one region pass */
    TIME_DELTA_LIMIT = 100000,    /* consecutive time steps at one time (#0 loops) */
    ERR_BUF = 256,
    STROBE_CAP0 = 8,
    NBA_CAP0 = 16,
    TASK_PREFIX = 8 /* strlen of "$monitor" and "$readmem" */
};

typedef struct frame {
    const tb_stmt *stmt;
    uint32_t pc;
    uint64_t cnt;
} frame;

enum { RACE_SITES = 64 };

typedef enum { P_RUN, P_TIME, P_EVENT, P_COND, P_DONE } pstate;

typedef struct proc {
    const tb_proc_def *def;
    frame st[TB_MAX_STACK];
    int sp;
    pstate state;
    uint64_t wake;
    bool woke;            /* scratch of wake_at */
    const tb_stmt *slept; /* the delay statement it sleeps on */
    uint64_t seq;         /* when it went to sleep on a delay: the order of wake-ups at one time */
    tbv prev[TB_MAX_ARGS];
    const tb_stmt *waiting;
} proc;

typedef struct run {
    tb_module *tb;
    tb_ev ev;
    proc *procs;
    uint32_t np;
    uint32_t *order;                  /* the order the processes run in the active region */
    uint64_t seq;                     /* delay sleeps so far */
    const tb_stmt *raced[RACE_SITES]; /* delay statements already warned about */
    uint32_t n_raced;
    uint32_t n_woken; /* processes woken by the last wake_at */
    tb_dut **duts;
    uint32_t nd;
    tb_wr *nbas; /* pending nonblocking writes, indices already evaluated */
    uint32_t nnba, cap_nba;
    const tb_stmt *monitor;
    bool mon_on;
    tbv mon_prev[TB_MAX_ARGS]; /* last values of the $monitor arguments except $time */
    bool mon_pending;
    const tb_stmt **strobes;
    uint32_t nstrobe, cap_strobe;
    bool finished, failed, hit_limit;
    uint64_t max_time;
    tb_vcd *vcd;
} run;

static void rfail(run *rt, const tb_stmt *stmt, const char *msg) {
    if (!rt->failed) {
        odin3_log(ODIN3_LOG_ERROR, "testbench:%u: %s", stmt != NULL ? stmt->line : 0U, msg);
    }
    rt->failed = true;
}

/* --- events --- */
static void arm(run *rt, proc *pr, const tb_stmt *stmt) {
    pr->waiting = stmt;
    for (uint32_t i = 0; i < stmt->nterms; i++) {
        tb_eval(&rt->ev, stmt->terms[i].sig, stmt->terms[i].sig->width, false, &pr->prev[i]);
    }
}

/* A change of the term's signal from old to now: any change, or the 4-state edge rule of the least
 * significant bit (posedge: 0 to 1, x or z; x or z to 1; negedge the mirror; IEEE 1364-2005 9.7.2).
 */
static bool triggered(const tb_event_term *term, const tbv *old, const tbv *now) {
    if (term->edge == 0) {
        return !tbv_same(old, now, term->sig->width);
    }
    uint32_t before = tbv_code(old, 0);
    uint32_t after = tbv_code(now, 0);
    if (before == after) {
        return false;
    }
    if (term->edge == 1) {
        return before == ODIN3_W4_0 || after == ODIN3_W4_1;
    }
    return before == ODIN3_W4_1 || after == ODIN3_W4_0;
}

static void check_waiters(run *rt) {
    for (uint32_t i = 0; i < rt->np; i++) {
        proc *pr = &rt->procs[i];
        if (pr->state == P_EVENT) {
            for (uint32_t k = 0; k < pr->waiting->nterms; k++) {
                const tb_event_term *term = &pr->waiting->terms[k];
                tbv now;
                tb_eval(&rt->ev, term->sig, term->sig->width, false, &now);
                if (triggered(term, &pr->prev[k], &now)) {
                    pr->state = P_RUN;
                }
                tbv_copy(&pr->prev[k], &now, term->sig->width);
            }
        } else if (pr->state == P_COND && tb_eval_truth(&rt->ev, pr->waiting->rhs) == ODIN3_W4_1) {
            pr->state = P_RUN;
        }
    }
}

/* One continuous assignment: its wire, or its driver slot of a wire with several drivers, takes the
 * value; true when the wire changed. */
static bool settle_one(run *rt, const tb_assign *asg) {
    const tb_var *var = &rt->tb->vars[asg->var];
    tbv val;
    tb_eval_assign(&rt->ev, asg->rhs, var->width, &val);
    if (asg->drv != 0) {
        return tb_drive(&rt->ev, asg->drv, &val);
    }
    tbv old;
    tb_var_load(&old, var, 0);
    if (tbv_same(&old, &val, var->width)) {
        return false;
    }
    tb_var_store(var, 0, &val);
    return true;
}

static void settle_assigns(run *rt) {
    for (uint32_t round = 0; round <= rt->tb->nassigns; round++) {
        bool changed = false;
        for (uint32_t i = 0; i < rt->tb->nassigns; i++) {
            changed = settle_one(rt, &rt->tb->assigns[i]) || changed;
        }
        if (!changed) {
            break;
        }
    }
}

static void post_write(run *rt) {
    settle_assigns(rt);
    for (uint32_t i = 0; i < rt->nd; i++) {
        tb_dut_observe(&rt->ev, rt->duts[i]);
    }
    check_waiters(rt);
}

/* val (from bits, unsigned) truncated or zero-extended to `to` bits. */
static void extend_to(tbv *val, uint32_t from, uint32_t to) {
    tb_resize rs = {from, to, false, false};
    tbv_extend(val, &rs);
}

/* Fails the run when an evaluation reported an error; false when the run has failed. */
static bool run_ok(run *rt) {
    if (rt->ev.failed && !rt->failed) {
        rfail(rt, NULL,
              rt->ev.why != NULL ? rt->ev.why : "expression could not be evaluated or written");
    }
    return !rt->failed;
}

/* --- system tasks --- */
static FILE *fd_stream(run *rt, uint64_t fd) {
    if (fd == 1) {
        return rt->tb->out;
    }
    return fd >= 3 && fd - 3 < TB_MAX_FILES ? rt->tb->files[fd - 3] : NULL;
}

static void print_to(run *rt, FILE *fp, const tb_expr *call, uint32_t first, bool newline) {
    char *line = tb_format(&rt->ev, call->list + first, call->n - first);
    if (line == NULL) {
        rt->failed = true;
        odin3_log(ODIN3_LOG_ERROR, "testbench: out of memory formatting output");
        return;
    }
    if (fp != NULL) {
        (void)fputs(line, fp);
        if (newline) {
            (void)fputc('\n', fp);
        }
    }
    free(line);
}

static void do_readmem(run *rt, const tb_stmt *stmt, bool hex) {
    const tb_expr *call = stmt->sys;
    char err[ERR_BUF] = "";
    if (call->n >= 2 && call->list[1]->kind == E_HIER) {
        rfail(rt, stmt,
              "$readmem into a hierarchical path is not supported (writes into a DUT "
              "signal by path are not supported)");
        return;
    }
    if (call->n < 2 || call->list[0]->kind != E_STR || call->list[1]->kind != E_VAR ||
        rt->tb->vars[call->list[1]->var].depth == 0) {
        rfail(rt, stmt, "$readmem expects (\"file\", array [, start [, end]])");
        return;
    }
    int64_t from = 0;
    int64_t to = -1;
    if ((call->n > 2 && !tb_const(rt->tb, call->list[2], &from)) ||
        (call->n > 3 && !tb_const(rt->tb, call->list[3], &to))) {
        rfail(rt, stmt, "$readmem start/end must be constants");
        return;
    }
    tb_readmem_req req = {call->list[0]->str, &rt->tb->vars[call->list[1]->var], hex, from, to, err,
                          sizeof err};
    if (!tb_readmem(&req)) {
        rfail(rt, stmt, err);
    }
}

static void push_strobe(run *rt, const tb_stmt *stmt) {
    if (rt->nstrobe == rt->cap_strobe) {
        uint32_t cap = rt->cap_strobe == 0 ? (uint32_t)STROBE_CAP0 : rt->cap_strobe * 2U;
        void *grown = realloc((void *)rt->strobes, (size_t)cap * sizeof(const tb_stmt *));
        if (grown == NULL) {
            rt->failed = true;
            return;
        }
        rt->strobes = (const tb_stmt **)grown;
        rt->cap_strobe = cap;
    }
    rt->strobes[rt->nstrobe++] = stmt;
}

static void do_close(run *rt, const tb_expr *call) {
    uint64_t fd = 0;
    if (call->n > 0) {
        (void)tb_eval_u64(&rt->ev, call->list[0], &fd);
    }
    if (fd >= 3 && fd - 3 < TB_MAX_FILES && rt->tb->files[fd - 3] != NULL) {
        (void)fclose(rt->tb->files[fd - 3]);
        rt->tb->files[fd - 3] = NULL;
    }
}

static void do_monitor(run *rt, const tb_stmt *stmt) {
    const char *name = stmt->sys->str;
    if (strcmp(name, "$monitor") == 0) {
        rt->monitor = stmt;
        rt->mon_pending = true;
    } else {
        rt->mon_on = strcmp(name, "$monitoron") == 0;
    }
}

static void do_systask(run *rt, const tb_stmt *stmt) {
    const tb_expr *call = stmt->sys;
    const char *name = call->str;
    if (strstr(name, "display") != NULL || strstr(name, "write") != NULL) {
        uint64_t fd = 1;
        if (name[1] == 'f' && (call->n == 0 || !tb_eval_u64(&rt->ev, call->list[0], &fd))) {
            fd = 0;
        }
        print_to(rt, fd_stream(rt, fd), call, name[1] == 'f' ? 1U : 0U,
                 strstr(name, "display") != NULL);
    } else if (strcmp(name, "$strobe") == 0) {
        push_strobe(rt, stmt);
    } else if (strncmp(name, "$monitor", TASK_PREFIX) == 0) {
        do_monitor(rt, stmt);
    } else if (strcmp(name, "$finish") == 0 || strcmp(name, "$stop") == 0) {
        rt->finished = true;
    } else if (strcmp(name, "$fclose") == 0) {
        do_close(rt, call);
    } else if (strncmp(name, "$readmem", TASK_PREFIX) == 0) {
        do_readmem(rt, stmt, name[TASK_PREFIX] == 'h');
    }
}

/* --- process stepping --- */
static void push_nba(run *rt, const tb_stmt *stmt) {
    tb_wr wrs[TB_WR_MAX];
    tbv val;
    tb_eval_assign(&rt->ev, stmt->rhs, stmt->lhs->width, &val);
    uint32_t count = tb_wr_resolve(&rt->ev, stmt->lhs, &val, wrs);
    if (rt->nnba + count > rt->cap_nba) {
        uint32_t cap = rt->cap_nba == 0 ? (uint32_t)NBA_CAP0 : rt->cap_nba;
        while (cap < rt->nnba + count) {
            cap *= 2U;
        }
        tb_wr *grown = realloc(rt->nbas, cap * sizeof *grown);
        if (grown == NULL) {
            rt->failed = true;
            return;
        }
        rt->nbas = grown;
        rt->cap_nba = cap;
    }
    for (uint32_t i = 0; i < count; i++) {
        rt->nbas[rt->nnba++] = wrs[i];
    }
}

static bool push(run *rt, proc *pr, const tb_stmt *stmt) {
    if (pr->sp >= TB_MAX_STACK) {
        rfail(rt, stmt, "statement nesting deeper than 48");
        return false;
    }
    pr->st[pr->sp++] = (frame){stmt, 0, 0};
    return true;
}

static void exec_leaf(run *rt, const tb_stmt *stmt) {
    switch (stmt->kind) {
    case S_ASSIGN: {
        tbv val;
        tb_eval_assign(&rt->ev, stmt->rhs, stmt->lhs->width, &val);
        tb_write(&rt->ev, stmt->lhs, &val);
        post_write(rt);
        break;
    }
    case S_NBA:
        push_nba(rt, stmt);
        break;
    case S_SYS:
        do_systask(rt, stmt);
        break;
    default:
        break;
    }
}

/* A case item matches when the expression and the label are equal on all four states (case),
 * with z and ? bits of either side ignored (casez), or with x and z bits ignored (casex). */
static bool case_match(uint8_t kind, const tbv *expr, const tbv *label, uint32_t width) {
    for (uint32_t i = 0; i < odin3_word_limbs(width); i++) {
        uint64_t care = UINT64_MAX;
        if (kind == TB_CASEZ) {
            care = ~((expr->v[i] & expr->u[i]) | (label->v[i] & label->u[i]));
        } else if (kind == TB_CASEX) {
            care = ~(expr->u[i] | label->u[i]);
        }
        if (((expr->v[i] ^ label->v[i]) & care) != 0 || ((expr->u[i] ^ label->u[i]) & care) != 0) {
            return false;
        }
    }
    return true;
}

static const tb_stmt *case_pick(run *rt, const tb_stmt *stmt) {
    uint32_t cw = stmt->rhs->width;
    const tb_stmt *dflt = NULL;
    for (uint32_t i = 0; i < stmt->n; i++) {
        if (stmt->nlab[i] == 0) {
            dflt = stmt->list[i];
        }
        for (uint32_t k = 0; k < stmt->nlab[i]; k++) {
            const tb_expr *lab = stmt->labels[i * TB_MAX_ARGS + k];
            uint32_t width = cw > lab->width ? cw : lab->width;
            tbv sel;
            tbv label;
            tb_eval(&rt->ev, stmt->rhs, width, false, &sel);
            tb_eval(&rt->ev, lab, width, false, &label);
            if (case_match(stmt->case_kind, &sel, &label, width)) {
                return stmt->list[i];
            }
        }
    }
    return dflt;
}

static bool step_block(run *rt, proc *pr, frame *frm) {
    const tb_stmt *stmt = frm->stmt;
    if (frm->pc < stmt->n) {
        return push(rt, pr, stmt->list[frm->pc++]);
    }
    pr->sp--;
    return true;
}

/* DELAY, EVENT and WAIT: the first visit blocks (or passes when a wait is already true), the
 * second runs the body. */
static bool step_wait(run *rt, proc *pr, frame *frm) {
    const tb_stmt *stmt = frm->stmt;
    if (frm->pc != 0) {
        pr->sp--;
        return push(rt, pr, stmt->a);
    }
    frm->pc = 1;
    if (stmt->kind == S_DELAY) {
        uint64_t delay = 0;
        if (!tb_eval_u64(&rt->ev, stmt->rhs, &delay)) {
            delay = 0; /* a delay with x or z bits is 0 */
        }
        pr->wake = rt->ev.time + delay;
        pr->state = P_TIME;
        pr->seq = rt->seq++;
        pr->slept = stmt;
    } else if (stmt->kind == S_EVENT) {
        arm(rt, pr, stmt);
        pr->state = P_EVENT;
    } else if (tb_eval_truth(&rt->ev, stmt->rhs) != ODIN3_W4_1) {
        pr->waiting = stmt;
        pr->state = P_COND;
    }
    return pr->state == P_RUN;
}

static bool step_branch(run *rt, proc *pr, const tb_stmt *stmt) {
    const tb_stmt *next = NULL;
    if (stmt->kind == S_IF) {
        next = tb_eval_truth(&rt->ev, stmt->rhs) == ODIN3_W4_1 ? stmt->a : stmt->b;
    } else {
        next = case_pick(rt, stmt);
    }
    pr->sp--;
    return next == NULL || push(rt, pr, next);
}

static bool step_for(run *rt, proc *pr, frame *frm) {
    const tb_stmt *stmt = frm->stmt;
    if (frm->pc == 0) {
        frm->pc = 1;
        exec_leaf(rt, stmt->init);
        return true;
    }
    if (frm->pc == 2) {
        frm->pc = 1;
        exec_leaf(rt, stmt->step);
        return true;
    }
    if (tb_eval_truth(&rt->ev, stmt->rhs) != ODIN3_W4_1) {
        pr->sp--;
        return true;
    }
    frm->pc = 2;
    return push(rt, pr, stmt->a);
}

/* The repeat count; a signed negative count, or one with x or z bits, is 0 (IEEE 1364-2005
 * 9.6). */
static uint64_t repeat_count(run *rt, const tb_expr *expr) {
    tbv val;
    uint64_t cnt = 0;
    tb_eval(&rt->ev, expr, expr->width, expr->sgn, &val);
    bool negative = expr->sgn && expr->width > 0 && tbv_code(&val, expr->width - 1U) == 1U;
    return !negative && tbv_u64(&val, expr->width, &cnt) ? cnt : 0;
}

static bool step_loop_kind(run *rt, proc *pr, frame *frm) {
    const tb_stmt *stmt = frm->stmt;
    if (stmt->kind == S_WHILE) {
        if (tb_eval_truth(&rt->ev, stmt->rhs) != ODIN3_W4_1) {
            pr->sp--;
            return true;
        }
        return push(rt, pr, stmt->a);
    }
    if (stmt->kind == S_REPEAT) {
        if (frm->pc == 0) {
            frm->pc = 1;
            frm->cnt = repeat_count(rt, stmt->rhs);
        }
        if (frm->cnt == 0) {
            pr->sp--;
            return true;
        }
        frm->cnt--;
    }
    return push(rt, pr, stmt->a);
}

/* A task call: the first visit copies the input arguments in and runs the body, the second
 * copies the output arguments back to the caller. */
static bool step_call(run *rt, proc *pr, frame *frm) {
    const tb_stmt *stmt = frm->stmt;
    const tb_task *task = &rt->tb->tasks[stmt->task];
    if (frm->pc == 0) {
        for (uint32_t i = 0; i < task->nargs; i++) {
            const tb_var *var = &rt->tb->vars[task->vars[i]];
            if ((task->dir[i] & TB_ARG_IN) != 0) {
                tbv val;
                tb_eval_assign(&rt->ev, stmt->args[i], var->width, &val);
                tb_var_store(var, 0, &val);
            }
        }
        post_write(rt);
        frm->pc = 1;
        return push(rt, pr, task->body);
    }
    for (uint32_t i = 0; i < task->nargs; i++) {
        if ((task->dir[i] & TB_ARG_OUT) != 0) {
            const tb_var *var = &rt->tb->vars[task->vars[i]];
            tbv val;
            tb_var_load(&val, var, 0);
            extend_to(&val, var->width, stmt->args[i]->width);
            tb_write(&rt->ev, stmt->args[i], &val);
        }
    }
    post_write(rt);
    pr->sp--;
    return true;
}

/* One step of the top frame; returns false when the process blocked or finished. */
static bool step_loop(run *rt, proc *pr, frame *frm) {
    const tb_stmt *stmt = frm->stmt;
    switch (stmt->kind) {
    case S_BLOCK:
        return step_block(rt, pr, frm);
    case S_CALL:
        return step_call(rt, pr, frm);
    case S_DELAY:
    case S_EVENT:
    case S_WAIT:
        return step_wait(rt, pr, frm);
    case S_IF:
    case S_CASE:
        return step_branch(rt, pr, stmt);
    case S_FOR:
        return step_for(rt, pr, frm);
    case S_WHILE:
    case S_REPEAT:
    case S_FOREVER:
        return step_loop_kind(rt, pr, frm);
    default:
        exec_leaf(rt, stmt);
        pr->sp--;
        return true;
    }
}

static void run_proc(run *rt, proc *pr) {
    for (long steps = 0; pr->state == P_RUN && !rt->finished && run_ok(rt); steps++) {
        if (steps > STEP_LIMIT) {
            rfail(rt, pr->sp > 0 ? pr->st[pr->sp - 1].stmt : NULL,
                  "process runs without waiting (infinite zero-delay loop)");
            return;
        }
        if (pr->sp == 0) {
            if (!pr->def->always) {
                pr->state = P_DONE;
                return;
            }
            (void)push(rt, pr, pr->def->body);
            continue;
        }
        if (!step_loop(rt, pr, &pr->st[pr->sp - 1])) {
            return;
        }
    }
}

/* --- regions --- */
static bool any_runnable(const run *rt) {
    for (uint32_t i = 0; i < rt->np; i++) {
        if (rt->procs[i].state == P_RUN) {
            return true;
        }
    }
    return false;
}

static void active_region(run *rt) {
    for (uint32_t guard = 0; guard < DEFAULT_DELTA_LIMIT && !rt->finished && run_ok(rt); guard++) {
        for (uint32_t i = 0; i < rt->np; i++) {
            run_proc(rt, &rt->procs[rt->order[i]]);
        }
        bool changed = false;
        for (uint32_t dev_no = 0; dev_no < rt->nd; dev_no++) {
            changed = tb_dut_comb(&rt->ev, rt->duts[dev_no]) || changed;
        }
        post_write(rt);
        if (!any_runnable(rt) && !changed) {
            return;
        }
    }
    if (!rt->finished && run_ok(rt)) {
        rfail(rt, NULL,
              "time step does not converge (combinational feedback through the testbench)");
    }
}

/* The NBA region: the DUTs' recorded clock edges first (they sample the inputs as the active
 * region left them, and their register outputs are published here), then the testbench's
 * nonblocking assignments. True when anything happened, so the active region must run again. */
static bool nba_region(run *rt) {
    bool progress = false;
    for (uint32_t i = 0; i < rt->nd; i++) {
        progress = tb_dut_edges(&rt->ev, rt->duts[i]) || progress;
    }
    for (uint32_t i = 0; i < rt->nnba; i++) {
        tb_wr_apply(&rt->ev, &rt->nbas[i]);
    }
    progress = progress || rt->nnba > 0;
    rt->nnba = 0;
    post_write(rt);
    return progress;
}

static bool mon_arg_is_time(const tb_expr *arg) {
    return arg->kind == E_SYS && strcmp(arg->str, "$time") == 0;
}

/* $monitor fires when an argument other than $time (and the format string) changes. */
static bool mon_changed(run *rt) {
    const tb_expr *call = rt->monitor->sys;
    bool changed = rt->mon_pending;
    for (uint32_t i = 0; i < call->n && i < TB_MAX_ARGS; i++) {
        const tb_expr *arg = call->list[i];
        if (arg->kind == E_STR || mon_arg_is_time(arg)) {
            continue;
        }
        tbv val;
        tb_eval(&rt->ev, arg, arg->width, arg->sgn, &val);
        changed = changed || !tbv_same(&val, &rt->mon_prev[i], arg->width);
        tbv_copy(&rt->mon_prev[i], &val, arg->width);
    }
    return changed;
}

static void postponed(run *rt) {
    for (uint32_t i = 0; i < rt->nstrobe; i++) {
        print_to(rt, rt->tb->out, rt->strobes[i]->sys, 0, true);
    }
    rt->nstrobe = 0;
    if (rt->monitor != NULL && rt->mon_on && mon_changed(rt)) {
        print_to(rt, rt->tb->out, rt->monitor->sys, 0, true);
        rt->mon_pending = false;
    }
}

static void time_step(run *rt) {
    bool progress = false;
    do {
        active_region(rt);
        progress = nba_region(rt);
    } while ((any_runnable(rt) || progress) && !rt->finished && run_ok(rt));
    postponed(rt);
    if (rt->vcd != NULL && !rt->failed) {
        tb_vcd_sample(rt->vcd, &rt->ev, rt->ev.time);
    }
    (void)run_ok(rt);
}

static bool next_time(run *rt, uint64_t *next) {
    bool found = false;
    for (uint32_t i = 0; i < rt->np; i++) {
        if (rt->procs[i].state == P_TIME && (!found || rt->procs[i].wake < *next)) {
            *next = rt->procs[i].wake;
            found = true;
        }
    }
    rt->hit_limit = found && *next > rt->max_time;
    return found && !rt->hit_limit;
}

/* The output connections of the DUTs that drive a whole variable (the drivers they add). */
static tb_conn *var_output(tb_module *tb, uint32_t inst, uint32_t index) {
    tb_conn *conn = &tb->insts[inst].conns[index];
    bool drives = conn->is_out && conn->expr != NULL && conn->expr->kind == E_VAR;
    return drives ? conn : NULL;
}

static void count_conn_drivers(tb_module *tb, uint32_t *count) {
    for (uint32_t i = 0; i < tb->ninsts; i++) {
        for (uint32_t k = 0; k < tb->insts[i].nconns; k++) {
            const tb_conn *conn = var_output(tb, i, k);
            count[conn != NULL ? conn->expr->var : 0] += conn != NULL ? 1U : 0U;
        }
    }
}

/* Gives the output connections to variables with several drivers their slots, from *next on. */
static void number_conn_drivers(tb_module *tb, const uint32_t *count, uint32_t *next) {
    for (uint32_t i = 0; i < tb->ninsts; i++) {
        for (uint32_t k = 0; k < tb->insts[i].nconns; k++) {
            tb_conn *conn = var_output(tb, i, k);
            if (conn == NULL) {
                continue;
            }
            conn->drv = 0;
            if (count[conn->expr->var] > 1) {
                tb->drvs[*next].var = conn->expr->var;
                conn->drv = ++*next;
            }
        }
    }
}

/* A wire with several drivers (continuous assignments and DUT outputs) gets one driver slot per
 * driver (design note section 6); the slots start z. */
static odin3_status setup_drivers(tb_module *tb) {
    uint32_t *count = calloc((size_t)tb->nvars + 1U, sizeof *count);
    if (count == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < tb->nassigns; i++) {
        count[tb->assigns[i].var]++;
    }
    count_conn_drivers(tb, count);
    uint32_t total = 0;
    for (uint32_t i = 0; i < tb->nvars; i++) {
        total += count[i] > 1 ? count[i] : 0U;
    }
    tb->ndrvs = 0;
    tb->drvs = total > 0 ? tb_alloc(tb, (size_t)total * sizeof *tb->drvs) : NULL;
    if (total > 0 && tb->drvs == NULL) {
        free(count);
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t next = 0;
    for (uint32_t i = 0; i < tb->nassigns; i++) {
        tb_assign *asg = &tb->assigns[i];
        asg->drv = 0;
        if (count[asg->var] > 1) {
            tb->drvs[next].var = asg->var;
            asg->drv = ++next;
        }
    }
    number_conn_drivers(tb, count, &next);
    for (uint32_t i = 0; i < next; i++) {
        tbv_fill(ODIN3_W4_Z, &tb->drvs[i].val, tb->vars[tb->drvs[i].var].width);
    }
    tb->ndrvs = next;
    free(count);
    return ODIN3_OK;
}

static odin3_status setup(run *rt, odin3_design *design) {
    odin3_status st = ODIN3_OK;
    rt->procs = calloc(rt->tb->nprocs + 1U, sizeof *rt->procs);
    rt->duts = (tb_dut **)calloc((size_t)rt->tb->ninsts + 1U, sizeof(tb_dut *));
    if (rt->procs == NULL || rt->duts == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    rt->np = rt->tb->nprocs;
    rt->order = calloc((size_t)rt->np + 1U, sizeof *rt->order);
    if (rt->order == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < rt->np; i++) {
        rt->order[i] = i;
    }
    for (uint32_t i = 0; i < rt->np; i++) {
        rt->procs[i].def = &rt->tb->procs[i];
        rt->procs[i].st[0] = (frame){rt->tb->procs[i].body, 0, 0};
        rt->procs[i].sp = 1;
    }
    for (uint32_t i = 0; i < rt->tb->ninsts && st == ODIN3_OK; i++) {
        st = tb_dut_open(design, rt->tb, &rt->tb->insts[i], &rt->duts[rt->nd]);
        rt->nd += st == ODIN3_OK ? 1U : 0U;
    }
    return st == ODIN3_OK ? setup_drivers(rt->tb) : st;
}

static void teardown(run *rt) {
    for (uint32_t i = 0; i < rt->nd; i++) {
        tb_dut_close(rt->duts[i]);
    }
    for (uint32_t i = 0; i < TB_MAX_FILES; i++) {
        if (rt->tb->files[i] != NULL) {
            (void)fclose(rt->tb->files[i]);
            rt->tb->files[i] = NULL;
        }
    }
    free(rt->procs);
    free(rt->order);
    free((void *)rt->duts);
    free(rt->nbas);
    free((void *)rt->strobes);
}

/* One `tb race:` WARNING per delay statement: it wakes at the same time as a process that comes
 * earlier in the source but runs after it, because it went to sleep first. Source order says
 * the other way; iverilog's event queue (and now this run) orders by sleep time, so a bench that
 * relies on either order is racy. */
static void warn_races(run *rt, uint64_t when) {
    uint32_t count = rt->n_woken;
    for (uint32_t k = 0; k < count; k++) {
        const proc *pr = &rt->procs[rt->order[k]];
        bool moved = false;
        for (uint32_t j = k + 1U; j < count && !moved; j++) {
            moved = rt->order[j] < rt->order[k];
        }
        bool seen = false;
        for (uint32_t i = 0; moved && i < rt->n_raced && !seen; i++) {
            seen = rt->raced[i] == pr->slept;
        }
        if (!moved || seen || rt->n_raced >= RACE_SITES) {
            continue;
        }
        rt->raced[rt->n_raced++] = pr->slept;
        odin3_log(
            ODIN3_LOG_WARN,
            "tb race: the delay at line %u wakes at time %llu together with a process that "
            "is earlier in the source; they run in the order they went to sleep, as in iverilog",
            pr->slept != NULL ? pr->slept->line : 0U, (unsigned long long)when);
    }
}

/* Wakes the processes sleeping until `when`. They run first, in the order they went to sleep
 * (a delay scheduled earlier fires earlier, as in event-queue simulators), then the others in
 * source order: a process that waits for an edge at the moment a clock toggles, because its own
 * delay ended then too, sees that edge only when it went to sleep before the clock process did. */
static void wake_at(run *rt, uint64_t when) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < rt->np; i++) {
        proc *pr = &rt->procs[i];
        pr->woke = pr->state == P_TIME && pr->wake == when;
        if (!pr->woke) {
            continue;
        }
        pr->state = P_RUN;
        uint32_t at = count++;
        while (at > 0 && rt->procs[rt->order[at - 1U]].seq > pr->seq) {
            rt->order[at] = rt->order[at - 1U];
            at--;
        }
        rt->order[at] = i;
    }
    rt->n_woken = count;
    warn_races(rt, when);
    for (uint32_t i = 0; i < rt->np; i++) {
        if (!rt->procs[i].woke) {
            rt->order[count++] = i;
        }
    }
}

/* Advances time from wake to wake; fails when time stays put for TIME_DELTA_LIMIT steps. */
static void event_loop(run *rt) {
    uint64_t next = 0;
    uint32_t same_time = 0;
    while (!rt->finished && !rt->failed && next_time(rt, &next)) {
        same_time = next == rt->ev.time ? same_time + 1U : 0U;
        if (same_time > TIME_DELTA_LIMIT) {
            rfail(rt, NULL, "time does not advance (a zero-delay `#0` loop)");
            return;
        }
        rt->ev.time = next;
        wake_at(rt, next);
        time_step(rt);
    }
}

odin3_status tb_run_end(odin3_design *design, tb_module *tb, const tb_options *opts, tb_end *end) {
    run rt = {0};
    rt.tb = tb;
    rt.ev.tb = tb;
    rt.ev.seed = (int32_t)opts->seed;
    rt.max_time = opts->max_time;
    rt.mon_on = true;
    tb->out = opts->out;
    tb->plusargs = opts->plusargs;
    tb->n_plusargs = opts->plusargs != NULL ? opts->n_plusargs : 0U;
    odin3_status st = setup(&rt, design);
    if (st == ODIN3_OK && opts->vcd != NULL) {
        rt.vcd = tb_vcd_open(opts->vcd, tb, opts);
        st = rt.vcd != NULL ? ODIN3_OK : ODIN3_ERR_IO;
    }
    if (st == ODIN3_OK) {
        settle_assigns(&rt);
        time_step(&rt);
        event_loop(&rt);
        st = rt.failed ? ODIN3_ERR_INVALID_ARG : ODIN3_OK;
    }
    if (!tb_vcd_close(rt.vcd, rt.ev.time) && st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "sim_tb: write error on the --vcd file");
        st = ODIN3_ERR_IO;
    }
    end->time = rt.ev.time;
    end->hit_limit = rt.hit_limit && !rt.finished;
    teardown(&rt);
    return st;
}

odin3_status tb_run(odin3_design *design, tb_module *tb, const tb_options *opts,
                    uint64_t *finish_time) {
    tb_end end = {0, false};
    odin3_status st = tb_run_end(design, tb, opts, &end);
    *finish_time = end.time;
    return st;
}
