/*
 * tb.h — the testbench interpreter (spec docs/specs/2026-10-31-testbench-design.md): a small
 * self-contained Verilog subset (lexer, parser, evaluator, event scheduler) that drives DUT modules
 * simulated by the cycle simulator. Private to src/sim and src/passes.
 */
#ifndef ODIN3_SIM_TB_H
#define ODIN3_SIM_TB_H

#include "ir/design.h"
#include "odin3/odin3.h"
#include "sim/signals.h"
#include "sim/sim.h"
#include "sim/word4.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum {
    TB_NAME = 64,
    TB_MAX_WIDTH = 1024, /* widest value (signal, literal, concatenation) */
    TB_LIMBS = TB_MAX_WIDTH / ODIN3_WORD_LIMB_BITS,
    TB_TIME_BITS = 64, /* $time */
    TB_MAX_STACK = 48,
    TB_MAX_ARGS = 16,
    TB_MAX_FILES = 8,
    TB_MAX_PLUSARGS = 16,
    TB_PLUSARG_LEN = 256,
    TB_INT_BITS = 32,         /* integer, $random and $fopen results */
    TB_FD_BASE = 3,           /* first $fopen descriptor (0 to 2 are reserved) */
    TB_RAND_MULT = 16807,     /* Park-Miller minimal standard generator */
    TB_RAND_MOD = 2147483647, /* 2^31 - 1 */
    TB_OP_CHARS = 4           /* operator text, with its terminator */
};

/*
 * A 4-state value of up to TB_MAX_WIDTH bits (design note
 * docs/specs/2026-11-05-sim-4state-design.md section 2): planes v and u of odin3_word_limbs(width)
 * limbs each, bit k the code (u_k v_k) = 0, 1, x = 2, z = 3 (word4.h). Only the first
 * odin3_word_limbs(width) limbs of a value of width bits are meaningful; every operation reads and
 * writes only those, so a value of 64 bits or fewer costs one limb per plane.
 */
typedef struct tbv {
    uint64_t v[TB_LIMBS];
    uint64_t u[TB_LIMBS];
} tbv;

/* --- tokens --- */
typedef enum { TK_EOF, TK_ID, TK_NUM, TK_STR, TK_SYS, TK_OP } tb_tok_kind;

typedef struct tb_tok {
    tb_tok_kind kind;
    char text[TB_NAME]; /* identifier, operator or system-task name; strings in str */
    char *str;          /* TK_STR: owned by the lexer's list */
    uint64_t num;       /* TK_NUM: the low 64 bits of the v plane */
    uint64_t *nv;       /* TK_NUM: the value planes, odin3_word_limbs of the literal's width */
    uint64_t *nu;
    uint32_t width; /* TK_NUM: 0 when unsized */
    bool sized;
    bool is_signed;
    bool xfill; /* TK_NUM: unsized with a leading x or z digit (extends with it, section 5) */
    uint32_t line;
} tb_tok;

/* --- expressions --- */
typedef enum {
    E_NUM,
    E_STR,
    E_VAR,
    E_BITSEL,
    E_PART,
    E_MEM,
    E_CONCAT,
    E_REPL,
    E_UN,
    E_BIN,
    E_COND,
    E_SYS,
    E_HIER,   /* read of a DUT signal by hierarchical path: var = instance, num = signal ID */
    E_HIERMEM /* a word array of the DUT (`inst.mem[i]` or, with a NULL, the whole array as the
                 target of $readmem): var = instance, ids/n = signal ID of word 0..n-1, a = the
                 index */
} tb_ekind;

typedef struct tb_expr {
    tb_ekind kind;
    char op[TB_OP_CHARS];
    uint32_t width; /* self-determined */
    bool sgn;
    uint64_t num; /* E_PART: msb in num, lsb in aux; E_REPL: count */
    uint32_t aux;
    const uint64_t *nv, *nu; /* E_NUM: the value planes (width bits) */
    bool xfill;              /* E_NUM: extends past its width with its top bit's x or z */
    int var;                 /* variable index */
    struct tb_expr *a, *b, *c;
    struct tb_expr **list; /* E_CONCAT items, E_SYS args */
    const uint32_t *ids;   /* E_HIERMEM: the signal ID of each word */
    uint32_t n;
    char *str;
    uint32_t line;
} tb_expr;

/* --- statements --- */
typedef enum {
    S_NOP,
    S_BLOCK,
    S_ASSIGN,
    S_NBA,
    S_DELAY,
    S_EVENT,
    S_WAIT,
    S_IF,
    S_CASE,
    S_FOR,
    S_WHILE,
    S_REPEAT,
    S_FOREVER,
    S_SYS,
    S_CALL /* task call: task = index into tb_module.tasks, args/n = the actual arguments */
} tb_skind;

typedef struct tb_event_term {
    int edge; /* 0 any change, 1 posedge, 2 negedge */
    tb_expr *sig;
} tb_event_term;

typedef struct tb_stmt tb_stmt;
struct tb_stmt {
    tb_skind kind;
    tb_expr *delay;     /* S_NBA: the intra-assignment delay of `a <= #d b`, else NULL */
    tb_expr *lhs, *rhs; /* assign; cond for if/while/wait; count for repeat; delay amount */
    tb_stmt *a, *b;     /* body / else; for: init in a2 */
    tb_stmt *init, *step;
    tb_stmt **list;
    tb_expr **labels; /* case: labels per item, flat; items in list, nlab[i] labels each */
    uint32_t *nlab;
    uint32_t n;
    tb_event_term *terms;
    uint32_t nterms;
    tb_expr *sys;   /* S_SYS: the call */
    tb_expr **args; /* S_CALL: actual arguments, n of them */
    char *callee;   /* S_CALL: the task name until the parse links it */
    int task;
    uint8_t case_kind; /* S_CASE: TB_CASE, TB_CASEZ or TB_CASEX */
    uint32_t line;
};

enum { TB_CASE = 0, TB_CASEZ = 1, TB_CASEX = 2 };

/* --- module --- */
enum { TB_PULL_NONE = 0, TB_PULL_UP = 1, TB_PULL_DOWN = 2 };

typedef struct tb_var {
    char name[TB_NAME];
    uint32_t width;
    bool sgn, is_wire, is_param;
    uint8_t
        pull; /* wire: TB_PULL_UP / TB_PULL_DOWN from pullup(w) / pulldown(w): z bits read 1 / 0 */
    uint32_t depth; /* array length, 0 for a scalar */
    uint32_t lo;    /* [h:lo] range: bit selects are refused unless lo is 0 (parse only) */
    bool refinable; /* parse: declared by direction only; a later reg/wire/integer sets its type */
    /* The value planes: max(depth, 1) words of odin3_word_limbs(width) limbs each (word i of an
     * array at limb i * limbs). */
    uint64_t *v;
    uint64_t *u;
} tb_var;

typedef struct tb_conn {
    char port[TB_NAME];
    tb_expr *expr; /* input: any expression; output: E_VAR */
    uint32_t sim_port;
    bool is_out;
    uint32_t width;
    uint32_t drv; /* output to a wire with several drivers: its driver slot + 1; else 0 */
} tb_conn;

typedef struct tb_inst {
    char module[TB_NAME], name[TB_NAME];
    tb_conn *conns;
    uint32_t nconns;
    uint32_t line;
    odin3_sim *sim; /* the DUT's simulator, built on first use (parse or run), owned here */
    odin3_sim_signals *sigs; /* its signal table, built with the first hierarchical read */
    bool poked;              /* a state was poked since the last settle (tb_dut_comb settles) */
} tb_inst;

typedef struct tb_assign {
    int var;
    tb_expr *rhs;
    uint32_t drv; /* the wire has several drivers: this one's slot + 1; else 0 */
} tb_assign;

/* One driver of a wire with several (assigns and DUT outputs): its value; the wire is the
 * resolution of all of them (odin3_w4_resolve, design note section 6). */
typedef struct tb_drv {
    int var;
    tbv val;
} tb_drv;

typedef struct tb_proc_def {
    tb_stmt *body;
    bool always;
} tb_proc_def;

/* A task: static storage, its formal arguments are variables of the module (copied in on a call
 * for input and inout, copied out for output and inout). */
enum { TB_ARG_IN = 1, TB_ARG_OUT = 2 };
typedef struct tb_task {
    char name[TB_NAME];
    tb_stmt *body;
    int *vars;    /* variable index of each formal argument */
    uint8_t *dir; /* TB_ARG_IN | TB_ARG_OUT per argument */
    uint32_t nargs;
    uint32_t line;
} tb_task;

/* A testbench-side module instance (flattened into the testbench): its path ("u0", "u0.ia") and
 * module. Its variables are named "<path>.<name>". */
typedef struct tb_sub {
    char path[TB_NAME];
    char module[TB_NAME];
} tb_sub;

typedef struct tb_module {
    char name[TB_NAME];
    tb_var *vars;
    uint32_t nvars, cap_vars;
    tb_inst *insts;
    uint32_t ninsts;
    tb_sub *subs;
    uint32_t nsubs;
    tb_assign *assigns;
    uint32_t nassigns;
    tb_proc_def *procs;
    uint32_t nprocs;
    tb_task *tasks;
    uint32_t ntasks;
    void **allocs; /* every parse allocation, freed with the module */
    uint32_t nallocs, cap_allocs;
    uint64_t time_scale;         /* timescale unit / precision (power of ten), for %t */
    char timescale[TB_NAME];     /* VCD $timescale, from `timescale ("1 ns" when absent) */
    bool saw_hier;               /* parse: the source has a hierarchical name */
    odin3_design *design;        /* the design the DUTs come from; NULL: no hierarchical reads */
    FILE *out;                   /* $display output (fd 1) */
    FILE *files[TB_MAX_FILES];   /* $fopen descriptors 3.. */
    const char *const *plusargs; /* $test$plusargs and $value$plusargs read these (tb_options) */
    uint32_t n_plusargs;
    bool four_state; /* x/z values: regs start x, wires z, the DUTs are 4-state simulators */
    tb_drv *drvs;    /* driver slots of the wires with several drivers (set up by the run) */
    uint32_t ndrvs;
} tb_module;

/* Source text and the file name used in messages. */
typedef struct tb_source {
    const char *file;
    const char *text;
    odin3_design *design; /* the DUTs' design, for hierarchical reads; may be NULL */
    bool four_state;      /* see tb_module.four_state; false: the 2-state interpreter */
} tb_source;

/* Maps a line of the joined, preprocessed testbench text to its source file and line (sim_tb joins
 * several files and expands `include, so the joined line is not the file's line). file[k], line[k]
 * belong to joined line k + 1; a NULL file means unknown. Borrowed by tb_set_locmap until it is
 * called again with NULL. Used by error and warning texts only. */
typedef struct tb_locmap {
    uint32_t nlines;
    const char **file;
    const uint32_t *line;
} tb_locmap;
enum { TB_WHERE_MAX = 600 };
void tb_set_locmap(const tb_locmap *map);
/* "file:line" for a joined line, written to buf (size bytes) and returned. With no map: the
 * fallback name and the line as given. */
const char *tb_where(const char *fallback, uint32_t line, char *buf, size_t size);

/* Parses src and picks the top: name, or the first module when NULL.
 * ODIN3_ERR_INVALID_ARG (logged "file:line: unsupported: ...") for a parse failure. */
odin3_status tb_parse(const tb_source *src, const char *top, tb_module **out);
void tb_module_free(tb_module *tb);

/* --- run --- */
typedef struct tb_options {
    uint64_t max_time;
    uint64_t seed;
    FILE *out;
    const char *vcd; /* NULL: none */
    /* The plusargs of the run ("name=value" or "name", a leading '+' is optional); the array
     * must outlive the run. */
    const char *const *plusargs;
    uint32_t n_plusargs;
    /* vcd_all: the waveform also holds every net of every DUT instance, one scalar variable per
     * net; namer (may be NULL) names them, ctx is namer_ctx (odin3_sim_signals_create_nets). */
    bool vcd_all;
    odin3_sim_net_namer namer;
    void *namer_ctx;
} tb_options;

/* How a run ended: the last simulation time, and whether it stopped because the next event lay
 * beyond max_time (not `$finish`, not an empty event queue: a clock with no `$finish` ends so). */
typedef struct tb_end {
    uint64_t time;
    bool hit_limit;
} tb_end;

/* Runs the testbench against the modules of design. *finish_time gets the last time. */
odin3_status tb_run(odin3_design *design, tb_module *tb, const tb_options *opts,
                    uint64_t *finish_time);

/* As tb_run, with the way the run ended in *end. */
odin3_status tb_run_end(odin3_design *design, tb_module *tb, const tb_options *opts, tb_end *end);

#endif
