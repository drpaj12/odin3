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
bool tb_lex(const tb_source *src, tb_module *tb, tb_lexed *out);

/* Evaluation state: the module, the current time and the random seed; run may be NULL for a
 * constant expression (no $time). */
typedef struct tb_ev {
    tb_module *tb;
    uint64_t time;
    int32_t seed;
    bool const_only;
    bool failed;
    const char *why; /* what set failed, or NULL for the generic message */
} tb_ev;

uint64_t tb_mask(uint32_t width);
/* Value of e in a context of w bits and signedness sg (spec section 2). */
uint64_t tb_eval(tb_ev *ev, const tb_expr *expr, uint32_t width, bool sgn);
/* Value of e assigned to a target of `width` bits. */
uint64_t tb_eval_assign(tb_ev *ev, const tb_expr *expr, uint32_t width);
/* One resolved write: a non-concatenation lvalue leaf, its value slice and its evaluated index. */
typedef struct tb_wr {
    const tb_expr *leaf;
    uint64_t val;
    uint64_t idx;
} tb_wr;
enum { TB_WR_MAX = 2 * TB_MAX_WIDTH };
/* Evaluates the indices of lv now and splits val over its leaves (a concatenation takes its parts
 * from the right); returns the leaf count, 0 with ev->failed set when lv is too large. A
 * nonblocking assignment keeps these so that its indices are those of the statement's time. */
uint32_t tb_wr_resolve(tb_ev *ev, const tb_expr *lv, uint64_t val, tb_wr *out);
void tb_wr_apply(tb_ev *ev, const tb_wr *wr);
/* Writes val to the lvalue (resolve, then apply); an index out of range drops the write. */
void tb_write(tb_ev *ev, const tb_expr *lv, uint64_t val);
bool tb_const(tb_module *tb, const tb_expr *expr, int64_t *out);
int32_t tb_random(int32_t *seed);

/* Display formatting (tb_fmt.c): formats args (first may be a format string) into a new
 * NUL-terminated string of any length; the caller frees it. NULL when out of memory. */
char *tb_format(tb_ev *ev, tb_expr *const *args, uint32_t nargs);
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

/* DUT binding (tb_dut.c): one simulated module instance of the testbench. The DUT follows the
 * Verilog regions: inputs and combinational outputs are handled in the active region
 * (tb_dut_comb); clock edges are recorded when the clock is written (tb_dut_observe) and replayed
 * in order in the NBA region (tb_dut_edges), so a register update is visible only after every
 * active process has run, whatever the order of the processes. The clock starts at 0 (2-state):
 * `initial clk = 1;` is a posedge at time 0. */
typedef struct tb_dut tb_dut;
odin3_status tb_dut_open(odin3_design *design, tb_module *tb, tb_inst *inst, tb_dut **out);
/* Applies changed inputs, settles, publishes changed outputs; true when any output changed. */
bool tb_dut_comb(tb_ev *ev, tb_dut *dut);
/* Records the clock edge, if any, since the last call. */
void tb_dut_observe(tb_ev *ev, tb_dut *dut);
/* Replays the recorded edges and publishes the outputs; true when an edge was replayed. */
bool tb_dut_edges(tb_ev *ev, tb_dut *dut);
void tb_dut_close(tb_dut *dut);

#endif
