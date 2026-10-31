/* tb_internal.h — shared by the testbench interpreter's files (tb_lex, tb_parse, tb_expr, ...). */
#ifndef ODIN3_SIM_TB_INTERNAL_H
#define ODIN3_SIM_TB_INTERNAL_H

#include "sim/tb.h"

typedef struct tb_lexed {
    tb_tok *toks;
    uint32_t n;
} tb_lexed;

void *tb_alloc(tb_module *tb, size_t size);
void tb_error(const char *file, uint32_t line, const char *what);
bool tb_lex(const char *file, const char *text, tb_module *tb, tb_lexed *out);

/* Evaluation state: the module, the current time and the random seed; run may be NULL for a
 * constant expression (no $time). */
typedef struct tb_ev {
    tb_module *tb;
    uint64_t time;
    int32_t seed;
    bool const_only;
    bool failed;
} tb_ev;

uint64_t tb_mask(uint32_t width);
/* Value of e in a context of w bits and signedness sg (spec section 2). */
uint64_t tb_eval(tb_ev *ev, const tb_expr *e, uint32_t w, bool sg);
/* Value of e assigned to a target of `width` bits. */
uint64_t tb_eval_assign(tb_ev *ev, const tb_expr *e, uint32_t width);
/* Writes val to the lvalue; returns false on an index out of range (the write is dropped). */
void tb_write(tb_ev *ev, const tb_expr *lv, uint64_t val);
bool tb_const(tb_module *tb, const tb_expr *e, int64_t *out);
int32_t tb_random(int32_t *seed);

/* Display formatting (tb_fmt.c): formats args (first may be a format string) into out. */
void tb_format(tb_ev *ev, tb_expr *const *args, uint32_t nargs, char *out, size_t cap);
bool tb_fmt_valid(const char *fmt, char *bad);
typedef struct tb_readmem_req {
    const char *file;
    tb_var *mem;
    bool hex;
    int64_t from, to; /* addresses; to < 0: the last word */
    char *err;
    size_t errcap;
} tb_readmem_req;
bool tb_readmem(const tb_readmem_req *req);

/* DUT binding (tb_dut.c): one simulated module instance of the testbench. */
typedef struct tb_dut tb_dut;
odin3_status tb_dut_open(odin3_design *design, tb_module *tb, tb_inst *inst, tb_dut **out);
/* Applies the inputs, edges the clock when it changed, settles, publishes changed outputs to the
 * testbench wires; true when any output changed. */
bool tb_dut_sync(tb_ev *ev, tb_dut *dut);
void tb_dut_close(tb_dut *dut);

#endif
