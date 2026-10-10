/*
 * kinds.h — the AST vocabulary (AST-6, AST-7): the 68 node kinds and their slot table, the
 * per-kind sub-kind enums and flag bits, the child classes, the store forms and the caps.
 */
#ifndef ODIN3_AST_KINDS_H
#define ODIN3_AST_KINDS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The slot table (spec §4.3, normative), one row per kind in table order. Columns:
 *   X(KIND, MEMBER, NAME, SUB, FLAGS, SLOTS, TAIL, PAYLOAD)
 * MEMBER: the classes of §4.2 the kind belongs to (E S D I U; DI = D and I, SI, IU; NO = none).
 * NAME: REQ / OPT / NO (must be 0). SUB: SUB(K) for the ODIN3_AST_SUBS_K list, or NOSUB.
 * FLAGS: F when the kind defines flags (ODIN3_AST_FM_<KIND>), else NOF. SLOTS: SLn(c, …), a slot
 * class per positional slot, suffix _OPT when optional (class X is spelled ANY). TAIL: TL(class,
 * min, max) (MANY = no bound but ODIN3_AST_MAX_CHILDREN), TLZ(class, KIND) (E*0: a 0 entry is
 * allowed when the node carries ODIN3_AST_F_<KIND>_SYSTEM), or NOTAIL. PAYLOAD: PNUM / PREAL /
 * PTEXT / NOPAY. Only src/ast/kinds.c interprets the columns; Phase 5 appends rows, never
 * renumbers.
 */
#define ODIN3_AST_KINDS(X)                                                                         \
    X(UNIT, NO, REQ, NOSUB, NOF, SL0, TL(U, 0, MANY), NOPAY)                                       \
    X(DIRECTIVE, IU, OPT, SUB(DIRECTIVE), NOF, SL0, NOTAIL, NOPAY)                                 \
    X(MODULE, U, REQ, NOSUB, F, SL2(L_OPT, L_OPT), TL(I, 0, MANY), NOPAY)                          \
    X(UDP_DECL, U, REQ, NOSUB, NOF, SL0, NOTAIL, PTEXT)                                            \
    X(CONFIG_DECL, U, REQ, NOSUB, NOF, SL0, NOTAIL, PTEXT)                                         \
    X(LIST, NO, NO, NOSUB, NOF, SL0, TL(ANY, 0, MANY), NOPAY)                                      \
    X(PORT_REF, NO, OPT, NOSUB, F, SL1(E_OPT), NOTAIL, NOPAY)                                      \
    X(PORT_DECL, I, NO, SUB(PORT_DECL), F, SL1(R_OPT), TL(DECLARATOR, 1, MANY), NOPAY)             \
    X(DECLARATOR, NO, REQ, NOSUB, NOF, SL1(E_OPT), TL(R, 0, MANY), NOPAY)                          \
    X(RANGE, NO, NO, NOSUB, NOF, SL2(E, E), NOTAIL, NOPAY)                                         \
    X(NET_DECL, DI, NO, SUB(NET_DECL), F, SL3(STRENGTH_OPT, R_OPT, DELAY_OPT),                     \
      TL(DECLARATOR, 1, MANY), NOPAY)                                                              \
    X(VAR_DECL, DI, NO, SUB(VAR_DECL), F, SL1(R_OPT), TL(DECLARATOR, 1, MANY), NOPAY)              \
    X(PARAM_DECL, DI, NO, SUB(PARAM_DECL), F, SL1(R_OPT), TL(DECLARATOR, 1, MANY), NOPAY)          \
    X(DEFPARAM, I, NO, NOSUB, NOF, SL2(HIER_NAME, E), NOTAIL, NOPAY)                               \
    X(GENVAR_DECL, DI, NO, NOSUB, NOF, SL0, TL(DECLARATOR, 1, MANY), NOPAY)                        \
    X(EVENT_DECL, DI, NO, NOSUB, NOF, SL0, TL(DECLARATOR, 1, MANY), NOPAY)                         \
    X(STRENGTH, NO, NO, SUB(STRENGTH), F, SL0, NOTAIL, NOPAY)                                      \
    X(DELAY, NO, NO, NOSUB, NOF, SL0, TL(E, 1, 3), NOPAY)                                          \
    X(CONT_ASSIGN, I, NO, NOSUB, NOF, SL2(STRENGTH_OPT, DELAY_OPT), TL(NET_ASSIGN, 1, MANY),       \
      NOPAY)                                                                                       \
    X(NET_ASSIGN, NO, NO, NOSUB, NOF, SL2(E, E), NOTAIL, NOPAY)                                    \
    X(ALWAYS, I, NO, SUB(ALWAYS), NOF, SL1(S), NOTAIL, NOPAY)                                      \
    X(INITIAL, I, NO, NOSUB, NOF, SL1(S), NOTAIL, NOPAY)                                           \
    X(INSTANTIATION, I, REQ, NOSUB, NOF, SL1(L_OPT), TL(INSTANCE, 1, MANY), NOPAY)                 \
    X(INSTANCE, NO, OPT, NOSUB, NOF, SL1(R_OPT), TL(C, 0, MANY), NOPAY)                            \
    X(CONNECTION, NO, OPT, NOSUB, NOF, SL1(E_OPT), NOTAIL, NOPAY)                                  \
    X(GATE_DECL, I, NO, SUB(GATE_DECL), NOF, SL2(STRENGTH_OPT, DELAY_OPT), TL(INSTANCE, 1, MANY),  \
      NOPAY)                                                                                       \
    X(FUNCTION_DECL, I, REQ, NOSUB, F, SL4(R_OPT, L, L, S), NOTAIL, NOPAY)                         \
    X(TASK_DECL, I, REQ, NOSUB, F, SL3(L, L, S), NOTAIL, NOPAY)                                    \
    X(SPECIFY_BLOCK, I, NO, NOSUB, NOF, SL0, NOTAIL, PTEXT)                                        \
    X(ATTR, NO, REQ, NOSUB, F, SL1(E_OPT), NOTAIL, NOPAY)                                          \
    X(GENERATE, I, NO, NOSUB, NOF, SL0, TL(I, 0, MANY), NOPAY)                                     \
    X(GEN_FOR, I, NO, NOSUB, NOF, SL4(BLOCKING_ASSIGN, E, BLOCKING_ASSIGN, I), NOTAIL, NOPAY)      \
    X(GEN_IF, I, NO, NOSUB, NOF, SL3(E, I, I_OPT), NOTAIL, NOPAY)                                  \
    X(GEN_CASE, I, NO, NOSUB, NOF, SL1(E), TL(CASE_ITEM, 1, MANY), NOPAY)                          \
    X(GEN_BLOCK, I, OPT, NOSUB, NOF, SL1(NUMBER_OPT), TL(I, 0, MANY), NOPAY)                       \
    X(SEQ_BLOCK, S, OPT, NOSUB, NOF, SL1(L_OPT), TL(S, 0, MANY), NOPAY)                            \
    X(PAR_BLOCK, S, OPT, NOSUB, NOF, SL1(L_OPT), TL(S, 0, MANY), NOPAY)                            \
    X(BLOCKING_ASSIGN, S, NO, NOSUB, NOF, SL3(E, TIMING_OPT, E), NOTAIL, NOPAY)                    \
    X(NONBLOCKING_ASSIGN, S, NO, NOSUB, NOF, SL3(E, TIMING_OPT, E), NOTAIL, NOPAY)                 \
    X(PROC_CONT_ASSIGN, S, NO, SUB(PROC_CONT_ASSIGN), NOF, SL2(E, E_OPT), NOTAIL, NOPAY)           \
    X(IF, S, NO, NOSUB, NOF, SL3(E, S, S_OPT), NOTAIL, NOPAY)                                      \
    X(CASE, S, NO, SUB(CASE), NOF, SL1(E), TL(CASE_ITEM, 1, MANY), NOPAY)                          \
    X(CASE_ITEM, NO, NO, NOSUB, F, SL1(BODY), TL(E, 0, MANY), NOPAY)                               \
    X(FOR, S, NO, NOSUB, NOF, SL4(BLOCKING_ASSIGN, E, BLOCKING_ASSIGN, S), NOTAIL, NOPAY)          \
    X(WHILE, S, NO, NOSUB, NOF, SL2(E, S), NOTAIL, NOPAY)                                          \
    X(REPEAT, S, NO, NOSUB, NOF, SL2(E, S), NOTAIL, NOPAY)                                         \
    X(FOREVER, S, NO, NOSUB, NOF, SL1(S), NOTAIL, NOPAY)                                           \
    X(TIMING_STMT, S, NO, NOSUB, NOF, SL2(TIMING, S_OPT), NOTAIL, NOPAY)                           \
    X(WAIT, S, NO, NOSUB, NOF, SL2(E, S_OPT), NOTAIL, NOPAY)                                       \
    X(DISABLE, S, NO, NOSUB, NOF, SL1(HIER_NAME), NOTAIL, NOPAY)                                   \
    X(EVENT_TRIGGER, S, NO, NOSUB, NOF, SL1(HIER_NAME), NOTAIL, NOPAY)                             \
    X(TASK_CALL, S, REQ, NOSUB, F, SL0, TLZ(E, TASK_CALL), NOPAY)                                  \
    X(NULL_STMT, SI, NO, NOSUB, NOF, SL0, NOTAIL, NOPAY)                                           \
    X(EVENT_CONTROL, NO, NO, NOSUB, F, SL1(E_OPT), TL(EVENT_EXPR, 0, MANY), NOPAY)                 \
    X(EVENT_EXPR, NO, NO, SUB(EVENT_EXPR), F, SL1(E), NOTAIL, NOPAY)                               \
    X(NUMBER, E, NO, SUB(NUMBER), F, SL0, NOTAIL, PNUM)                                            \
    X(REAL, E, NO, NOSUB, NOF, SL0, NOTAIL, PREAL)                                                 \
    X(STRING, E, REQ, NOSUB, NOF, SL0, NOTAIL, NOPAY)                                              \
    X(IDENT, E, REQ, NOSUB, NOF, SL0, NOTAIL, NOPAY)                                               \
    X(HIER_NAME, E, NO, NOSUB, NOF, SL0, TL(HIER_PART, 1, MANY), NOPAY)                            \
    X(SELECT, E, NO, SUB(SELECT), NOF, SL3(E, E, E_OPT), NOTAIL, NOPAY)                            \
    X(CONCAT, E, NO, NOSUB, NOF, SL0, TL(E, 1, MANY), NOPAY)                                       \
    X(REPLICATE, E, NO, NOSUB, NOF, SL2(E, CONCAT), NOTAIL, NOPAY)                                 \
    X(UNARY, E, NO, SUB(UNARY), NOF, SL1(E), NOTAIL, NOPAY)                                        \
    X(BINARY, E, NO, SUB(BINARY), NOF, SL2(E, E), NOTAIL, NOPAY)                                   \
    X(TERNARY, E, NO, NOSUB, NOF, SL3(E, E, E), NOTAIL, NOPAY)                                     \
    X(CALL, E, REQ, NOSUB, F, SL0, TLZ(E, CALL), NOPAY)                                            \
    X(MINTYPMAX, E, NO, NOSUB, NOF, SL3(E, E, E), NOTAIL, NOPAY)

#define ODIN3_AST_KIND_ENUM_(KIND, MEMBER, NAME, SUB, FLAGS, SLOTS, TAIL, PAYLOAD) ODIN3_AST_##KIND,

/* Node kinds: NONE = 0, UNIT = 1 … MINTYPMAX = 68. */
typedef enum odin3_ast_kind {
    ODIN3_AST_NONE = 0,
    ODIN3_AST_KINDS(ODIN3_AST_KIND_ENUM_) ODIN3_AST_KIND_COUNT /* one past the last kind */
} odin3_ast_kind;

/* The two subsets of the one format (AST-9); fixed when a store is created. */
typedef enum odin3_ast_form {
    ODIN3_AST_FORM_PARSED = 1,
    ODIN3_AST_FORM_ELABORATED = 2
} odin3_ast_form;

/* --- sub-kinds: one list per kind that has them, Y(KIND, NAME, "spelling") in order ---------- */

#define ODIN3_AST_SUBS_DIRECTIVE(Y)                                                                \
    Y(DIRECTIVE, DEFAULT_NETTYPE, "default_nettype")                                               \
    Y(DIRECTIVE, TIMESCALE, "timescale")                                                           \
    Y(DIRECTIVE, CELLDEFINE, "celldefine")                                                         \
    Y(DIRECTIVE, ENDCELLDEFINE, "endcelldefine")                                                   \
    Y(DIRECTIVE, RESETALL, "resetall")                                                             \
    Y(DIRECTIVE, BEGIN_KEYWORDS, "begin_keywords")                                                 \
    Y(DIRECTIVE, END_KEYWORDS, "end_keywords")                                                     \
    Y(DIRECTIVE, UNCONNECTED_DRIVE, "unconnected_drive")                                           \
    Y(DIRECTIVE, NOUNCONNECTED_DRIVE, "nounconnected_drive")                                       \
    Y(DIRECTIVE, PRAGMA, "pragma")

#define ODIN3_AST_SUBS_PORT_DECL(Y)                                                                \
    Y(PORT_DECL, INPUT, "input") Y(PORT_DECL, OUTPUT, "output") Y(PORT_DECL, INOUT, "inout")

#define ODIN3_AST_SUBS_NET_DECL(Y)                                                                 \
    Y(NET_DECL, WIRE, "wire")                                                                      \
    Y(NET_DECL, TRI, "tri")                                                                        \
    Y(NET_DECL, WAND, "wand")                                                                      \
    Y(NET_DECL, WOR, "wor")                                                                        \
    Y(NET_DECL, TRIAND, "triand")                                                                  \
    Y(NET_DECL, TRIOR, "trior")                                                                    \
    Y(NET_DECL, TRI0, "tri0")                                                                      \
    Y(NET_DECL, TRI1, "tri1")                                                                      \
    Y(NET_DECL, TRIREG, "trireg")                                                                  \
    Y(NET_DECL, UWIRE, "uwire")                                                                    \
    Y(NET_DECL, SUPPLY0, "supply0")                                                                \
    Y(NET_DECL, SUPPLY1, "supply1")

#define ODIN3_AST_SUBS_VAR_DECL(Y)                                                                 \
    Y(VAR_DECL, REG, "reg")                                                                        \
    Y(VAR_DECL, INTEGER, "integer")                                                                \
    Y(VAR_DECL, TIME, "time")                                                                      \
    Y(VAR_DECL, REAL, "real")                                                                      \
    Y(VAR_DECL, REALTIME, "realtime")

#define ODIN3_AST_SUBS_PARAM_DECL(Y)                                                               \
    Y(PARAM_DECL, PARAMETER, "parameter")                                                          \
    Y(PARAM_DECL, LOCALPARAM, "localparam")                                                        \
    Y(PARAM_DECL, SPECPARAM, "specparam")

#define ODIN3_AST_SUBS_STRENGTH(Y) Y(STRENGTH, DRIVE, "drive") Y(STRENGTH, CHARGE, "charge")

#define ODIN3_AST_SUBS_ALWAYS(Y)                                                                   \
    Y(ALWAYS, ALWAYS, "always")                                                                    \
    Y(ALWAYS, ALWAYS_FF, "always_ff")                                                              \
    Y(ALWAYS, ALWAYS_COMB, "always_comb")                                                          \
    Y(ALWAYS, ALWAYS_LATCH, "always_latch")

#define ODIN3_AST_SUBS_GATE_DECL(Y)                                                                \
    Y(GATE_DECL, AND, "and")                                                                       \
    Y(GATE_DECL, NAND, "nand")                                                                     \
    Y(GATE_DECL, OR, "or")                                                                         \
    Y(GATE_DECL, NOR, "nor")                                                                       \
    Y(GATE_DECL, XOR, "xor")                                                                       \
    Y(GATE_DECL, XNOR, "xnor")                                                                     \
    Y(GATE_DECL, BUF, "buf")                                                                       \
    Y(GATE_DECL, NOT, "not")                                                                       \
    Y(GATE_DECL, BUFIF0, "bufif0")                                                                 \
    Y(GATE_DECL, BUFIF1, "bufif1")                                                                 \
    Y(GATE_DECL, NOTIF0, "notif0")                                                                 \
    Y(GATE_DECL, NOTIF1, "notif1")                                                                 \
    Y(GATE_DECL, NMOS, "nmos")                                                                     \
    Y(GATE_DECL, PMOS, "pmos")                                                                     \
    Y(GATE_DECL, CMOS, "cmos")                                                                     \
    Y(GATE_DECL, RNMOS, "rnmos")                                                                   \
    Y(GATE_DECL, RPMOS, "rpmos")                                                                   \
    Y(GATE_DECL, RCMOS, "rcmos")                                                                   \
    Y(GATE_DECL, TRAN, "tran")                                                                     \
    Y(GATE_DECL, RTRAN, "rtran")                                                                   \
    Y(GATE_DECL, TRANIF0, "tranif0")                                                               \
    Y(GATE_DECL, TRANIF1, "tranif1")                                                               \
    Y(GATE_DECL, RTRANIF0, "rtranif0")                                                             \
    Y(GATE_DECL, RTRANIF1, "rtranif1")                                                             \
    Y(GATE_DECL, PULLUP, "pullup")                                                                 \
    Y(GATE_DECL, PULLDOWN, "pulldown")

#define ODIN3_AST_SUBS_PROC_CONT_ASSIGN(Y)                                                         \
    Y(PROC_CONT_ASSIGN, ASSIGN, "assign")                                                          \
    Y(PROC_CONT_ASSIGN, DEASSIGN, "deassign")                                                      \
    Y(PROC_CONT_ASSIGN, FORCE, "force")                                                            \
    Y(PROC_CONT_ASSIGN, RELEASE, "release")

#define ODIN3_AST_SUBS_CASE(Y) Y(CASE, CASE, "case") Y(CASE, CASEZ, "casez") Y(CASE, CASEX, "casex")

#define ODIN3_AST_SUBS_EVENT_EXPR(Y)                                                               \
    Y(EVENT_EXPR, NONE, "none") Y(EVENT_EXPR, POSEDGE, "posedge") Y(EVENT_EXPR, NEGEDGE, "negedge")

#define ODIN3_AST_SUBS_NUMBER(Y)                                                                   \
    Y(NUMBER, DEC, "dec") Y(NUMBER, BIN, "bin") Y(NUMBER, OCT, "oct") Y(NUMBER, HEX, "hex")

#define ODIN3_AST_SUBS_SELECT(Y)                                                                   \
    Y(SELECT, BIT, "bit")                                                                          \
    Y(SELECT, PART, "part")                                                                        \
    Y(SELECT, PART_PLUS, "part_plus")                                                              \
    Y(SELECT, PART_MINUS, "part_minus")

/* `^~` is stored as `~^`. */
#define ODIN3_AST_SUBS_UNARY(Y)                                                                    \
    Y(UNARY, PLUS, "+")                                                                            \
    Y(UNARY, MINUS, "-")                                                                           \
    Y(UNARY, LNOT, "!")                                                                            \
    Y(UNARY, NOT, "~")                                                                             \
    Y(UNARY, AND, "&")                                                                             \
    Y(UNARY, NAND, "~&")                                                                           \
    Y(UNARY, OR, "|")                                                                              \
    Y(UNARY, NOR, "~|")                                                                            \
    Y(UNARY, XOR, "^")                                                                             \
    Y(UNARY, XNOR, "~^")

#define ODIN3_AST_SUBS_BINARY(Y)                                                                   \
    Y(BINARY, ADD, "+")                                                                            \
    Y(BINARY, SUB, "-")                                                                            \
    Y(BINARY, MUL, "*")                                                                            \
    Y(BINARY, DIV, "/")                                                                            \
    Y(BINARY, MOD, "%")                                                                            \
    Y(BINARY, POW, "**")                                                                           \
    Y(BINARY, EQ, "==")                                                                            \
    Y(BINARY, NE, "!=")                                                                            \
    Y(BINARY, CASE_EQ, "===")                                                                      \
    Y(BINARY, CASE_NE, "!==")                                                                      \
    Y(BINARY, LAND, "&&")                                                                          \
    Y(BINARY, LOR, "||")                                                                           \
    Y(BINARY, LT, "<")                                                                             \
    Y(BINARY, LE, "<=")                                                                            \
    Y(BINARY, GT, ">")                                                                             \
    Y(BINARY, GE, ">=")                                                                            \
    Y(BINARY, AND, "&")                                                                            \
    Y(BINARY, OR, "|")                                                                             \
    Y(BINARY, XOR, "^")                                                                            \
    Y(BINARY, XNOR, "~^")                                                                          \
    Y(BINARY, SHL, "<<")                                                                           \
    Y(BINARY, SHR, ">>")                                                                           \
    Y(BINARY, ASHL, "<<<")                                                                         \
    Y(BINARY, ASHR, ">>>")

#define ODIN3_AST_SUB_ENUM_(KIND, NAME, TEXT) ODIN3_AST_##KIND##_##NAME,

/* Each enum ends with ODIN3_AST_<KIND>_COUNT, the kind's sub-kind count. */
typedef enum odin3_ast_directive_kind {
    ODIN3_AST_SUBS_DIRECTIVE(ODIN3_AST_SUB_ENUM_) ODIN3_AST_DIRECTIVE_COUNT
} odin3_ast_directive_kind;
typedef enum odin3_ast_direction {
    ODIN3_AST_SUBS_PORT_DECL(ODIN3_AST_SUB_ENUM_) ODIN3_AST_PORT_DECL_COUNT
} odin3_ast_direction;
typedef enum odin3_ast_net_kind {
    ODIN3_AST_SUBS_NET_DECL(ODIN3_AST_SUB_ENUM_) ODIN3_AST_NET_DECL_COUNT
} odin3_ast_net_kind;
typedef enum odin3_ast_var_kind {
    ODIN3_AST_SUBS_VAR_DECL(ODIN3_AST_SUB_ENUM_) ODIN3_AST_VAR_DECL_COUNT
} odin3_ast_var_kind;
typedef enum odin3_ast_param_kind {
    ODIN3_AST_SUBS_PARAM_DECL(ODIN3_AST_SUB_ENUM_) ODIN3_AST_PARAM_DECL_COUNT
} odin3_ast_param_kind;
typedef enum odin3_ast_strength_kind {
    ODIN3_AST_SUBS_STRENGTH(ODIN3_AST_SUB_ENUM_) ODIN3_AST_STRENGTH_COUNT
} odin3_ast_strength_kind;
typedef enum odin3_ast_always_kind {
    ODIN3_AST_SUBS_ALWAYS(ODIN3_AST_SUB_ENUM_) ODIN3_AST_ALWAYS_COUNT
} odin3_ast_always_kind;
typedef enum odin3_ast_gate_kind {
    ODIN3_AST_SUBS_GATE_DECL(ODIN3_AST_SUB_ENUM_) ODIN3_AST_GATE_DECL_COUNT
} odin3_ast_gate_kind;
typedef enum odin3_ast_proc_assign_kind {
    ODIN3_AST_SUBS_PROC_CONT_ASSIGN(ODIN3_AST_SUB_ENUM_) ODIN3_AST_PROC_CONT_ASSIGN_COUNT
} odin3_ast_proc_assign_kind;
typedef enum odin3_ast_case_kind {
    ODIN3_AST_SUBS_CASE(ODIN3_AST_SUB_ENUM_) ODIN3_AST_CASE_COUNT
} odin3_ast_case_kind;
typedef enum odin3_ast_edge {
    ODIN3_AST_SUBS_EVENT_EXPR(ODIN3_AST_SUB_ENUM_) ODIN3_AST_EVENT_EXPR_COUNT
} odin3_ast_edge;
typedef enum odin3_ast_base {
    ODIN3_AST_SUBS_NUMBER(ODIN3_AST_SUB_ENUM_) ODIN3_AST_NUMBER_COUNT
} odin3_ast_base;
typedef enum odin3_ast_select_kind {
    ODIN3_AST_SUBS_SELECT(ODIN3_AST_SUB_ENUM_) ODIN3_AST_SELECT_COUNT
} odin3_ast_select_kind;
typedef enum odin3_ast_unary_op {
    ODIN3_AST_SUBS_UNARY(ODIN3_AST_SUB_ENUM_) ODIN3_AST_UNARY_COUNT
} odin3_ast_unary_op;
typedef enum odin3_ast_binary_op {
    ODIN3_AST_SUBS_BINARY(ODIN3_AST_SUB_ENUM_) ODIN3_AST_BINARY_COUNT
} odin3_ast_binary_op;

/* --- flags (AST-7): per kind, bits from 0 in row order; fields as _SHIFT/_MASK --------------- */

enum {
    ODIN3_AST_F_MODULE_MACROMODULE = 1U << 0,

    ODIN3_AST_F_PORT_REF_DOTTED = 1U << 0,

    ODIN3_AST_F_PORT_DECL_SIGNED = 1U << 0,
    ODIN3_AST_F_PORT_DECL_IN_HEADER = 1U << 1,
    ODIN3_AST_F_PORT_DECL_DT_SHIFT = 2,
    ODIN3_AST_F_PORT_DECL_DT_MASK = 0x7U << 2,
    ODIN3_AST_F_PORT_DECL_NET_SHIFT = 5, /* 0 = unspecified, else 1 + odin3_ast_net_kind */
    ODIN3_AST_F_PORT_DECL_NET_MASK = 0xFU << 5,

    ODIN3_AST_F_NET_DECL_SIGNED = 1U << 0,
    ODIN3_AST_F_NET_DECL_VECTORED = 1U << 1,
    ODIN3_AST_F_NET_DECL_SCALARED = 1U << 2,

    ODIN3_AST_F_VAR_DECL_SIGNED = 1U << 0,

    ODIN3_AST_F_PARAM_DECL_SIGNED = 1U << 0,
    ODIN3_AST_F_PARAM_DECL_IN_HEADER = 1U << 1,
    ODIN3_AST_F_PARAM_DECL_DT_SHIFT = 2,
    ODIN3_AST_F_PARAM_DECL_DT_MASK = 0x7U << 2,

    ODIN3_AST_F_STRENGTH_S0_SHIFT = 0, /* odin3_ast_drive_value or odin3_ast_charge_value */
    ODIN3_AST_F_STRENGTH_S0_MASK = 0xFU << 0,
    ODIN3_AST_F_STRENGTH_S1_SHIFT = 4,
    ODIN3_AST_F_STRENGTH_S1_MASK = 0xFU << 4,

    ODIN3_AST_F_FUNCTION_DECL_AUTOMATIC = 1U << 0,
    ODIN3_AST_F_FUNCTION_DECL_SIGNED = 1U << 1,
    ODIN3_AST_F_FUNCTION_DECL_DT_SHIFT = 2,
    ODIN3_AST_F_FUNCTION_DECL_DT_MASK = 0x7U << 2,

    ODIN3_AST_F_TASK_DECL_AUTOMATIC = 1U << 0,

    ODIN3_AST_F_ATTR_METACOMMENT = 1U << 0,

    ODIN3_AST_F_TASK_CALL_SYSTEM = 1U << 0,
    ODIN3_AST_F_TASK_CALL_HIER = 1U << 1,

    ODIN3_AST_F_CASE_ITEM_DEFAULT = 1U << 0,

    ODIN3_AST_F_EVENT_CONTROL_STAR = 1U << 0,
    ODIN3_AST_F_EVENT_CONTROL_NO_PARENS = 1U << 1,

    ODIN3_AST_F_EVENT_EXPR_AFTER_OR = 1U << 0,

    ODIN3_AST_F_NUMBER_SIGNED = 1U << 0,

    ODIN3_AST_F_CALL_SYSTEM = 1U << 0,
    ODIN3_AST_F_CALL_HIER = 1U << 1,
};

/* The defined-flag mask of each kind with flags (the F column); check rejects other bits. */
enum {
    ODIN3_AST_FM_MODULE = ODIN3_AST_F_MODULE_MACROMODULE,
    ODIN3_AST_FM_PORT_REF = ODIN3_AST_F_PORT_REF_DOTTED,
    ODIN3_AST_FM_PORT_DECL = ODIN3_AST_F_PORT_DECL_SIGNED | ODIN3_AST_F_PORT_DECL_IN_HEADER |
                             ODIN3_AST_F_PORT_DECL_DT_MASK | ODIN3_AST_F_PORT_DECL_NET_MASK,
    ODIN3_AST_FM_NET_DECL =
        ODIN3_AST_F_NET_DECL_SIGNED | ODIN3_AST_F_NET_DECL_VECTORED | ODIN3_AST_F_NET_DECL_SCALARED,
    ODIN3_AST_FM_VAR_DECL = ODIN3_AST_F_VAR_DECL_SIGNED,
    ODIN3_AST_FM_PARAM_DECL = ODIN3_AST_F_PARAM_DECL_SIGNED | ODIN3_AST_F_PARAM_DECL_IN_HEADER |
                              ODIN3_AST_F_PARAM_DECL_DT_MASK,
    ODIN3_AST_FM_STRENGTH = ODIN3_AST_F_STRENGTH_S0_MASK | ODIN3_AST_F_STRENGTH_S1_MASK,
    ODIN3_AST_FM_FUNCTION_DECL = ODIN3_AST_F_FUNCTION_DECL_AUTOMATIC |
                                 ODIN3_AST_F_FUNCTION_DECL_SIGNED |
                                 ODIN3_AST_F_FUNCTION_DECL_DT_MASK,
    ODIN3_AST_FM_TASK_DECL = ODIN3_AST_F_TASK_DECL_AUTOMATIC,
    ODIN3_AST_FM_ATTR = ODIN3_AST_F_ATTR_METACOMMENT,
    ODIN3_AST_FM_TASK_CALL = ODIN3_AST_F_TASK_CALL_SYSTEM | ODIN3_AST_F_TASK_CALL_HIER,
    ODIN3_AST_FM_CASE_ITEM = ODIN3_AST_F_CASE_ITEM_DEFAULT,
    ODIN3_AST_FM_EVENT_CONTROL =
        ODIN3_AST_F_EVENT_CONTROL_STAR | ODIN3_AST_F_EVENT_CONTROL_NO_PARENS,
    ODIN3_AST_FM_EVENT_EXPR = ODIN3_AST_F_EVENT_EXPR_AFTER_OR,
    ODIN3_AST_FM_NUMBER = ODIN3_AST_F_NUMBER_SIGNED,
    ODIN3_AST_FM_CALL = ODIN3_AST_F_CALL_SYSTEM | ODIN3_AST_F_CALL_HIER,
};

/* Values of the DT field (PORT_DECL, PARAM_DECL, FUNCTION_DECL). */
typedef enum odin3_ast_dt {
    ODIN3_AST_DT_NONE = 0,
    ODIN3_AST_DT_REG,
    ODIN3_AST_DT_INTEGER,
    ODIN3_AST_DT_REAL,
    ODIN3_AST_DT_REALTIME,
    ODIN3_AST_DT_TIME
} odin3_ast_dt;

/* Values of STRENGTH's S0/S1 fields: drive strengths (sub DRIVE), charge (sub CHARGE, in S0). */
typedef enum odin3_ast_drive_value {
    ODIN3_AST_DRIVE_NONE = 0,
    ODIN3_AST_DRIVE_HIGHZ,
    ODIN3_AST_DRIVE_WEAK,
    ODIN3_AST_DRIVE_PULL,
    ODIN3_AST_DRIVE_STRONG,
    ODIN3_AST_DRIVE_SUPPLY
} odin3_ast_drive_value;
typedef enum odin3_ast_charge_value {
    ODIN3_AST_CHARGE_NONE = 0,
    ODIN3_AST_CHARGE_SMALL,
    ODIN3_AST_CHARGE_MEDIUM,
    ODIN3_AST_CHARGE_LARGE
} odin3_ast_charge_value;

/* --- caps (spec §10); each a located ODIN3_ERR_PARSE except the ID-space ones ---------------- */

#define ODIN3_AST_MAX_DEPTH UINT32_C(32768)            /* node height, builder-enforced */
#define ODIN3_AST_MAX_NUMBER_BITS (UINT32_C(1) << 20)  /* = ODIN3_READER_MAX_WIDTH */
#define ODIN3_AST_MAX_DECIMAL_DIGITS UINT32_C(4096)    /* digits of a decimal literal */
#define ODIN3_AST_MAX_STRING_BYTES (UINT32_C(1) << 17) /* lexer-checked */
#define ODIN3_AST_MAX_IDENT_BYTES UINT32_C(4096)       /* lexer-checked */
#define ODIN3_AST_MAX_TEXT_BYTES (UINT32_C(1) << 20)   /* an opaque TEXT payload */
#define ODIN3_AST_MAX_CHILDREN (UINT32_C(1) << 24)     /* children of one node */
#define ODIN3_AST_MAX_NODES (UINT32_MAX - 1)           /* nodes; then NO_MEMORY */
#define ODIN3_AST_MAX_CHILD_TABLE (UINT32_MAX - 1)     /* child-table entries; then NO_MEMORY */

/* --- the generated slot table ------------------------------------------------------------- */

/* Class membership bits (§4.2); R, L and C are the kinds RANGE, LIST and CONNECTION. */
enum {
    ODIN3_AST_MEMBER_E = 1U << 0,
    ODIN3_AST_MEMBER_S = 1U << 1,
    ODIN3_AST_MEMBER_D = 1U << 2,
    ODIN3_AST_MEMBER_I = 1U << 3,
    ODIN3_AST_MEMBER_U = 1U << 4,
};

/* Child classes of slots and tails: a class of §4.2, an exact kind or a pair of kinds. */
typedef enum odin3_ast_class {
    ODIN3_AST_CLS_NONE = 0, /* no slot / no tail */
    ODIN3_AST_CLS_E,
    ODIN3_AST_CLS_S,
    ODIN3_AST_CLS_D,
    ODIN3_AST_CLS_I,
    ODIN3_AST_CLS_U,
    ODIN3_AST_CLS_R,
    ODIN3_AST_CLS_L,
    ODIN3_AST_CLS_C,
    ODIN3_AST_CLS_X,         /* any kind */
    ODIN3_AST_CLS_BODY,      /* CASE_ITEM body: S, or I under GEN_CASE (check rule 2) */
    ODIN3_AST_CLS_TIMING,    /* DELAY or EVENT_CONTROL */
    ODIN3_AST_CLS_HIER_PART, /* IDENT or SELECT (bit over IDENT) */
    ODIN3_AST_CLS_BLOCKING_ASSIGN,
    ODIN3_AST_CLS_HIER_NAME,
    ODIN3_AST_CLS_STRENGTH,
    ODIN3_AST_CLS_DELAY,
    ODIN3_AST_CLS_CONCAT,
    ODIN3_AST_CLS_NUMBER,
    ODIN3_AST_CLS_CASE_ITEM,
    ODIN3_AST_CLS_DECLARATOR,
    ODIN3_AST_CLS_INSTANCE,
    ODIN3_AST_CLS_NET_ASSIGN,
    ODIN3_AST_CLS_EVENT_EXPR,
    ODIN3_AST_CLS_COUNT
} odin3_ast_class;

/* A slot entry is a class, ORed with ODIN3_AST_SLOT_OPT when the slot may be 0. */
enum { ODIN3_AST_SLOT_OPT = 0x80U, ODIN3_AST_SLOT_CLASS = 0x7FU, ODIN3_AST_MAX_SLOTS = 4 };

typedef enum odin3_ast_name_rule {
    ODIN3_AST_NAME_NO = 0, /* must be 0 */
    ODIN3_AST_NAME_REQ,    /* non-zero */
    ODIN3_AST_NAME_OPT     /* either */
} odin3_ast_name_rule;

typedef enum odin3_ast_payload_kind {
    ODIN3_AST_PAYLOAD_NONE = 0,
    ODIN3_AST_PAYLOAD_NUMBER,
    ODIN3_AST_PAYLOAD_REAL,
    ODIN3_AST_PAYLOAD_TEXT
} odin3_ast_payload_kind;

/* One row of the slot table. */
typedef struct odin3_ast_kind_info {
    const char *name;                   /* "UNIT" … */
    const char *const *sub_names;       /* sub_count spellings, NULL when the kind has none */
    uint32_t sub_count;                 /* 0: sub must be 0 */
    uint32_t tail_min, tail_max;        /* tail count bounds (tail_max UINT32_MAX = unbounded) */
    uint16_t flags;                     /* defined-flag mask */
    uint16_t tail_zero;                 /* flag bits allowing a 0 tail entry (E*0), 0 = never */
    uint8_t kind;                       /* the row's own kind */
    uint8_t member;                     /* ODIN3_AST_MEMBER_* bits */
    uint8_t name_rule;                  /* odin3_ast_name_rule */
    uint8_t payload;                    /* odin3_ast_payload_kind */
    uint8_t nslots;                     /* positional slots */
    uint8_t slots[ODIN3_AST_MAX_SLOTS]; /* class | ODIN3_AST_SLOT_OPT per slot */
    uint8_t tail;                       /* class of tail entries, ODIN3_AST_CLS_NONE = no tail */
} odin3_ast_kind_info;

/* The row of kind, NULL for NONE or an unknown kind. */
const odin3_ast_kind_info *odin3_ast_kind_info_of(uint32_t kind);

/*
 * True when a node of the row's kind belongs to cls; false for a NULL row (kind 0 or unknown):
 * odin3_ast_class_has(cls, odin3_ast_kind_info_of(kind)).
 */
bool odin3_ast_class_has(odin3_ast_class cls, const odin3_ast_kind_info *info);

/* "UNIT" … "MINTYPMAX"; "NONE" for 0 and "" for an unknown kind. Never NULL. */
const char *odin3_ast_kind_name(uint32_t kind);

/* Positional slot count of kind; 0 for an unknown kind. */
uint32_t odin3_ast_slot_count(uint32_t kind);

/* True when kind has a tail; false for an unknown kind. */
bool odin3_ast_has_tail(uint32_t kind);

#endif
