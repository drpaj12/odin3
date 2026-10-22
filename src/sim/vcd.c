/* vcd.c — the VCD waveform writer (see vcd.h; spec SW-1 to SW-3). */
#include "sim/vcd.h"

#include "odin3/odin3.h"
#include "sim/signals.h"
#include "sim/sim.h"
#include "util/alloc.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { CODE_BASE = 94, CODE_DIGITS_32 = 5 };
#define NONE UINT32_MAX

/* Five base-94 digits cover every uint32 index, and the code buffer holds at least that many. */
_Static_assert((uint64_t)CODE_BASE *CODE_BASE *CODE_BASE *CODE_BASE *CODE_BASE > UINT32_MAX,
               "five base-94 digits must cover a uint32 index");
_Static_assert(ODIN3_SIM_VCD_CODE_MAX - 1 >= CODE_DIGITS_32,
               "the code buffer must hold the digits of a uint32 index");

void odin3_sim_vcd_code(uint32_t index, char code[ODIN3_SIM_VCD_CODE_MAX]) {
    size_t len = 0;
    do {
        code[len++] = (char)('!' + (index % CODE_BASE));
        index /= CODE_BASE;
    } while (index != 0);
    code[len] = '\0';
}

/* --- header ----------------------------------------------------------------------------------- */

/* The tree of scopes: first child and next sibling per scope, first signal and next per signal. */
typedef struct tree {
    uint32_t *first_child, *next_sibling, *first_sig, *next_sig, *stack;
    uint32_t n_scopes;
} tree;

static void tree_free(tree *tr) {
    odin3_util_free(tr->first_child);
    odin3_util_free(tr->next_sibling);
    odin3_util_free(tr->first_sig);
    odin3_util_free(tr->next_sig);
    odin3_util_free(tr->stack);
}

static bool tree_alloc(tree *tr, const odin3_sim_signals *sig) {
    tr->n_scopes = odin3_sim_signals_scope_count(sig);
    size_t sigs = (size_t)odin3_sim_signals_listed(sig) + 1U;
    tr->first_child = odin3_util_malloc(sizeof(uint32_t) * tr->n_scopes);
    tr->next_sibling = odin3_util_malloc(sizeof(uint32_t) * tr->n_scopes);
    tr->first_sig = odin3_util_malloc(sizeof(uint32_t) * tr->n_scopes);
    tr->next_sig = odin3_util_malloc(sizeof(uint32_t) * sigs);
    tr->stack = odin3_util_malloc(sizeof(uint32_t) * tr->n_scopes);
    return tr->first_child != NULL && tr->next_sibling != NULL && tr->first_sig != NULL &&
           tr->next_sig != NULL && tr->stack != NULL;
}

/* Links the lists, walking backwards so each list ends up in ascending order. */
static void tree_link(tree *tr, const odin3_sim_signals *sig) {
    for (uint32_t scope = 0; scope < tr->n_scopes; scope++) {
        tr->first_child[scope] = NONE;
        tr->first_sig[scope] = NONE;
    }
    for (uint32_t scope = tr->n_scopes; scope-- > 1;) {
        uint32_t parent = odin3_sim_signals_scope(sig, scope)->parent;
        tr->next_sibling[scope] = tr->first_child[parent];
        tr->first_child[parent] = scope;
    }
    for (uint32_t id = odin3_sim_signals_listed(sig); id > 0; id--) {
        uint32_t scope = odin3_sim_signals_at(sig, id)->scope;
        tr->next_sig[id] = tr->first_sig[scope];
        tr->first_sig[scope] = id;
    }
}

/* A name with every byte outside `!`..`~` replaced by `_` (SW-2). */
static void put_name(FILE *out, const char *text) {
    for (const char *cur = text; *cur != '\0'; cur++) {
        unsigned char chr = (unsigned char)*cur;
        (void)fputc(chr >= '!' && chr <= '~' ? chr : '_', out);
    }
}

static void put_scope_open(const odin3_sim_vcd *vcd, uint32_t scope) {
    const odin3_sim_signals *sig = vcd->sig;
    (void)fputs("$scope module ", vcd->out);
    put_name(vcd->out, odin3_sim_signals_text(sig, odin3_sim_signals_scope(sig, scope)->leaf));
    (void)fputs(" $end\n", vcd->out);
}

static void put_var(const odin3_sim_vcd *vcd, uint32_t id) {
    const odin3_sim_signal *item = odin3_sim_signals_at(vcd->sig, id);
    char code[ODIN3_SIM_VCD_CODE_MAX];
    odin3_sim_vcd_code(id - 1U, code);
    (void)fprintf(vcd->out, "$var wire %u %s ", item->width, code);
    put_name(vcd->out, odin3_sim_signals_text(vcd->sig, item->leaf));
    if (item->width > 1) {
        (void)fprintf(vcd->out, " [%d:%d]", item->msb, item->lsb);
    }
    (void)fputs(" $end\n", vcd->out);
}

/* Opens a scope and lists its signals. */
static void enter_scope(const odin3_sim_vcd *vcd, const tree *tr, uint32_t scope) {
    put_scope_open(vcd, scope);
    for (uint32_t id = tr->first_sig[scope]; id != NONE; id = tr->next_sig[id]) {
        put_var(vcd, id);
    }
}

/* Depth first over the scope tree with an explicit stack; children in scope-index order. */
static void write_scopes(const odin3_sim_vcd *vcd, tree *tr) {
    size_t depth = 0;
    tr->stack[depth++] = 0;
    enter_scope(vcd, tr, 0);
    while (depth > 0) {
        uint32_t top = tr->stack[depth - 1];
        uint32_t child = tr->first_child[top];
        if (child == NONE) {
            (void)fputs("$upscope $end\n", vcd->out);
            depth--;
            continue;
        }
        tr->first_child[top] = tr->next_sibling[child];
        enter_scope(vcd, tr, child);
        tr->stack[depth++] = child;
    }
}

static odin3_status write_header(const odin3_sim_vcd *vcd) {
    tree tr = {0};
    if (!tree_alloc(&tr, vcd->sig)) {
        tree_free(&tr);
        return ODIN3_ERR_NO_MEMORY;
    }
    tree_link(&tr, vcd->sig);
    (void)fputs("$version odin3 sim $end\n$timescale 1ns $end\n", vcd->out);
    write_scopes(vcd, &tr);
    (void)fputs("$enddefinitions $end\n", vcd->out);
    tree_free(&tr);
    return ODIN3_OK;
}

/* --- values ----------------------------------------------------------------------------------- */

/* The value line of signal id with the bits cur (bit 0 first). */
static void put_value(const odin3_sim_vcd *vcd, uint32_t id, const uint8_t *cur, uint32_t width) {
    char code[ODIN3_SIM_VCD_CODE_MAX];
    odin3_sim_vcd_code(id - 1U, code);
    if (width == 1) {
        (void)fprintf(vcd->out, "%c%s\n", cur[0] != 0 ? '1' : '0', code);
        return;
    }
    (void)fputc('b', vcd->out);
    for (uint32_t k = width; k-- > 0;) {
        (void)fputc(cur[k] != 0 ? '1' : '0', vcd->out);
    }
    (void)fprintf(vcd->out, " %s\n", code);
}

/* Reads the current values of every listed signal into the scratch half of last. */
static odin3_status read_all(const odin3_sim_vcd *vcd) {
    uint8_t *cur = vcd->last + vcd->n_last;
    for (uint32_t id = 1; id <= odin3_sim_signals_listed(vcd->sig); id++) {
        uint32_t width = odin3_sim_signals_at(vcd->sig, id)->width;
        odin3_status st = odin3_sim_signals_read(vcd->sig, id, cur, width);
        if (st != ODIN3_OK) {
            return st;
        }
        cur += width;
    }
    return ODIN3_OK;
}

static uint32_t total_bits(const odin3_sim_signals *sig) {
    uint64_t bits = 0;
    for (uint32_t id = 1; id <= odin3_sim_signals_listed(sig); id++) {
        bits += odin3_sim_signals_at(sig, id)->width;
    }
    return bits > (UINT32_MAX / 2U) ? NONE : (uint32_t)bits;
}

static odin3_status stream_status(const odin3_sim_vcd *vcd) {
    return ferror(vcd->out) != 0 ? ODIN3_ERR_IO : ODIN3_OK;
}

odin3_status odin3_sim_vcd_begin(odin3_sim_vcd *vcd, const odin3_sim_signals *sig, FILE *out) {
    memset(vcd, 0, sizeof *vcd);
    uint32_t bits = total_bits(sig);
    if (bits == NONE) {
        return ODIN3_ERR_NO_MEMORY;
    }
    vcd->sig = sig;
    vcd->out = out;
    vcd->last = odin3_util_calloc(2U * (size_t)bits + 1U);
    if (vcd->last == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    vcd->n_last = bits;
    odin3_status st = write_header(vcd);
    if (st == ODIN3_OK) {
        st = read_all(vcd);
    }
    if (st == ODIN3_OK) {
        (void)fputs("#0\n$dumpvars\n", out);
        const uint8_t *cur = vcd->last + bits;
        for (uint32_t id = 1; id <= odin3_sim_signals_listed(sig); id++) {
            uint32_t width = odin3_sim_signals_at(sig, id)->width;
            put_value(vcd, id, cur, width);
            cur += width;
        }
        (void)fputs("$end\n", out);
        memcpy(vcd->last, vcd->last + bits, bits);
        st = stream_status(vcd);
    }
    if (st != ODIN3_OK) {
        odin3_sim_vcd_free(vcd);
    }
    return st;
}

odin3_status odin3_sim_vcd_sample(odin3_sim_vcd *vcd, uint64_t time) {
    odin3_status read = read_all(vcd);
    if (read != ODIN3_OK) {
        return read;
    }
    bool stamped = false;
    uint8_t *last = vcd->last;
    const uint8_t *cur = vcd->last + vcd->n_last;
    for (uint32_t id = 1; id <= odin3_sim_signals_listed(vcd->sig); id++) {
        uint32_t width = odin3_sim_signals_at(vcd->sig, id)->width;
        if (memcmp(last, cur, width) != 0) {
            if (!stamped) {
                (void)fprintf(vcd->out, "#%" PRIu64 "\n", time);
                stamped = true;
            }
            put_value(vcd, id, cur, width);
            memcpy(last, cur, width);
        }
        last += width;
        cur += width;
    }
    return stream_status(vcd);
}

odin3_status odin3_sim_vcd_cycle(odin3_sim_vcd *vcd, odin3_sim *sim, uint64_t cycle) {
    const uint64_t base = cycle * ODIN3_SIM_VCD_PERIOD;
    odin3_sim_settle(sim);
    odin3_status st = odin3_sim_vcd_sample(vcd, base + ODIN3_SIM_VCD_INPUT_AT);
    if (st == ODIN3_OK) {
        st = odin3_sim_clock_edge(sim, ODIN3_SIM_POSEDGE);
    }
    if (st == ODIN3_OK) {
        st = odin3_sim_vcd_sample(vcd, base + ODIN3_SIM_VCD_RISE_AT);
    }
    if (st == ODIN3_OK) {
        st = odin3_sim_clock_edge(sim, ODIN3_SIM_NEGEDGE);
    }
    return st == ODIN3_OK ? odin3_sim_vcd_sample(vcd, base + ODIN3_SIM_VCD_PERIOD) : st;
}

void odin3_sim_vcd_free(odin3_sim_vcd *vcd) {
    if (vcd == NULL) {
        return;
    }
    odin3_util_free(vcd->last);
    memset(vcd, 0, sizeof *vcd);
}
