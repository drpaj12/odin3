/*
 * dfflibmap.h — the `dfflibmap <library>` pass (gatemap spec 6, G-6; plan Tasks 8 and 9): maps
 * every $_DFF_P_, $_DFF_N_, $_DLATCH_P_ and $_DLATCH_N_ onto a state cell of an .o3lib library
 * read with read_techlib. Internal: the state cell choice (choose.c), shared by the pass
 * (dfflibmap.c) and its unit test.
 */
#ifndef ODIN3_PASSES_DFFLIBMAP_DFFLIBMAP_H
#define ODIN3_PASSES_DFFLIBMAP_DFFLIBMAP_H

#include "ir/design.h"
#include "techlib/libcells.h"

#include <stdbool.h>
#include <stdint.h>

/* The bit flip-flop and latch types dfflibmap maps: $_DFF_P_, $_DFF_N_, $_DLATCH_P_, $_DLATCH_N_.
 */
typedef enum odin3_dlm_kind {
    ODIN3_DLM_DFF_P = 0,
    ODIN3_DLM_DFF_N,
    ODIN3_DLM_DLATCH_P,
    ODIN3_DLM_DLATCH_N,
    ODIN3_DLM_KINDS
} odin3_dlm_kind;

/* A chosen state cell: index into cells->states, and whether its clock needs the library's
 * inverter (a $_DFF_N_ on a posedge cell). */
typedef struct odin3_dlm_choice {
    uint32_t state;
    bool invert_clock;
} odin3_dlm_choice;

/*
 * Chooses the state cell of cells (odin3_libcells_collect) for a cell of kind kind whose INIT
 * parameter is init (BLIF: 0, 1, 2 don't care, 3 unknown). The trigger must be POSEDGE (DFF_P),
 * NEGEDGE (DFF_N), HIGH (DLATCH_P) or LOW (DLATCH_N); a cell is compatible when its init is x
 * (-1), or init is 2 or 3, or the two are equal. Among compatible cells: the smallest numeric
 * area, a cell without a numeric area after every one with, then library order. A DFF_N with no
 * NEGEDGE match takes the POSEDGE choice with invert_clock set, only when cells->inv is set;
 * latches never invert. True and *out on a match; false (out untouched) when nothing matches or
 * kind is out of range.
 */
bool odin3_dlm_choose(const odin3_design *design, const odin3_libcells *cells, odin3_dlm_kind kind,
                      int64_t init, odin3_dlm_choice *out);

#endif
