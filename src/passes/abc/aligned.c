/*
 * aligned.c — the aligned MiniLUT of the name map (3D spec §6.3, AB-16; plan Task 21): the result
 * with the loaded AIG's interface.
 */
#include "passes/abc/aligned.h"
#include "odin3/odin3.h"
#include "passes/abc/region.h"
#include "passes/abc/run.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    MINI_NULL = 0x7FFFFFFF,
    MINI_NULL2 = 0x7FFFFFFE,
    FIRST_OBJ = 2, /* objects 0 and 1 are the constants */
    TRUTH_BITS_LOG = 5,
    MIN_LUT_SIZE = 2,
    MAX_LUT_SIZE = 16
};

#define NONE UINT32_MAX /* a k that no result register is named after */

enum { KIND_PI = 0, KIND_PO, KIND_LUT };

/* One alignment. */
typedef struct align {
    const odin3_abc_luts *src;
    odin3_abc_luts *dst;
    uint32_t *map;    /* per source object: its object in dst (POs: unused) */
    uint32_t *pos;    /* the source's PO objects in order of appearance */
    uint32_t *j_of_k; /* per input register k: the result register named k, or NONE */
    uint32_t real;    /* real inputs */
    uint32_t regs_in; /* input registers */
    uint32_t luts, n_pos, real_cos;
    uint32_t words; /* 32-bit truth words per object */
} align;

static uint32_t kind_of(const align *al, uint32_t obj) {
    const int32_t *fan = al->src->fanins + (size_t)al->src->lut_size * obj;
    if (fan[0] == MINI_NULL) {
        return KIND_PI;
    }
    return fan[1] == MINI_NULL2 ? KIND_PO : KIND_LUT;
}

static int32_t *dst_fanins(const align *al, uint32_t obj) {
    return (int32_t *)al->dst->fanins + (size_t)al->dst->lut_size * obj;
}

/* Counts the kinds and lists the POs. */
static void count_kinds(align *al) {
    uint32_t pis = 0;
    for (uint32_t obj = FIRST_OBJ; obj < al->src->n_objs; obj++) {
        uint32_t kind = kind_of(al, obj);
        pis += kind == KIND_PI ? 1U : 0U;
        al->luts += kind == KIND_LUT ? 1U : 0U;
        if (kind == KIND_PO) {
            al->pos[al->n_pos++] = obj;
        }
    }
    al->real = pis - al->src->n_regs;
    al->real_cos = al->n_pos - al->src->n_regs;
}

/* Where the numbering of the source's PIs and LUTs stands. */
typedef struct numbering {
    uint32_t pi, lut;
} numbering;

/* The next PI: a real input keeps its place; a register PI goes to the slot of the input register
 * its name says, which no other register may carry. */
static odin3_status number_pi(align *al, uint32_t obj, numbering *num) {
    if (num->pi < al->real) {
        al->map[obj] = FIRST_OBJ + num->pi++;
        return ODIN3_OK;
    }
    int32_t name = al->src->ci_from != NULL ? al->src->ci_from[num->pi] : -1;
    uint32_t reg = num->pi - al->real;
    num->pi++;
    if (name < (int32_t)al->real || (uint32_t)name - al->real >= al->regs_in ||
        al->j_of_k[(uint32_t)name - al->real] != NONE) {
        return ODIN3_ERR_INVALID_ARG;
    }
    al->j_of_k[(uint32_t)name - al->real] = reg;
    al->map[obj] = FIRST_OBJ + (uint32_t)name;
    return ODIN3_OK;
}

/* The numbering: constants and real PIs stay, a register PI goes to its input register's slot,
 * LUTs follow the PIs in order of appearance. */
static odin3_status number_objects(align *al) {
    numbering num = {0, 0};
    al->map[0] = 0;
    al->map[1] = 1;
    for (uint32_t obj = FIRST_OBJ; obj < al->src->n_objs; obj++) {
        uint32_t kind = kind_of(al, obj);
        if (kind == KIND_LUT) {
            al->map[obj] = FIRST_OBJ + al->real + al->regs_in + num.lut++;
        } else if (kind == KIND_PI) {
            odin3_status st = number_pi(al, obj, &num);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

/* Copies LUT obj of the source: remapped fanins, the same table words. */
static void copy_lut(const align *al, uint32_t obj) {
    const odin3_abc_luts *src = al->src;
    const int32_t *from = src->fanins + (size_t)src->lut_size * obj;
    int32_t *to = dst_fanins(al, al->map[obj]);
    for (uint32_t slot = 0; slot < src->lut_size && from[slot] < MINI_NULL2; slot++) {
        to[slot] = (int32_t)al->map[from[slot]];
    }
    memcpy((uint32_t *)al->dst->truths + (size_t)al->words * al->map[obj],
           src->truths + (size_t)al->words * obj, sizeof(uint32_t) * al->words);
}

/* One PO of dst: its object number and its driver (a dst object). */
typedef struct po_slot {
    uint32_t obj;
    uint32_t driver;
} po_slot;

static void set_po(const align *al, po_slot po) {
    int32_t *fan = dst_fanins(al, po.obj);
    fan[0] = (int32_t)po.driver;
    fan[1] = MINI_NULL2;
}

/* The POs: the real outputs as in the source, then each input register's D (constant 0 for a
 * register nothing is named after). */
static void fill_pos(const align *al) {
    uint32_t base = FIRST_OBJ + al->real + al->regs_in + al->luts;
    for (uint32_t idx = 0; idx < al->real_cos; idx++) {
        const int32_t *fan = al->src->fanins + (size_t)al->src->lut_size * al->pos[idx];
        set_po(al, (po_slot){base + idx, al->map[fan[0]]});
    }
    for (uint32_t reg = 0; reg < al->regs_in; reg++) {
        uint32_t from = al->j_of_k[reg];
        uint32_t drv = 0;
        if (from != NONE) {
            const int32_t *fan =
                al->src->fanins + (size_t)al->src->lut_size * al->pos[al->real_cos + from];
            drv = al->map[fan[0]];
        }
        set_po(al, (po_slot){base + al->real_cos + reg, drv});
    }
}

static odin3_status alloc_dst(align *al, uint32_t n_objs, odin3_arena *arena) {
    size_t fanin_n = (size_t)al->src->lut_size * n_objs;
    size_t truth_n = (size_t)al->words * n_objs;
    int32_t *fanins = odin3_arena_alloc(arena, sizeof(int32_t) * fanin_n);
    uint32_t *truths = odin3_arena_alloc(arena, sizeof(uint32_t) * truth_n);
    if (fanins == NULL || truths == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t at = 0; at < fanin_n; at++) {
        fanins[at] = MINI_NULL;
    }
    memset(truths, 0, sizeof(uint32_t) * truth_n);
    for (uint32_t word = 0; word < al->words; word++) {
        truths[al->words + word] = UINT32_MAX; /* the constant 1 */
    }
    *al->dst = (odin3_abc_luts){fanins, truths, NULL, n_objs, al->regs_in, al->src->lut_size};
    return ODIN3_OK;
}

/* The source has as many real inputs and outputs as the region sent. */
static bool shape_ok(const align *al, const odin3_abc_run *run) {
    const odin3_abc_luts *res = al->src;
    return res->n_regs <= al->n_pos && al->real == run->region->cis.len &&
           al->real_cos == run->region->cos.len;
}

static void copy_luts(const align *al) {
    for (uint32_t obj = FIRST_OBJ; obj < al->src->n_objs; obj++) {
        if (kind_of(al, obj) == KIND_LUT) {
            copy_lut(al, obj);
        }
    }
}

odin3_status odin3_abc_aligned_build(const odin3_abc_run *run, const odin3_abc_luts *res,
                                     odin3_arena *arena, odin3_abc_luts *aligned) {
    if (res->lut_size < MIN_LUT_SIZE || res->lut_size > MAX_LUT_SIZE) {
        return ODIN3_ERR_INVALID_ARG;
    }
    align al = {res,
                aligned,
                NULL,
                NULL,
                NULL,
                0,
                (uint32_t)run->region->regs.len,
                0,
                0,
                0,
                res->lut_size > TRUTH_BITS_LOG ? 1U << (res->lut_size - TRUTH_BITS_LOG) : 1U};
    size_t count = (size_t)res->n_objs + 1U;
    al.map = odin3_arena_alloc(arena, sizeof(uint32_t) * count);
    al.pos = odin3_arena_alloc(arena, sizeof(uint32_t) * count);
    al.j_of_k = odin3_arena_alloc(arena, sizeof(uint32_t) * ((size_t)al.regs_in + 1U));
    if (al.map == NULL || al.pos == NULL || al.j_of_k == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t reg = 0; reg < al.regs_in; reg++) {
        al.j_of_k[reg] = NONE;
    }
    count_kinds(&al);
    if (!shape_ok(&al, run)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = number_objects(&al);
    st = st == ODIN3_OK
             ? alloc_dst(&al, FIRST_OBJ + al.real + al.regs_in + al.luts + al.real_cos + al.regs_in,
                         arena)
             : st;
    if (st == ODIN3_OK) {
        copy_luts(&al);
        fill_pos(&al);
    }
    return st;
}
