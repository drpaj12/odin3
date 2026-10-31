/*
 * tb.h — the testbench interpreter (spec docs/specs/2026-10-31-testbench-design.md): a small
 * self-contained Verilog subset (lexer, parser, evaluator, event scheduler) that drives DUT modules
 * simulated by the cycle simulator. Private to src/sim and src/passes.
 */
#ifndef ODIN3_SIM_TB_H
#define ODIN3_SIM_TB_H

#include "ir/design.h"
#include "odin3/odin3.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum { TB_NAME = 64, TB_MAX_WIDTH = 64, TB_MAX_STACK = 48, TB_MAX_ARGS = 16, TB_MAX_FILES = 8 };

/* --- tokens --- */
typedef enum { TK_EOF, TK_ID, TK_NUM, TK_STR, TK_SYS, TK_OP } tb_tok_kind;

typedef struct tb_tok {
    tb_tok_kind kind;
    char text[TB_NAME]; /* identifier, operator or system-task name; strings in str */
    char *str;          /* TK_STR: owned by the lexer's list */
    uint64_t num;
    uint32_t width; /* TK_NUM: 0 when unsized */
    bool sized;
    bool is_signed;
    uint32_t line;
} tb_tok;

/* --- expressions --- */
typedef enum {
    E_NUM, E_STR, E_VAR, E_BITSEL, E_PART, E_MEM, E_CONCAT, E_REPL, E_UN, E_BIN, E_COND, E_SYS
} tb_ekind;

typedef struct tb_expr {
    tb_ekind kind;
    char op[4];
    uint32_t width; /* self-determined */
    bool sgn;
    uint64_t num; /* E_NUM; E_PART: msb in num, lsb in aux; E_REPL: count */
    uint32_t aux;
    int var; /* variable index */
    struct tb_expr *a, *b, *c;
    struct tb_expr **list; /* E_CONCAT items, E_SYS args */
    uint32_t n;
    char *str;
    uint32_t line;
} tb_expr;

/* --- statements --- */
typedef enum {
    S_NOP, S_BLOCK, S_ASSIGN, S_NBA, S_DELAY, S_EVENT, S_WAIT, S_IF, S_CASE, S_FOR, S_WHILE,
    S_REPEAT, S_FOREVER, S_SYS
} tb_skind;

typedef struct tb_event_term {
    int edge; /* 0 any change, 1 posedge, 2 negedge */
    tb_expr *sig;
} tb_event_term;

typedef struct tb_stmt tb_stmt;
struct tb_stmt {
    tb_skind kind;
    tb_expr *lhs, *rhs; /* assign; cond for if/while/wait; count for repeat; delay amount */
    tb_stmt *a, *b;     /* body / else; for: init in a2 */
    tb_stmt *init, *step;
    tb_stmt **list;
    tb_expr **labels; /* case: labels per item, flat; items in list, nlab[i] labels each */
    uint32_t *nlab;
    uint32_t n;
    tb_event_term *terms;
    uint32_t nterms;
    tb_expr *sys; /* S_SYS: the call */
    uint32_t line;
};

/* --- module --- */
typedef struct tb_var {
    char name[TB_NAME];
    uint32_t width;
    bool sgn, is_wire, is_param;
    uint64_t val;
    uint32_t depth; /* array length, 0 for a scalar */
    uint64_t *mem;
} tb_var;

typedef struct tb_conn {
    char port[TB_NAME];
    tb_expr *expr; /* input: any expression; output: E_VAR */
    uint32_t sim_port;
    bool is_out;
    uint32_t width;
    uint64_t last; /* last value applied (input) or published (output) */
    bool seen;
} tb_conn;

typedef struct tb_inst {
    char module[TB_NAME], name[TB_NAME];
    tb_conn *conns;
    uint32_t nconns;
    uint32_t line;
} tb_inst;

typedef struct tb_assign {
    int var;
    tb_expr *rhs;
} tb_assign;

typedef struct tb_proc_def {
    tb_stmt *body;
    bool always;
} tb_proc_def;

typedef struct tb_module {
    char name[TB_NAME];
    tb_var *vars;
    uint32_t nvars, cap_vars;
    tb_inst *insts;
    uint32_t ninsts;
    tb_assign *assigns;
    uint32_t nassigns;
    tb_proc_def *procs;
    uint32_t nprocs;
    void **allocs; /* every parse allocation, freed with the module */
    uint32_t nallocs, cap_allocs;
    FILE *out;                    /* $display output (fd 1) */
    FILE *files[TB_MAX_FILES];    /* $fopen descriptors 3.. */
} tb_module;

/* Parses text (file name for messages) and picks the top: name, or the first module when NULL.
 * ODIN3_ERR_INVALID_ARG (logged "file:line: unsupported: ...") for a parse failure. */
odin3_status tb_parse(const char *file, const char *text, const char *top, tb_module **out);
void tb_module_free(tb_module *tb);

/* --- run --- */
typedef struct tb_options {
    uint64_t max_time;
    uint64_t seed;
    FILE *out;
    const char *vcd; /* NULL: none */
} tb_options;

/* Runs the testbench against the modules of design. *finish_time gets the last time. */
odin3_status tb_run(odin3_design *design, tb_module *tb, const tb_options *opts,
                    uint64_t *finish_time);

#endif
