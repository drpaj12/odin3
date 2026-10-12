/*
 * proc_clock.c — proc's clock pre-check (2E spec §5.5, PR-11) and E4 (§11, PR-17; plan point (n)):
 * after arst, the user errors Yosys 0.55's proc_dff (`Multiple edge sensitive events found for
 * this signal!`, `Missing edge-sensitive event for this signal!`) and proc_arst (`Async reset ...
 * causes memory write to ...`) would raise, found before anything in the IR changes.
 */
#include "passes/proc/proc_internal.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/proc/pw.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NO_SYNC UINT32_MAX /* no sync */
#define KEY_BIT_MASK                                                                               \
    UINT32_MAX /* a bit key's low half: the bit; the high half all ones: no wire                   \
                */
enum { KEY_WIRE_SHIFT = 32 };

static bool is_edge(uint8_t type) {
    return type == ODIN3_SYNC_POSEDGE || type == ODIN3_SYNC_NEGEDGE;
}

static bool is_level(uint8_t type) {
    return type == ODIN3_SYNC_LEVEL0 || type == ODIN3_SYNC_LEVEL1;
}

static const odin3_pw_sync *sync_at(const odin3_pw *pw, uint32_t index) {
    return odin3_vec_cat(&pw->syncs, index);
}

/* --- the clock pre-check (§5.5) -------------------------------------------------------------- */

/* One lhs bit of an edge or level sync action, in the live view, with its sort key. */
typedef struct ck_bit {
    uint64_t key;
    odin3_pw_bit bit;
    uint32_t sync;
} ck_bit;

static int ck_cmp(const void *lhs, const void *rhs) {
    const ck_bit *left = lhs;
    const ck_bit *right = rhs;
    if (left->key != right->key) {
        return left->key < right->key ? -1 : 1;
    }
    return (left->sync > right->sync) - (left->sync < right->sync);
}

/* What the syncs updating one bit say: E1 (two edge syncs, the first two in sync order), E5
 * (level syncs only) or nothing. */
typedef enum ck_kind { CK_OK, CK_E1, CK_E5 } ck_kind;

typedef struct ck_verdict {
    ck_kind kind;
    uint32_t first, second; /* the edge syncs of E1 */
} ck_verdict;

/* Every lhs bit of every edge and level sync action, in the live view, sorted by bit key then
 * sync. ODIN3_ERR_NO_MEMORY. */
static odin3_status collect_bits(odin3_proc_mctx *mctx, const odin3_pw *pw, odin3_vec *out) {
    for (uint32_t idx = 0; idx < pw->syncs.len; idx++) {
        const odin3_pw_sync *sync = sync_at(pw, idx);
        if (!is_edge(sync->type) && !is_level(sync->type)) {
            continue;
        }
        for (size_t i = 0; i < sync->actions.len; i++) {
            odin3_pw_sig lhs = ((const odin3_pw_action *)odin3_vec_cat(&sync->actions, i))->lhs;
            if (odin3_vec_reserve(out, out->len + lhs.len) != ODIN3_OK) {
                return ODIN3_ERR_NO_MEMORY;
            }
            for (uint32_t k = 0; k < lhs.len; k++) {
                odin3_pw_bit bit = odin3_proc_find(mctx, odin3_pw_sig_bits(pw, lhs)[k]);
                *(ck_bit *)odin3_vec_push(out) = /* reserved */
                    (ck_bit){odin3_proc_bit_key(mctx->module, bit), bit, idx};
            }
        }
    }
    if (out->len > 1) {
        qsort(out->data, out->len, sizeof(ck_bit), ck_cmp);
    }
    return ODIN3_OK;
}

static const ck_bit *bit_at(const odin3_vec *bits, size_t index) {
    return odin3_vec_cat(bits, index);
}

/* The verdict on the bit whose entries start at *at; *at moves past them. */
static ck_verdict judge(const odin3_pw *pw, const odin3_vec *bits, size_t *at) {
    ck_verdict verdict = {CK_OK, NO_SYNC, NO_SYNC};
    bool level = false;
    size_t i = *at;
    for (; i < bits->len && bit_at(bits, i)->key == bit_at(bits, *at)->key; i++) {
        uint32_t idx = bit_at(bits, i)->sync;
        level = level || is_level(sync_at(pw, idx)->type);
        if (!is_edge(sync_at(pw, idx)->type)) {
            continue;
        }
        if (verdict.first == NO_SYNC) {
            verdict.first = idx;
        } else if (idx != verdict.first && verdict.second == NO_SYNC) {
            verdict.second = idx;
        }
    }
    *at = i;
    if (verdict.second != NO_SYNC) {
        verdict.kind = CK_E1;
    } else if (verdict.first == NO_SYNC && level) {
        verdict.kind = CK_E5;
    }
    return verdict;
}

static bool same_verdict(ck_verdict lhs, ck_verdict rhs) {
    return lhs.kind == rhs.kind && lhs.first == rhs.first && lhs.second == rhs.second;
}

/* Is key the next bit of prev's wire? (Bits without a primary wire are never joined.) */
static bool next_on_wire(uint64_t prev, uint64_t key) {
    uint64_t wire = prev >> KEY_WIRE_SHIFT;
    return wire != KEY_BIT_MASK && key >> KEY_WIRE_SHIFT == wire &&
           (key & KEY_BIT_MASK) == (prev & KEY_BIT_MASK) + 1;
}

/* The error's bits: the bit whose entries start at `at` (its verdict is verdict), then each next
 * bit of its wire with the same verdict. ODIN3_ERR_NO_MEMORY. */
static odin3_status error_bits(const odin3_pw *pw, const odin3_vec *bits, size_t at,
                               ck_verdict verdict, odin3_vec *out) {
    uint64_t prev = 0;
    while (at < bits->len && (out->len == 0 || next_on_wire(prev, bit_at(bits, at)->key))) {
        size_t next = at;
        if (!same_verdict(judge(pw, bits, &next), verdict)) {
            break;
        }
        if (odin3_vec_reserve(out, out->len + 1) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *(odin3_pw_bit *)odin3_vec_push(out) = bit_at(bits, at)->bit; /* reserved */
        prev = bit_at(bits, at)->key;
        at = next;
    }
    return ODIN3_OK;
}

/* Names n bits (one sync signal, or an error's bits) into out. ODIN3_ERR_NO_MEMORY. */
static odin3_status name(const odin3_proc_mctx *mctx, const odin3_pw_bit *bits, uint32_t n,
                         odin3_strbuf *out) {
    return odin3_proc_name_bits(mctx, bits, n, out);
}

/* Reports the verdict on the bits named (§15's E1a, E1b or E5) at the process. */
static odin3_status clock_error(odin3_proc_mctx *mctx, const odin3_pw *pw, ck_verdict verdict,
                                const odin3_vec *named) {
    odin3_strbuf names[3];
    for (size_t i = 0; i < 3; i++) {
        odin3_strbuf_init(&names[i]);
    }
    odin3_status st = name(mctx, named->data, (uint32_t)named->len, &names[0]);
    if (st == ODIN3_OK && verdict.kind == CK_E5) {
        odin3_proc_error(mctx, pw->prov,
                         "'%s' is assigned only under level events; a register needs a clock edge",
                         names[0].data);
    } else if (st == ODIN3_OK) {
        const odin3_pw_sync *one = sync_at(pw, verdict.first);
        const odin3_pw_sync *two = sync_at(pw, verdict.second);
        st = name(mctx, &one->signal, 1, &names[1]);
        st = st == ODIN3_OK ? name(mctx, &two->signal, 1, &names[2]) : st;
        bool dual = odin3_proc_find(mctx, one->signal) == odin3_proc_find(mctx, two->signal) &&
                    one->type != two->type;
        if (st == ODIN3_OK && dual) {
            odin3_proc_error(mctx, pw->prov,
                             "'%s' is clocked by both edges of '%s'; dual-edge registers are not "
                             "supported",
                             names[0].data, names[1].data);
        } else if (st == ODIN3_OK) {
            odin3_proc_error(mctx, pw->prov,
                             "'%s' is clocked by both '%s' and '%s'; a second edge event must be "
                             "an asynchronous reset tested by the block's only top-level 'if'",
                             names[0].data, names[1].data, names[2].data);
        }
    }
    for (size_t i = 0; i < 3; i++) {
        odin3_strbuf_free(&names[i]);
    }
    return st == ODIN3_OK ? ODIN3_ERR_PARSE : st;
}

odin3_status odin3_pw_check_clocks(odin3_proc_mctx *mctx, const odin3_pw *pw) {
    odin3_vec bits;
    odin3_vec named;
    odin3_vec_init(&bits, sizeof(ck_bit));
    odin3_vec_init(&named, sizeof(odin3_pw_bit));
    odin3_status st = collect_bits(mctx, pw, &bits);
    size_t at = 0;
    while (st == ODIN3_OK && at < bits.len) {
        size_t start = at;
        ck_verdict verdict = judge(pw, &bits, &at);
        if (verdict.kind != CK_OK) {
            st = error_bits(pw, &bits, start, verdict, &named);
            st = st == ODIN3_OK ? clock_error(mctx, pw, verdict, &named) : st;
        }
    }
    odin3_vec_free(&bits);
    odin3_vec_free(&named);
    return st;
}

/* --- E4 (§11, point (n)) --------------------------------------------------------------------- */

/* Is pin a WR_EN, WR_ADDR or WR_DATA pin of a $mem? *memid gets the $mem's MEMID string. */
static bool mem_write_pin(const odin3_module *module, odin3_pin_id pin, uint32_t *memid) {
    odin3_node_id node = odin3_pin_node(module, pin);
    const odin3_celltype_def *def =
        odin3_celltype_get(odin3_module_design(module), odin3_node_type(module, node));
    if (def == NULL || strcmp(def->name, "$mem") != 0) {
        return false;
    }
    const char *port = def->ports[odin3_pin_port(module, pin)].name;
    if (strcmp(port, "WR_EN") != 0 && strcmp(port, "WR_ADDR") != 0 &&
        strcmp(port, "WR_DATA") != 0) {
        return false;
    }
    *memid = 0;
    for (uint32_t i = 0; i < def->n_params; i++) {
        const odin3_value *val = odin3_node_param(module, node, i);
        if (strcmp(def->params[i].name, "MEMID") == 0 && val != NULL &&
            val->kind == ODIN3_VAL_STRING) {
            *memid = val->str;
        }
    }
    return true;
}

/* The MEMID of the first $mem write pin on an O net of pw no sync updates (marked in the scratch
 * index); false when there is none. */
static bool find_mem_write(const odin3_proc_mctx *mctx, const odin3_pw *pw, uint32_t *memid) {
    const odin3_module *module = mctx->module;
    odin3_pinslice outs = odin3_node_port(module, pw->node, ODIN3_PROC_PORT_O);
    for (uint32_t i = 0; i < outs.count; i++) {
        odin3_net_id net = odin3_pin_net(module, (odin3_pin_id){outs.first.v + i});
        if (!odin3_net_valid(net) || odin3_proc_scratch_get(mctx, net) != 0) {
            continue;
        }
        odin3_pinlist sinks = odin3_net_sinks(module, net);
        for (uint32_t k = 0; k < sinks.count; k++) {
            if (mem_write_pin(module, sinks.pins[k], memid)) {
                return true;
            }
        }
    }
    return false;
}

/* Marks every net a sync action updates. */
static void mark_updated(odin3_proc_mctx *mctx, const odin3_pw *pw) {
    for (uint32_t idx = 0; idx < pw->syncs.len; idx++) {
        const odin3_pw_sync *sync = sync_at(pw, idx);
        for (size_t i = 0; i < sync->actions.len; i++) {
            odin3_pw_sig lhs = ((const odin3_pw_action *)odin3_vec_cat(&sync->actions, i))->lhs;
            for (uint32_t k = 0; k < lhs.len; k++) {
                odin3_pw_bit bit = odin3_pw_sig_bits(pw, lhs)[k];
                if ((bit & ODIN3_PW_CONST) == 0) {
                    odin3_proc_scratch_set(mctx, (odin3_net_id){bit}, 1);
                }
            }
        }
    }
}

/* E4 at the process: the memory (its MEMID without RTLIL's leading backslash) and the reset. */
static odin3_status memwr_error(odin3_proc_mctx *mctx, const odin3_pw *pw, uint32_t memid,
                                const odin3_pw_sync *level) {
    const char *mem = memid != 0 ? odin3_strtab_get(odin3_design_strtab(mctx->design), memid) : "";
    mem = mem != NULL && mem[0] == '\\' ? mem + 1 : mem;
    odin3_strbuf rst;
    odin3_strbuf_init(&rst);
    odin3_status st = name(mctx, &level->signal, 1, &rst);
    if (st == ODIN3_OK) {
        odin3_proc_error(mctx, pw->prov,
                         "memory '%s' is written in an always block with asynchronous reset '%s'",
                         mem != NULL ? mem : "", rst.data);
        st = ODIN3_ERR_PARSE;
    }
    odin3_strbuf_free(&rst);
    return st;
}

odin3_status odin3_pw_check_memwr(odin3_proc_mctx *mctx, const odin3_pw *pw) {
    const odin3_pw_sync *level = NULL;
    for (uint32_t idx = 0; idx < pw->syncs.len && level == NULL; idx++) {
        level = is_level(sync_at(pw, idx)->type) ? sync_at(pw, idx) : NULL;
    }
    if (level == NULL || !odin3_node_live(mctx->module, pw->node)) {
        return ODIN3_OK;
    }
    odin3_status st = odin3_proc_scratch_grow(mctx);
    if (st != ODIN3_OK) {
        return st;
    }
    mark_updated(mctx, pw);
    uint32_t memid = 0;
    bool found = find_mem_write(mctx, pw, &memid);
    odin3_proc_scratch_reset(mctx);
    return found ? memwr_error(mctx, pw, memid, level) : ODIN3_OK;
}
