/* kinds.c — the generated slot table (AST-6) and the class predicates of §4.2. */
#include "ast/kinds.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- sub-kind spellings -------------------------------------------------------------------- */

#define SUB_TEXT(KIND, NAME, TEXT) TEXT,

static const char *const names_DIRECTIVE[] = {ODIN3_AST_SUBS_DIRECTIVE(SUB_TEXT)};
static const char *const names_PORT_DECL[] = {ODIN3_AST_SUBS_PORT_DECL(SUB_TEXT)};
static const char *const names_NET_DECL[] = {ODIN3_AST_SUBS_NET_DECL(SUB_TEXT)};
static const char *const names_VAR_DECL[] = {ODIN3_AST_SUBS_VAR_DECL(SUB_TEXT)};
static const char *const names_PARAM_DECL[] = {ODIN3_AST_SUBS_PARAM_DECL(SUB_TEXT)};
static const char *const names_STRENGTH[] = {ODIN3_AST_SUBS_STRENGTH(SUB_TEXT)};
static const char *const names_ALWAYS[] = {ODIN3_AST_SUBS_ALWAYS(SUB_TEXT)};
static const char *const names_GATE_DECL[] = {ODIN3_AST_SUBS_GATE_DECL(SUB_TEXT)};
static const char *const names_PROC_CONT_ASSIGN[] = {ODIN3_AST_SUBS_PROC_CONT_ASSIGN(SUB_TEXT)};
static const char *const names_CASE[] = {ODIN3_AST_SUBS_CASE(SUB_TEXT)};
static const char *const names_EVENT_EXPR[] = {ODIN3_AST_SUBS_EVENT_EXPR(SUB_TEXT)};
static const char *const names_NUMBER[] = {ODIN3_AST_SUBS_NUMBER(SUB_TEXT)};
static const char *const names_SELECT[] = {ODIN3_AST_SUBS_SELECT(SUB_TEXT)};
static const char *const names_UNARY[] = {ODIN3_AST_SUBS_UNARY(SUB_TEXT)};
static const char *const names_BINARY[] = {ODIN3_AST_SUBS_BINARY(SUB_TEXT)};

/* --- column vocabulary of ODIN3_AST_KINDS (kinds.h) ---------------------------------------- */

enum {
    MEMBER_NO = 0,
    MEMBER_E = ODIN3_AST_MEMBER_E,
    MEMBER_S = ODIN3_AST_MEMBER_S,
    MEMBER_I = ODIN3_AST_MEMBER_I,
    MEMBER_U = ODIN3_AST_MEMBER_U,
    MEMBER_DI = ODIN3_AST_MEMBER_D | ODIN3_AST_MEMBER_I,
    MEMBER_SI = ODIN3_AST_MEMBER_S | ODIN3_AST_MEMBER_I,
    MEMBER_IU = ODIN3_AST_MEMBER_I | ODIN3_AST_MEMBER_U,
};

enum {
    NAME_NO = ODIN3_AST_NAME_NO,
    NAME_REQ = ODIN3_AST_NAME_REQ,
    NAME_OPT = ODIN3_AST_NAME_OPT,
    PAY_NOPAY = ODIN3_AST_PAYLOAD_NONE,
    PAY_PNUM = ODIN3_AST_PAYLOAD_NUMBER,
    PAY_PREAL = ODIN3_AST_PAYLOAD_REAL,
    PAY_PTEXT = ODIN3_AST_PAYLOAD_TEXT,
};

#define OPT_ ODIN3_AST_SLOT_OPT
enum {
    CL_E = ODIN3_AST_CLS_E,
    CL_E_OPT = ODIN3_AST_CLS_E | OPT_,
    CL_S = ODIN3_AST_CLS_S,
    CL_S_OPT = ODIN3_AST_CLS_S | OPT_,
    CL_I = ODIN3_AST_CLS_I,
    CL_I_OPT = ODIN3_AST_CLS_I | OPT_,
    CL_U = ODIN3_AST_CLS_U,
    CL_R = ODIN3_AST_CLS_R,
    CL_R_OPT = ODIN3_AST_CLS_R | OPT_,
    CL_L = ODIN3_AST_CLS_L,
    CL_L_OPT = ODIN3_AST_CLS_L | OPT_,
    CL_C = ODIN3_AST_CLS_C,
    CL_ANY = ODIN3_AST_CLS_X, /* X: the class token cannot be the macro parameter X */
    CL_BODY = ODIN3_AST_CLS_BODY,
    CL_TIMING = ODIN3_AST_CLS_TIMING,
    CL_TIMING_OPT = ODIN3_AST_CLS_TIMING | OPT_,
    CL_HIER_PART = ODIN3_AST_CLS_HIER_PART,
    CL_BLOCKING_ASSIGN = ODIN3_AST_CLS_BLOCKING_ASSIGN,
    CL_HIER_NAME = ODIN3_AST_CLS_HIER_NAME,
    CL_STRENGTH_OPT = ODIN3_AST_CLS_STRENGTH | OPT_,
    CL_DELAY_OPT = ODIN3_AST_CLS_DELAY | OPT_,
    CL_CONCAT = ODIN3_AST_CLS_CONCAT,
    CL_NUMBER_OPT = ODIN3_AST_CLS_NUMBER | OPT_,
    CL_CASE_ITEM = ODIN3_AST_CLS_CASE_ITEM,
    CL_DECLARATOR = ODIN3_AST_CLS_DECLARATOR,
    CL_INSTANCE = ODIN3_AST_CLS_INSTANCE,
    CL_NET_ASSIGN = ODIN3_AST_CLS_NET_ASSIGN,
    CL_EVENT_EXPR = ODIN3_AST_CLS_EVENT_EXPR,
};
#undef OPT_

#define MANY UINT32_MAX
#define SUB(KIND) .sub_names = names_##KIND, .sub_count = ODIN3_AST_##KIND##_COUNT
#define NOSUB .sub_names = NULL, .sub_count = 0
#define FLAGS_F(KIND) ODIN3_AST_FM_##KIND
#define FLAGS_NOF(KIND) 0
#define SL0 .nslots = 0
#define SL1(a) .nslots = 1, .slots = {CL_##a}
#define SL2(a, b) .nslots = 2, .slots = {CL_##a, CL_##b}
#define SL3(a, b, c) .nslots = 3, .slots = {CL_##a, CL_##b, CL_##c}
#define SL4(a, b, c, d) .nslots = 4, .slots = {CL_##a, CL_##b, CL_##c, CL_##d}
#define TL(cls, lo, hi) .tail = CL_##cls, .tail_min = (lo), .tail_max = (hi)
#define TLZ(cls, KIND)                                                                             \
    .tail = CL_##cls, .tail_min = 0, .tail_max = MANY, .tail_zero = ODIN3_AST_F_##KIND##_SYSTEM
#define NOTAIL .tail = ODIN3_AST_CLS_NONE

#define ROW(KIND, MEMBER, NAME, SUBS, FLAGS, SLOTS, TAIL, PAYLOAD)                                 \
    [ODIN3_AST_##KIND] = {.name = #KIND,                                                           \
                          .kind = ODIN3_AST_##KIND,                                                \
                          SUBS,                                                                    \
                          .flags = FLAGS_##FLAGS(KIND),                                            \
                          .member = MEMBER_##MEMBER,                                               \
                          .name_rule = NAME_##NAME,                                                \
                          .payload = PAY_##PAYLOAD,                                                \
                          SLOTS,                                                                   \
                          TAIL},

static const odin3_ast_kind_info kind_table[ODIN3_AST_KIND_COUNT] = {ODIN3_AST_KINDS(ROW)};

/* --- classes ------------------------------------------------------------------------------- */

/* A class matches the kinds whose membership meets mask, and up to two exact kinds. */
typedef struct class_rule {
    uint8_t mask;
    uint8_t kind_a, kind_b;
} class_rule;

static const class_rule class_table[ODIN3_AST_CLS_COUNT] = {
    [ODIN3_AST_CLS_E] = {ODIN3_AST_MEMBER_E, 0, 0},
    [ODIN3_AST_CLS_S] = {ODIN3_AST_MEMBER_S, 0, 0},
    [ODIN3_AST_CLS_D] = {ODIN3_AST_MEMBER_D, 0, 0},
    [ODIN3_AST_CLS_I] = {ODIN3_AST_MEMBER_I, 0, 0},
    [ODIN3_AST_CLS_U] = {ODIN3_AST_MEMBER_U, 0, 0},
    [ODIN3_AST_CLS_R] = {0, ODIN3_AST_RANGE, 0},
    [ODIN3_AST_CLS_L] = {0, ODIN3_AST_LIST, 0},
    [ODIN3_AST_CLS_C] = {0, ODIN3_AST_CONNECTION, 0},
    [ODIN3_AST_CLS_BODY] = {ODIN3_AST_MEMBER_S | ODIN3_AST_MEMBER_I, 0, 0},
    [ODIN3_AST_CLS_TIMING] = {0, ODIN3_AST_DELAY, ODIN3_AST_EVENT_CONTROL},
    [ODIN3_AST_CLS_HIER_PART] = {0, ODIN3_AST_IDENT, ODIN3_AST_SELECT},
    [ODIN3_AST_CLS_BLOCKING_ASSIGN] = {0, ODIN3_AST_BLOCKING_ASSIGN, 0},
    [ODIN3_AST_CLS_HIER_NAME] = {0, ODIN3_AST_HIER_NAME, 0},
    [ODIN3_AST_CLS_STRENGTH] = {0, ODIN3_AST_STRENGTH, 0},
    [ODIN3_AST_CLS_DELAY] = {0, ODIN3_AST_DELAY, 0},
    [ODIN3_AST_CLS_CONCAT] = {0, ODIN3_AST_CONCAT, 0},
    [ODIN3_AST_CLS_NUMBER] = {0, ODIN3_AST_NUMBER, 0},
    [ODIN3_AST_CLS_CASE_ITEM] = {0, ODIN3_AST_CASE_ITEM, 0},
    [ODIN3_AST_CLS_DECLARATOR] = {0, ODIN3_AST_DECLARATOR, 0},
    [ODIN3_AST_CLS_INSTANCE] = {0, ODIN3_AST_INSTANCE, 0},
    [ODIN3_AST_CLS_NET_ASSIGN] = {0, ODIN3_AST_NET_ASSIGN, 0},
    [ODIN3_AST_CLS_EVENT_EXPR] = {0, ODIN3_AST_EVENT_EXPR, 0},
};

const odin3_ast_kind_info *odin3_ast_kind_info_of(uint32_t kind) {
    return kind != ODIN3_AST_NONE && kind < ODIN3_AST_KIND_COUNT ? &kind_table[kind] : NULL;
}

bool odin3_ast_class_has(odin3_ast_class cls, const odin3_ast_kind_info *info) {
    if (info == NULL || (unsigned)cls >= ODIN3_AST_CLS_COUNT || cls == ODIN3_AST_CLS_NONE) {
        return false;
    }
    if (cls == ODIN3_AST_CLS_X) {
        return true;
    }
    const class_rule *rule = &class_table[cls];
    return (info->member & rule->mask) != 0 || info->kind == rule->kind_a ||
           info->kind == rule->kind_b;
}

const char *odin3_ast_kind_name(uint32_t kind) {
    const odin3_ast_kind_info *info = odin3_ast_kind_info_of(kind);
    if (info != NULL) {
        return info->name;
    }
    return kind == ODIN3_AST_NONE ? "NONE" : "";
}

uint32_t odin3_ast_slot_count(uint32_t kind) {
    const odin3_ast_kind_info *info = odin3_ast_kind_info_of(kind);
    return info != NULL ? info->nslots : 0;
}

bool odin3_ast_has_tail(uint32_t kind) {
    const odin3_ast_kind_info *info = odin3_ast_kind_info_of(kind);
    return info != NULL && info->tail != ODIN3_AST_CLS_NONE;
}
