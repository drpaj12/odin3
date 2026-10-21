/*
 * dffx.h — the list of the 68 gate-level flip-flops with a reset or an enable (5.3 follow-up; Yosys
 * 0.55 techlibs/common/simcells.v). Included by dffx.c (definitions), cells.h (declarations) and
 * builtin.c (the table). Names are `$_<family>_<clock><reset polarity><reset value><enable>_`:
 * each letter group is P (positive, active high) or N (negative), the value 0 or 1.
 *
 *   A   $_DFF_<c><r><v>_      async reset to v  C R D Q    8
 *   E   $_DFFE_<c><e>_        enable            C E D Q    4
 *   AE  $_DFFE_<c><r><v><e>_  async reset+enable C R E D Q 16
 *   S   $_SDFF_<c><r><v>_     sync reset        C R D Q    8
 *   SE  $_SDFFE_<c><r><v><e>_ sync reset over the enable C R E D Q 16
 *   SCE $_SDFFCE_<c><r><v><e>_ enable over the sync reset C R E D Q 16
 *
 * The list macro takes six macro names, one per family, and calls each with its letters.
 */
#ifndef ODIN3_IR_CELLS_DFFX_H
#define ODIN3_IR_CELLS_DFFX_H

#define ODIN3_DFFX_RV(M)                                                                           \
    M(P, P, 0) M(P, P, 1) M(P, N, 0) M(P, N, 1) M(N, P, 0) M(N, P, 1) M(N, N, 0) M(N, N, 1)
#define ODIN3_DFFX_CE(M) M(P, P) M(P, N) M(N, P) M(N, N)
#define ODIN3_DFFX_RVE(M)                                                                          \
    M(P, P, 0, P)                                                                                  \
    M(P, P, 0, N)                                                                                  \
    M(P, P, 1, P)                                                                                  \
    M(P, P, 1, N)                                                                                  \
    M(P, N, 0, P)                                                                                  \
    M(P, N, 0, N)                                                                                  \
    M(P, N, 1, P)                                                                                  \
    M(P, N, 1, N)                                                                                  \
    M(N, P, 0, P)                                                                                  \
    M(N, P, 0, N)                                                                                  \
    M(N, P, 1, P)                                                                                  \
    M(N, P, 1, N)                                                                                  \
    M(N, N, 0, P)                                                                                  \
    M(N, N, 0, N)                                                                                  \
    M(N, N, 1, P)                                                                                  \
    M(N, N, 1, N)

#define ODIN3_DFFX_LIST(A, E, AE, S, SE, SCE)                                                      \
    ODIN3_DFFX_RV(A)                                                                               \
    ODIN3_DFFX_CE(E) ODIN3_DFFX_RVE(AE) ODIN3_DFFX_RV(S) ODIN3_DFFX_RVE(SE) ODIN3_DFFX_RVE(SCE)

/* The number of types in the list: 8 + 4 + 16 + 8 + 16 + 16. */
#define ODIN3_DFFX_COUNT 68

#endif
