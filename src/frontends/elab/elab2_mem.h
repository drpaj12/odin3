/*
 * elab2_mem.h — what elab2_mem.c (the decision, the ports, the $mem cell) and elab2_memscan.c
 * (Yosys's mem2reg_as_needed pass 1 over the module) share. Private to the two files.
 */
#ifndef ODIN3_FRONTENDS_ELAB_ELAB2_MEM_H
#define ODIN3_FRONTENDS_ELAB_ELAB2_MEM_H

#include "frontends/elab/elab_internal.h"

#include "ast/ast.h"
#include "ast/symtab.h"
#include "util/vec.h"

#include <stdint.h>

/* The flags of Yosys's MEM2REG_FL_*, and this file's own for a continuous write. */
enum {
    MEM_FL_EQ2 = 1U << 1,       /* read after a blocking write in the same block */
    MEM_FL_SET_ASYNC = 1U << 2, /* written in a block without exactly one edge event */
    MEM_FL_SET_INIT = 1U << 3,  /* written in an initial block */
    MEM_FL_SET_ELSE = 1U << 4,  /* written anywhere else */
    MEM_FL_CMPLX_LHS = 1U << 5, /* inside an lvalue's index */
    MEM_FL_CONST_LHS = 1U << 6, /* a write with constant indices */
    MEM_FL_VAR_LHS = 1U << 7,   /* a write with a variable index */
    MEM_FL_CONT = 1U << 8       /* written by a continuous assignment */
};

/* What the scan learned about one array (parallel to the memory table). */
typedef struct odin3_mem_fact {
    uint32_t flags;
    uint32_t eq1_proc; /* serial of the block whose blocking write was seen (0: none) */
    odin3_vec places;  /* odin3_loc, in insertion order (Yosys's mem2reg_places) */
    odin3_ast_id async_node, cont_node; /* the first write of each kind (the --nomem2reg error) */
} odin3_mem_fact;

/* The table index of the array sym names; MEM_NONE when it is not in the table. */
#define MEM_NONE UINT32_MAX
uint32_t odin3_elab2_mem_find(const odin3_elab2 *e2, odin3_sym_id sym);

/* The table entry at index mem for update; NULL outside the table. */
odin3_elab_mem *odin3_elab2_mem_at(odin3_elab2 *e2, uint32_t mem);

/* The number of indices of an unpacked dimension, and its lowest declared one. */
uint64_t odin3_elab2_dim_size(const odin3_dim *dim);
int32_t odin3_elab2_dim_low(const odin3_dim *dim);

/*
 * Pass 1 of Yosys's mem2reg_as_needed over module_node: fills facts (one odin3_mem_fact per table
 * entry, zeroed on entry) from the module's always and initial blocks and continuous assignments.
 * A function's or task's body is not scanned (its statements run where it is called, Task 23).
 * ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab2_mem_scan(odin3_elab2 *e2, odin3_ast_id module_node, odin3_vec *facts);

/*
 * Marks every bit of a register-array word's wire as assigned in an always block (the table behind
 * odin3_elab2_always_assigned_word; elab2_drive.c). ODIN3_ERR_NO_MEMORY.
 */
odin3_status odin3_elab2_word_assigned(odin3_elab2 *e2, odin3_wire_id wire);

#endif
