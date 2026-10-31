/* tb_run.c — the testbench scheduler: processes as explicit-stack interpreters, the event wheel,
 * the active/NBA/postponed regions (spec section 3) and the system tasks. */
#include "sim/tb_internal.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>

enum { LINE_MAX = 1024, STEP_LIMIT = 20000000, DEFAULT_DELTA_LIMIT = 100000 };

typedef struct frame {
    const tb_stmt *s;
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

typedef struct nba {
    const tb_expr *lv;
    uint64_t val;
} nba;

typedef struct run {
    tb_module *tb;
    tb_ev ev;
    proc *procs;
    uint32_t np;
    tb_dut **duts;
    uint32_t nd;
    nba *nbas;
    uint32_t nnba, cap_nba;
    const tb_stmt *monitor;
    bool mon_on;
    char mon_last[LINE_MAX];
    bool mon_pending;
    const tb_stmt **strobes;
    uint32_t nstrobe, cap_strobe;
    bool finished, failed;
    uint64_t max_time;
} run;

static void rfail(run *r, const tb_stmt *s, const char *msg) {
    if (!r->failed) {
        odin3_log(ODIN3_LOG_ERROR, "testbench:%u: %s", s != NULL ? s->line : 0U, msg);
    }
    r->failed = true;
}

/* --- events --- */
static uint64_t term_value(run *r, const tb_event_term *t) {
    return tb_eval(&r->ev, t->sig, t->sig->width, false) & tb_mask(t->sig->width);
}

static void arm(run *r, proc *p, const tb_stmt *s) {
    p->waiting = s;
    for (uint32_t i = 0; i < s->nterms; i++) {
        p->prev[i] = term_value(r, &s->terms[i]);
    }
}

static bool triggered(const tb_event_term *t, uint64_t old, uint64_t now) {
    if (t->edge == 0) {
        return old != now;
    }
    bool before = (old & 1U) != 0;
    bool after = (now & 1U) != 0;
    return t->edge == 1 ? (!before && after) : (before && !after);
}

static void check_waiters(run *r) {
    for (uint32_t i = 0; i < r->np; i++) {
        proc *p = &r->procs[i];
        if (p->state == P_EVENT) {
            for (uint32_t k = 0; k < p->waiting->nterms; k++) {
                uint64_t now = term_value(r, &p->waiting->terms[k]);
                if (triggered(&p->waiting->terms[k], p->prev[k], now)) {
                    p->state = P_RUN;
                }
                p->prev[k] = now;
            }
        } else if (p->state == P_COND &&
                   tb_eval(&r->ev, p->waiting->rhs, p->waiting->rhs->width, false) != 0) {
            p->state = P_RUN;
        }
    }
}

static void settle_assigns(run *r) {
    for (uint32_t round = 0; round <= r->tb->nassigns; round++) {
        bool changed = false;
        for (uint32_t i = 0; i < r->tb->nassigns; i++) {
            tb_var *v = &r->tb->vars[r->tb->assigns[i].var];
            uint64_t val = tb_eval_assign(&r->ev, r->tb->assigns[i].rhs, v->width);
            changed = changed || val != v->val;
            v->val = val;
        }
        if (!changed) {
            break;
        }
    }
}

static void post_write(run *r) {
    settle_assigns(r);
    check_waiters(r);
}

/* --- system tasks --- */
static FILE *fd_stream(run *r, uint64_t fd) {
    if (fd == 1) {
        return r->tb->out;
    }
    return fd >= 3 && fd - 3 < TB_MAX_FILES ? r->tb->files[fd - 3] : NULL;
}

static void print_to(run *r, FILE *fp, const tb_expr *call, uint32_t first, bool newline) {
    char line[LINE_MAX];
    tb_format(&r->ev, call->list + first, call->n - first, line, sizeof line);
    if (fp != NULL) {
        (void)fputs(line, fp);
        if (newline) {
            (void)fputc('\n', fp);
        }
    }
}

static void do_readmem(run *r, const tb_stmt *s, bool hex) {
    const tb_expr *call = s->sys;
    char err[256] = "";
    if (call->n < 2 || call->list[0]->kind != E_STR || call->list[1]->kind != E_VAR ||
        r->tb->vars[call->list[1]->var].depth == 0) {
        rfail(r, s, "$readmem expects (\"file\", array [, start [, end]])");
        return;
    }
    int64_t from = 0;
    int64_t to = -1;
    if ((call->n > 2 && !tb_const(r->tb, call->list[2], &from)) ||
        (call->n > 3 && !tb_const(r->tb, call->list[3], &to))) {
        rfail(r, s, "$readmem start/end must be constants");
        return;
    }
    tb_readmem_req req = {call->list[0]->str, &r->tb->vars[call->list[1]->var], hex, from, to, err,
                          sizeof err};
    if (!tb_readmem(&req)) {
        rfail(r, s, err);
    }
}

static void push_strobe(run *r, const tb_stmt *s) {
    if (r->nstrobe == r->cap_strobe) {
        uint32_t cap = r->cap_strobe == 0 ? 8U : r->cap_strobe * 2U;
        const tb_stmt **grown = realloc(r->strobes, cap * sizeof *grown);
        if (grown == NULL) {
            r->failed = true;
            return;
        }
        r->strobes = grown;
        r->cap_strobe = cap;
    }
    r->strobes[r->nstrobe++] = s;
}

static void do_close(run *r, const tb_expr *call) {
    uint64_t fd = call->n > 0 ? tb_eval(&r->ev, call->list[0], 32, false) : 0;
    if (fd >= 3 && fd - 3 < TB_MAX_FILES && r->tb->files[fd - 3] != NULL) {
        (void)fclose(r->tb->files[fd - 3]);
        r->tb->files[fd - 3] = NULL;
    }
}

static void do_monitor(run *r, const tb_stmt *s) {
    const char *name = s->sys->str;
    if (strcmp(name, "$monitor") == 0) {
        r->monitor = s;
        r->mon_pending = true;
    } else {
        r->mon_on = name[8] == 'o' && name[9] == 'n';
    }
}

static void do_systask(run *r, const tb_stmt *s) {
    const tb_expr *call = s->sys;
    const char *name = call->str;
    if (strstr(name, "display") != NULL || strstr(name, "write") != NULL) {
        uint64_t fd = 1;
        if (name[1] == 'f') {
            fd = call->n > 0 ? tb_eval(&r->ev, call->list[0], 32, false) : 0;
        }
        print_to(r, fd_stream(r, fd), call, name[1] == 'f' ? 1U : 0U,
                 strstr(name, "display") != NULL);
    } else if (strcmp(name, "$strobe") == 0) {
        push_strobe(r, s);
    } else if (strncmp(name, "$monitor", 8) == 0) {
        do_monitor(r, s);
    } else if (strcmp(name, "$finish") == 0 || strcmp(name, "$stop") == 0) {
        r->finished = true;
    } else if (strcmp(name, "$fclose") == 0) {
        do_close(r, call);
    } else if (strncmp(name, "$readmem", 8) == 0) {
        do_readmem(r, s, name[8] == 'h');
    }
}

/* --- process stepping --- */
static void push_nba(run *r, const tb_stmt *s) {
    if (r->nnba == r->cap_nba) {
        uint32_t cap = r->cap_nba == 0 ? 16U : r->cap_nba * 2U;
        nba *grown = realloc(r->nbas, cap * sizeof *grown);
        if (grown == NULL) {
            r->failed = true;
            return;
        }
        r->nbas = grown;
        r->cap_nba = cap;
    }
    r->nbas[r->nnba++] = (nba){s->lhs, tb_eval_assign(&r->ev, s->rhs, s->lhs->width)};
}

static bool push(run *r, proc *p, const tb_stmt *s) {
    if (p->sp >= TB_MAX_STACK) {
        rfail(r, s, "statement nesting deeper than 48");
        return false;
    }
    p->st[p->sp++] = (frame){s, 0, 0};
    return true;
}

static void exec_leaf(run *r, const tb_stmt *s) {
    switch (s->kind) {
    case S_ASSIGN:
        tb_write(&r->ev, s->lhs, tb_eval_assign(&r->ev, s->rhs, s->lhs->width));
        post_write(r);
        break;
    case S_NBA:
        push_nba(r, s);
        break;
    case S_SYS:
        do_systask(r, s);
        break;
    default:
        break;
    }
}

static const tb_stmt *case_pick(run *r, const tb_stmt *s) {
    uint32_t cw = s->rhs->width;
    const tb_stmt *dflt = NULL;
    for (uint32_t i = 0; i < s->n; i++) {
        if (s->nlab[i] == 0) {
            dflt = s->list[i];
        }
        for (uint32_t k = 0; k < s->nlab[i]; k++) {
            const tb_expr *lab = s->labels[i * TB_MAX_ARGS + k];
            uint32_t w = cw > lab->width ? cw : lab->width;
            if (tb_eval(&r->ev, s->rhs, w, false) == tb_eval(&r->ev, lab, w, false)) {
                return s->list[i];
            }
        }
    }
    return dflt;
}

static bool step_block(run *r, proc *p, frame *f) {
    const tb_stmt *s = f->s;
    if (f->pc < s->n) {
        return push(r, p, s->list[f->pc++]);
    }
    p->sp--;
    return true;
}

/* DELAY, EVENT and WAIT: the first visit blocks (or passes when a wait is already true), the
 * second runs the body. */
static bool step_wait(run *r, proc *p, frame *f) {
    const tb_stmt *s = f->s;
    if (f->pc != 0) {
        p->sp--;
        return push(r, p, s->a);
    }
    f->pc = 1;
    if (s->kind == S_DELAY) {
        p->wake = r->ev.time + tb_eval(&r->ev, s->rhs, s->rhs->width, false);
        p->state = P_TIME;
    } else if (s->kind == S_EVENT) {
        arm(r, p, s);
        p->state = P_EVENT;
    } else if (tb_eval(&r->ev, s->rhs, s->rhs->width, false) == 0) {
        p->waiting = s;
        p->state = P_COND;
    }
    return p->state == P_RUN;
}

static bool step_branch(run *r, proc *p, const tb_stmt *s) {
    const tb_stmt *next = NULL;
    if (s->kind == S_IF) {
        next = tb_eval(&r->ev, s->rhs, s->rhs->width, false) != 0 ? s->a : s->b;
    } else {
        next = case_pick(r, s);
    }
    p->sp--;
    return next == NULL || push(r, p, next);
}

static bool step_for(run *r, proc *p, frame *f) {
    const tb_stmt *s = f->s;
    if (f->pc == 0) {
        f->pc = 1;
        exec_leaf(r, s->init);
        return true;
    }
    if (f->pc == 2) {
        f->pc = 1;
        exec_leaf(r, s->step);
        return true;
    }
    if (tb_eval(&r->ev, s->rhs, s->rhs->width, false) == 0) {
        p->sp--;
        return true;
    }
    f->pc = 2;
    return push(r, p, s->a);
}

static bool step_loop_kind(run *r, proc *p, frame *f) {
    const tb_stmt *s = f->s;
    if (s->kind == S_WHILE) {
        if (tb_eval(&r->ev, s->rhs, s->rhs->width, false) == 0) {
            p->sp--;
            return true;
        }
        return push(r, p, s->a);
    }
    if (s->kind == S_REPEAT) {
        if (f->pc == 0) {
            f->pc = 1;
            f->cnt = tb_eval(&r->ev, s->rhs, s->rhs->width, false);
        }
        if (f->cnt == 0) {
            p->sp--;
            return true;
        }
        f->cnt--;
    }
    return push(r, p, s->a);
}

/* One step of the top frame; returns false when the process blocked or finished. */
static bool step_loop(run *r, proc *p, frame *f) {
    const tb_stmt *s = f->s;
    switch (s->kind) {
    case S_BLOCK:
        return step_block(r, p, f);
    case S_DELAY:
    case S_EVENT:
    case S_WAIT:
        return step_wait(r, p, f);
    case S_IF:
    case S_CASE:
        return step_branch(r, p, s);
    case S_FOR:
        return step_for(r, p, f);
    case S_WHILE:
    case S_REPEAT:
    case S_FOREVER:
        return step_loop_kind(r, p, f);
    default:
        exec_leaf(r, s);
        p->sp--;
        return true;
    }
}

static void run_proc(run *r, proc *p) {
    for (long steps = 0; p->state == P_RUN && !r->finished && !r->failed; steps++) {
        if (steps > STEP_LIMIT) {
            rfail(r, p->sp > 0 ? p->st[p->sp - 1].s : NULL,
                  "process runs without waiting (infinite zero-delay loop)");
            return;
        }
        if (p->sp == 0) {
            if (!p->def->always) {
                p->state = P_DONE;
                return;
            }
            (void)push(r, p, p->def->body);
            continue;
        }
        if (!step_loop(r, p, &p->st[p->sp - 1])) {
            return;
        }
    }
}

/* --- regions --- */
static bool any_runnable(const run *r) {
    for (uint32_t i = 0; i < r->np; i++) {
        if (r->procs[i].state == P_RUN) {
            return true;
        }
    }
    return false;
}

static void active_region(run *r) {
    for (uint32_t guard = 0; guard < DEFAULT_DELTA_LIMIT && !r->finished && !r->failed; guard++) {
        for (uint32_t i = 0; i < r->np; i++) {
            run_proc(r, &r->procs[i]);
        }
        bool changed = false;
        for (uint32_t d = 0; d < r->nd; d++) {
            changed = tb_dut_sync(&r->ev, r->duts[d]) || changed;
        }
        post_write(r);
        if (!any_runnable(r) && !changed) {
            return;
        }
    }
    if (!r->finished) {
        rfail(r, NULL,
              "time step does not converge (combinational feedback through the testbench)");
    }
}

static void nba_region(run *r) {
    for (uint32_t i = 0; i < r->nnba; i++) {
        tb_write(&r->ev, r->nbas[i].lv, r->nbas[i].val);
    }
    r->nnba = 0;
    post_write(r);
}

static void postponed(run *r) {
    for (uint32_t i = 0; i < r->nstrobe; i++) {
        print_to(r, r->tb->out, r->strobes[i]->sys, 0, true);
    }
    r->nstrobe = 0;
    if (r->monitor != NULL && r->mon_on) {
        char line[LINE_MAX];
        tb_format(&r->ev, r->monitor->sys->list, r->monitor->sys->n, line, sizeof line);
        if (r->mon_pending || strcmp(line, r->mon_last) != 0) {
            (void)fprintf(r->tb->out, "%s\n", line);
            (void)snprintf(r->mon_last, sizeof r->mon_last, "%s", line);
            r->mon_pending = false;
        }
    }
}

static void time_step(run *r) {
    do {
        active_region(r);
        nba_region(r);
    } while ((any_runnable(r) || r->nnba > 0) && !r->finished && !r->failed);
    postponed(r);
}

static bool next_time(run *r, uint64_t *next) {
    bool found = false;
    for (uint32_t i = 0; i < r->np; i++) {
        if (r->procs[i].state == P_TIME && (!found || r->procs[i].wake < *next)) {
            *next = r->procs[i].wake;
            found = true;
        }
    }
    return found && *next <= r->max_time;
}

static odin3_status setup(run *r, odin3_design *design) {
    odin3_status st = ODIN3_OK;
    r->procs = calloc(r->tb->nprocs + 1U, sizeof *r->procs);
    r->duts = calloc(r->tb->ninsts + 1U, sizeof *r->duts);
    if (r->procs == NULL || r->duts == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    r->np = r->tb->nprocs;
    for (uint32_t i = 0; i < r->np; i++) {
        r->procs[i].def = &r->tb->procs[i];
        r->procs[i].st[0] = (frame){r->tb->procs[i].body, 0, 0};
        r->procs[i].sp = 1;
    }
    for (uint32_t i = 0; i < r->tb->ninsts && st == ODIN3_OK; i++) {
        st = tb_dut_open(design, r->tb, &r->tb->insts[i], &r->duts[r->nd]);
        r->nd += st == ODIN3_OK ? 1U : 0U;
    }
    return st;
}

static void teardown(run *r) {
    for (uint32_t i = 0; i < r->nd; i++) {
        tb_dut_close(r->duts[i]);
    }
    for (uint32_t i = 0; i < TB_MAX_FILES; i++) {
        if (r->tb->files[i] != NULL) {
            (void)fclose(r->tb->files[i]);
            r->tb->files[i] = NULL;
        }
    }
    free(r->procs);
    free(r->duts);
    free(r->nbas);
    free(r->strobes);
}

odin3_status tb_run(odin3_design *design, tb_module *tb, const tb_options *opts,
                    uint64_t *finish_time) {
    run r = {0};
    r.tb = tb;
    r.ev.tb = tb;
    r.ev.seed = (int32_t)opts->seed;
    r.max_time = opts->max_time;
    r.mon_on = true;
    tb->out = opts->out;
    odin3_status st = setup(&r, design);
    if (st == ODIN3_OK) {
        settle_assigns(&r);
        time_step(&r);
        uint64_t next = 0;
        while (!r.finished && !r.failed && next_time(&r, &next)) {
            r.ev.time = next;
            for (uint32_t i = 0; i < r.np; i++) {
                if (r.procs[i].state == P_TIME && r.procs[i].wake == next) {
                    r.procs[i].state = P_RUN;
                }
            }
            time_step(&r);
        }
        st = r.failed ? ODIN3_ERR_INVALID_ARG : ODIN3_OK;
    }
    *finish_time = r.ev.time;
    teardown(&r);
    return st;
}
