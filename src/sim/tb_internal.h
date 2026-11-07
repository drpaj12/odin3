/* tb_internal.h — shared by the testbench interpreter's files (tb_lex, tb_parse, tb_expr, ...). */
#ifndef ODIN3_SIM_TB_INTERNAL_H
#define ODIN3_SIM_TB_INTERNAL_H

#include "sim/tb.h"

#include <stddef.h>

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

/* --- 4-state values (tb_val.c): the operations read and write the first odin3_word_limbs(width)
 * limbs of each plane only, and keep the bits at width and above in the top limb 0 --- */
void tbv_zero(tbv *val, uint32_t width);
/* Every bit gets code (0, 1, 2 = x, 3 = z). */
void tbv_fill(uint32_t code, tbv *val, uint32_t width);
/* A known value: num truncated to width (zero above 64 bits). */
void tbv_set_u64(uint64_t num, tbv *val, uint32_t width);
/* Bit 0 gets code, the others 0 (a 1-bit result in a wider context). */
void tbv_set_code(uint32_t code, tbv *val, uint32_t width);
void tbv_copy(tbv *dst, const tbv *src, uint32_t width);
bool tbv_known(const tbv *val, uint32_t width);
/* lhs === rhs over width bits. */
bool tbv_same(const tbv *lhs, const tbv *rhs, uint32_t width);
/* The code of bit pos. */
uint32_t tbv_code(const tbv *val, uint32_t pos);
/* The value as an unsigned number: false when a bit is x or z; a value that does not fit in 64
 * bits gives UINT64_MAX. */
bool tbv_u64(const tbv *val, uint32_t width, uint64_t *out);

/* A width change in place: from `from` to `to` bits; the bits past `from` are the sign bit's code
 * when sgn, the top bit's x or z when xfill and it is one (an unsized x literal), else 0. */
typedef struct tb_resize {
    uint32_t from;
    uint32_t to;
    bool sgn;
    bool xfill;
} tb_resize;
void tbv_extend(tbv *val, const tb_resize *rs);

/* Planes of some width that a bit field is read from or written to (a variable word, a value). */
typedef struct tb_planes {
    uint64_t *v;
    uint64_t *u;
    uint32_t width;
} tb_planes;
typedef struct tb_cplanes {
    const uint64_t *v;
    const uint64_t *u;
    uint32_t width;
} tb_cplanes;
typedef struct tb_field {
    uint32_t lsb;
    uint32_t width;
} tb_field;
/* dst (field.width bits) = src[field.lsb +: field.width]; bits past src.width read 0. */
void tb_field_get(tbv *dst, tb_cplanes src, tb_field field);
/* dst[field.lsb +: field.width] = src; bits past dst.width are dropped. */
void tb_field_put(tb_planes dst, tb_field field, const tbv *src);
/* Word index of var (0 for a scalar) as planes; index < max(depth, 1). */
tb_planes tb_var_word(const tb_var *var, uint32_t index);
void tb_var_load(tbv *dst, const tb_var *var, uint32_t index);
void tb_var_store(const tb_var *var, uint32_t index, const tbv *src);
/* Every bit of every word of var gets code. */
void tb_var_fill(const tb_var *var, uint32_t code);

/* Digits of a based number (bin, oct, hex: x, z and ? digits allowed; dec: digits, or one x or z)
 * with underscores; the value of all the digits in *out, *bits its width (digits * bits per digit,
 * at most TB_MAX_WIDTH; dec: TB_MAX_WIDTH). False on any other character or no digit. */
typedef struct tb_digits {
    const char *text;
    size_t len;
    unsigned base;
} tb_digits;
bool tb_parse_digits(const tb_digits *digits, tbv *out, uint32_t *bits);

/* --- operators (tb_ops.c; design note section 5): results replace the left operand --- */
typedef struct tb_binop {
    tbv *lhs; /* operand and result */
    const tbv *rhs;
    uint32_t width; /* both operands and the result */
    bool sgn;       /* two's complement (/, %, relational) */
} tb_binop;
/* + - * / % & | ^ ~^. */
void tb_op_arith(const char *op, const tb_binop *bo);
/* Unary - and ~ (unary + leaves the value). */
void tb_op_unary(const char *op, tbv *val, uint32_t width);
/* == != === !== < <= > >=: the result code. */
uint32_t tb_op_compare(const char *op, const tb_binop *bo);
typedef struct tb_shiftop {
    tbv *lhs; /* operand and result */
    const tbv *amount;
    uint32_t width;
    uint32_t amount_width;
    bool sgn; /* >>> of a signed operand */
} tb_shiftop;
/* << <<< >> >>>. */
void tb_op_shift(const char *op, const tb_shiftop *so);
typedef struct tb_powop {
    tbv *base; /* operand and result */
    const tbv *exp;
    uint32_t width;
    uint32_t exp_width;
    bool base_sgn;
    bool exp_sgn;
} tb_powop;
void tb_op_pow(const tb_powop *op);
/* lhs = the ?: merge of lhs and rhs: a bit with the same code in both keeps it, else x. */
void tb_op_merge(tbv *lhs, const tbv *rhs, uint32_t width);
/* !code: 0 and 1 swap, x and z give x. */
uint32_t tb_code_not(uint32_t code);

/* Value of e in a context of width bits and signedness sgn (spec section 2, design note section
 * 5): *out holds width bits (max(width, e->width)). */
void tb_eval(tb_ev *ev, const tb_expr *expr, uint32_t width, bool sgn, tbv *out);
/* Value of e assigned to a target of `width` bits. */
void tb_eval_assign(tb_ev *ev, const tb_expr *expr, uint32_t width, tbv *out);
/* Truth of e as a condition: code 1, 0 or x (design note section 5). */
uint32_t tb_eval_truth(tb_ev *ev, const tb_expr *expr);
/* e as an unsigned number (self-determined); false when it has x or z bits. */
bool tb_eval_u64(tb_ev *ev, const tb_expr *expr, uint64_t *out);
/* One resolved write: a non-concatenation lvalue leaf, its value slice (leaf->width bits) and
 * its evaluated index (UINT64_MAX when the index has x or z bits: the write is dropped). */
typedef struct tb_wr {
    const tb_expr *leaf;
    uint64_t idx;
    tbv val;
} tb_wr;
enum { TB_WR_MAX = 64 }; /* leaves of one lvalue */
/* Evaluates the indices of lv now and splits val (lv->width bits) over its leaves (a
 * concatenation takes its parts from the right); returns the leaf count, 0 with ev->failed set
 * when lv is too large. A nonblocking assignment keeps these so that its indices are those of the
 * statement's time. */
uint32_t tb_wr_resolve(tb_ev *ev, const tb_expr *lv, const tbv *val, tb_wr *out);
void tb_wr_apply(tb_ev *ev, const tb_wr *wr);
/* Writes val to the lvalue (resolve, then apply); an index out of range or unknown drops the
 * write. */
void tb_write(tb_ev *ev, const tb_expr *lv, const tbv *val);
/* A constant expression as a known number; false when not constant or not known. */
bool tb_const(tb_module *tb, const tb_expr *expr, int64_t *out);
/* A constant expression assigned to `width` bits (x and z kept); false when not constant. */
bool tb_const_val(tb_module *tb, const tb_expr *expr, uint32_t width, tbv *out);
/* Driver slot drv (slot + 1) of a wire with several drivers takes val; the wire becomes the
 * resolution of its drivers. True when the wire's value changed. */
bool tb_drive(tb_ev *ev, uint32_t drv, const tbv *val);
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
 * active process has run, whatever the order of the processes. A clock edge follows the 4-state
 * rule of @(posedge) (0 to 1, 0 to x or z, x or z to 1, and the mirror for negedge; the clock
 * starts x, so `initial clk = 1;` is a posedge at time 0); in 2-state mode the clock starts at 0.
 * The DUT's own clock input stays 2-state: a posedge sets it to 1, a negedge to 0. */
typedef struct tb_dut tb_dut;
odin3_status tb_dut_open(odin3_design *design, const tb_module *tb, tb_inst *inst, tb_dut **out);
/* Applies changed inputs, settles, publishes changed outputs; true when any output changed. */
bool tb_dut_comb(tb_ev *ev, tb_dut *dut);
/* Records the clock edge, if any, since the last call. */
void tb_dut_observe(tb_ev *ev, tb_dut *dut);
/* Replays the recorded edges and publishes the outputs; true when an edge was replayed. */
bool tb_dut_edges(tb_ev *ev, tb_dut *dut);
void tb_dut_close(tb_dut *dut);

/* Hierarchical reads of DUT signals (tb_dut.c). The simulator of an instance is built on the first
 * use, at parse time for a hierarchical name, and kept in the instance until the module is freed.
 */
/* Builds the simulator of instance `inst` of the design if it has none (4-state when four_state).
 */
odin3_status tb_inst_sim(odin3_design *design, tb_inst *inst, bool four_state);
/* A DUT signal found by path: its ID in the DUT's signal table and its width. */
typedef struct tb_hier_sig {
    uint32_t id;
    uint32_t width;
} tb_hier_sig;
/* Where a hierarchical read stands in the testbench source, for its errors. */
typedef struct tb_src_loc {
    const char *file;
    uint32_t line;
} tb_src_loc;
/* Finds the signal `path` (relative to the DUT top, as odin3_sim_signals_find) of inst. ODIN3_ERR_
 * INVALID_ARG (logged at loc) when the DUT has no such signal or it is wider than TB_MAX_WIDTH.
 * The DUT's simulator is built with tb->four_state. */
odin3_status tb_inst_signal(tb_module *tb, tb_inst *inst, const char *path, tb_src_loc loc,
                            tb_hier_sig *out);
/* The value of signal `id` (its width) as of the last settle of the DUT. */
void tb_inst_peek(const tb_inst *inst, uint32_t id, tbv *out);
/* The value of signal `id` of any signal table (its width, 4-state codes) as of the last settle. */
void tb_sig_peek(const odin3_sim_signals *sigs, uint32_t id, tbv *out);
/* Frees the simulator and signal table of inst. */
void tb_inst_release(tb_inst *inst);

/* VCD output of a testbench run (tb_vcd.c): dumps every scalar reg and wire of the testbench and
 * the connected ports of every DUT instance (in a $scope per instance), with x and z, as the values
 * at the end of each time step that changed one. With opts->vcd_all each instance scope also holds
 * the scopes and nets of the DUT (odin3_sim_signals_create_nets, named by opts->namer). */
typedef struct tb_vcd tb_vcd;
/* Opens path and writes the header; NULL (logged) when the file cannot be written or on no memory.
 */
tb_vcd *tb_vcd_open(const char *path, const tb_module *tb, const tb_options *opts);
/* Writes the values that changed at time (the first call writes them all, as $dumpvars). */
void tb_vcd_sample(tb_vcd *vcd, tb_ev *ev, uint64_t time);
/* Writes the end time when later than the last sample, closes the file; false when a write
 * failed. NULL is a no-op returning true. */
bool tb_vcd_close(tb_vcd *vcd, uint64_t end_time);

#endif
