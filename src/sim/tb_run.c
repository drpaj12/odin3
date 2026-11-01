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

typedef enum { P_RUN, P_TIME, P_EVENT, P_COND, P_DONE } pstate;

typedef struct proc {
    const tb_proc_def *def;
    frame st[TB_MAX_STACK];
    int sp;
    pstate state;
    uint64_t wake;
    uint64_t prev[TB_MAX_ARGS];
    const tb_stmt *waiting;
} proc;

typedef struct run {
    tb_module *tb;
    tb_ev ev;
    proc *procs;
    uint32_t np;
    tb_dut **duts;
    uint32_t nd;
    tb_wr *nbas; /* pending nonblocking writes, indices already evaluated */
    uint32_t nnba, cap_nba;
    const tb_stmt *monitor;
    bool mon_on;
    uint64_t mon_prev[TB_MAX_ARGS]; /* last values of the $monitor arguments except $time */
    bool mon_pending;
    const tb_stmt **strobes;
    uint32_t nstrobe, cap_strobe;
    bool finished, failed, hit_limit;
    uint64_t max_time;
} run;

static void rfail(run *rt, const tb_stmt *stmt, const char *msg) {
    if (!rt->failed) {
        odin3_log(ODIN3_LOG_ERROR, "testbench:%u: %s", stmt != NULL ? stmt->line : 0U, msg);
    }
    rt->failed = true;
}

/* --- events --- */
static uint64_t term_value(run *rt, const tb_event_term *term) {
    return tb_eval(&rt->ev, term->sig, term->sig->width, false) & tb_mask(term->sig->width);
}

static void arm(run *rt, proc *pr, const tb_stmt *stmt) {
    pr->waiting = stmt;
    for (uint32_t i = 0; i < stmt->nterms; i++) {
        pr->prev[i] = term_value(rt, &stmt->terms[i]);
    }
}

static bool triggered(const tb_event_term *term, uint64_t old, uint64_t now) {
    if (term->edge == 0) {
        return old != now;
    }
    bool before = (old & 1U) != 0;
    bool after = (now & 1U) != 0;
    return term->edge == 1 ? (!before && after) : (before && !after);
}

static void check_waiters(run *rt) {
    for (uint32_t i = 0; i < rt->np; i++) {
        proc *pr = &rt->procs[i];
        if (pr->state == P_EVENT) {
            for (uint32_t k = 0; k < pr->waiting->nterms; k++) {
                uint64_t now = term_value(rt, &pr->waiting->terms[k]);
                if (triggered(&pr->waiting->terms[k], pr->prev[k], now)) {
                    pr->state = P_RUN;
                }
                pr->prev[k] = now;
            }
        } else if (pr->state == P_COND &&
                   tb_eval(&rt->ev, pr->waiting->rhs, pr->waiting->rhs->width, false) != 0) {
            pr->state = P_RUN;
        }
    }
}

static void settle_assigns(run *rt) {
    for (uint32_t round = 0; round <= rt->tb->nassigns; round++) {
        bool changed = false;
        for (uint32_t i = 0; i < rt->tb->nassigns; i++) {
            tb_var *var = &rt->tb->vars[rt->tb->assigns[i].var];
            uint64_t val = tb_eval_assign(&rt->ev, rt->tb->assigns[i].rhs, var->width);
            changed = changed || val != var->val;
            var->val = val;
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
    uint64_t fd = call->n > 0 ? tb_eval(&rt->ev, call->list[0], TB_INT_BITS, false) : 0;
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
        if (name[1] == 'f') {
            fd = call->n > 0 ? tb_eval(&rt->ev, call->list[0], TB_INT_BITS, false) : 0;
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
    uint64_t val = tb_eval_assign(&rt->ev, stmt->rhs, stmt->lhs->width);
    uint32_t count = tb_wr_resolve(&rt->ev, stmt->lhs, val, wrs);
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
    case S_ASSIGN:
        tb_write(&rt->ev, stmt->lhs, tb_eval_assign(&rt->ev, stmt->rhs, stmt->lhs->width));
        post_write(rt);
        break;
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
            if (tb_eval(&rt->ev, stmt->rhs, width, false) == tb_eval(&rt->ev, lab, width, false)) {
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
        pr->wake = rt->ev.time + tb_eval(&rt->ev, stmt->rhs, stmt->rhs->width, false);
        pr->state = P_TIME;
    } else if (stmt->kind == S_EVENT) {
        arm(rt, pr, stmt);
        pr->state = P_EVENT;
    } else if (tb_eval(&rt->ev, stmt->rhs, stmt->rhs->width, false) == 0) {
        pr->waiting = stmt;
        pr->state = P_COND;
    }
    return pr->state == P_RUN;
}

static bool step_branch(run *rt, proc *pr, const tb_stmt *stmt) {
    const tb_stmt *next = NULL;
    if (stmt->kind == S_IF) {
        next = tb_eval(&rt->ev, stmt->rhs, stmt->rhs->width, false) != 0 ? stmt->a : stmt->b;
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
    if (tb_eval(&rt->ev, stmt->rhs, stmt->rhs->width, false) == 0) {
        pr->sp--;
        return true;
    }
    frm->pc = 2;
    return push(rt, pr, stmt->a);
}

/* The repeat count; a signed negative count is 0 (IEEE 1364-2005 9.6). */
static uint64_t repeat_count(run *rt, const tb_expr *expr) {
    uint64_t cnt = tb_eval(&rt->ev, expr, expr->width, expr->sgn) & tb_mask(expr->width);
    bool negative = expr->sgn && expr->width > 0 && ((cnt >> (expr->width - 1U)) & 1U) != 0;
    return negative ? 0 : cnt;
}

static bool step_loop_kind(run *rt, proc *pr, frame *frm) {
    const tb_stmt *stmt = frm->stmt;
    if (stmt->kind == S_WHILE) {
        if (tb_eval(&rt->ev, stmt->rhs, stmt->rhs->width, false) == 0) {
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

/* One step of the top frame; returns false when the process blocked or finished. */
static bool step_loop(run *rt, proc *pr, frame *frm) {
    const tb_stmt *stmt = frm->stmt;
    switch (stmt->kind) {
    case S_BLOCK:
        return step_block(rt, pr, frm);
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
            run_proc(rt, &rt->procs[i]);
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
        uint64_t val = tb_eval(&rt->ev, arg, arg->width, arg->sgn) & tb_mask(arg->width);
        changed = changed || val != rt->mon_prev[i];
        rt->mon_prev[i] = val;
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

static odin3_status setup(run *rt, odin3_design *design) {
    odin3_status st = ODIN3_OK;
    rt->procs = calloc(rt->tb->nprocs + 1U, sizeof *rt->procs);
    rt->duts = (tb_dut **)calloc((size_t)rt->tb->ninsts + 1U, sizeof(tb_dut *));
    if (rt->procs == NULL || rt->duts == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    rt->np = rt->tb->nprocs;
    for (uint32_t i = 0; i < rt->np; i++) {
        rt->procs[i].def = &rt->tb->procs[i];
        rt->procs[i].st[0] = (frame){rt->tb->procs[i].body, 0, 0};
        rt->procs[i].sp = 1;
    }
    for (uint32_t i = 0; i < rt->tb->ninsts && st == ODIN3_OK; i++) {
        st = tb_dut_open(design, rt->tb, &rt->tb->insts[i], &rt->duts[rt->nd]);
        rt->nd += st == ODIN3_OK ? 1U : 0U;
    }
    return st;
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
    free((void *)rt->duts);
    free(rt->nbas);
    free((void *)rt->strobes);
}

static void wake_at(run *rt, uint64_t when) {
    for (uint32_t i = 0; i < rt->np; i++) {
        if (rt->procs[i].state == P_TIME && rt->procs[i].wake == when) {
            rt->procs[i].state = P_RUN;
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
    odin3_status st = setup(&rt, design);
    if (st == ODIN3_OK) {
        settle_assigns(&rt);
        time_step(&rt);
        event_loop(&rt);
        st = rt.failed ? ODIN3_ERR_INVALID_ARG : ODIN3_OK;
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
