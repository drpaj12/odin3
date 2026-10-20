/*
 * recode.c — fsm_recode (spec R-1 to R-5; Yosys 0.55 passes/fsm/fsm_recode.cc:55-126).
 */
#include "odin3/odin3.h"
#include "passes/fsm/fsm.h"
#include "passes/fsm/fsmdata.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { LABEL_MAX = 160, AUTO_ONE_HOT_LIMIT = 32 };

int32_t odin3_fsm_ceil_log2(int32_t value) {
    int32_t bits = 0;
    for (uint32_t rest = value > 1 ? (uint32_t)(value - 1) : 0U; rest != 0; rest >>= 1) {
        bits++;
    }
    return bits;
}

typedef enum enc_kind { ENC_NONE, ENC_USER, ENC_ONE_HOT, ENC_BINARY, ENC_AUTO } enc_kind;

static const char *const k_enc_names[] = {"none", "user", "one-hot", "binary", "auto"};

/* The kind a name stands for, or -1. */
static int kind_of(const char *name) {
    for (int kind = 0; kind <= ENC_AUTO; kind++) {
        if (strcmp(name, k_enc_names[kind]) == 0) {
            return kind;
        }
    }
    return -1;
}

static void set_final(odin3_fsm_inst *inst, enc_kind kind) {
    (void)snprintf(inst->final_encoding, sizeof inst->final_encoding, "%s", k_enc_names[kind]);
}

/* R-1: the attribute, then the unknown-string fallback and the auto rule. */
static enc_kind resolve(const odin3_fsm_mod *fm, const odin3_fsm_inst *inst, const char *label) {
    int kind = kind_of(inst->encoding);
    odin3_log(ODIN3_LOG_INFO, "fsm: recoding '%s' using '%s' encoding", label, inst->encoding);
    if (kind < 0) {
        odin3_log(ODIN3_LOG_INFO, "fsm: '%s': unknown encoding '%s', using auto", label,
                  inst->encoding);
        kind = ENC_AUTO;
    }
    if (kind == ENC_NONE || kind == ENC_USER) {
        odin3_log(ODIN3_LOG_INFO, "fsm: '%s': nothing to do for encoding '%s'", label,
                  k_enc_names[kind]);
        return (enc_kind)kind;
    }
    if (kind == ENC_AUTO) {
        const char *want = fm->opts->encoding;
        if (want == NULL) {
            want = inst->data.states.len < (size_t)AUTO_ONE_HOT_LIMIT ? "one-hot" : "binary";
        }
        kind = kind_of(want);
        odin3_log(ODIN3_LOG_INFO, "fsm: '%s': mapping auto encoding to '%s'", label, want);
    }
    return (enc_kind)kind;
}

/* A code to make: its width, the state's index and the encoding. */
typedef struct code_req {
    int32_t bits, idx;
    enc_kind kind;
} code_req;

/* The new code of the state with index req.idx (R-2, R-3). */
static void make_code(uint8_t *code, code_req req) {
    for (int32_t k = 0; k < req.bits; k++) {
        if (req.kind == ENC_ONE_HOT) {
            code[k] = k == req.idx ? ODIN3_FSM_BIT_1 : ODIN3_FSM_BIT_DC;
        } else {
            code[k] = (uint8_t)(((uint32_t)req.idx >> k) & 1U);
        }
    }
}

/* Rewrites every state code; the new codes are built first, then swapped in. */
static odin3_status renumber(odin3_fsm_inst *inst, enc_kind kind, int32_t bits) {
    odin3_fsm_data *data = &inst->data;
    size_t count = data->states.len;
    uint8_t **fresh = (uint8_t **)data->states.data;
    uint8_t *codes = odin3_fsm_data_bits(data, count * (size_t)bits + 1);
    if (codes == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    int32_t counter = data->reset_state >= 0 ? 1 : 0;
    for (size_t i = 0; i < count; i++) {
        int32_t idx = data->reset_state == (int32_t)i ? 0 : counter++;
        make_code(codes + i * (size_t)bits, (code_req){bits, idx, kind});
        odin3_log(ODIN3_LOG_DEBUG, "fsm: state %zu -> index %d", i, idx);
    }
    for (size_t i = 0; i < count; i++) {
        fresh[i] = codes + i * (size_t)bits;
    }
    data->state_bits = bits;
    return ODIN3_OK;
}

odin3_status odin3_fsm_recode(const odin3_fsm_mod *fm, odin3_fsm_inst *inst) {
    char label[LABEL_MAX];
    odin3_fsm_wire_label(fm, inst->wire, label, sizeof label);
    enc_kind kind = resolve(fm, inst, label);
    set_final(inst, kind);
    if (kind == ENC_NONE || kind == ENC_USER) {
        return ODIN3_OK;
    }
    odin3_fsm_data *data = &inst->data;
    int32_t bits = kind == ENC_ONE_HOT ? (int32_t)data->states.len
                                       : odin3_fsm_ceil_log2((int32_t)data->states.len);
    if (kind == ENC_BINARY && bits == data->state_bits) {
        odin3_log(ODIN3_LOG_INFO,
                  "fsm: '%s': existing encoding is already a packed binary encoding", label);
        return ODIN3_OK;
    }
    odin3_status st = renumber(inst, kind, bits);
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO, "fsm: recoding '%s' with %s encoding (%zu states, %d bits)",
                  label, k_enc_names[kind], data->states.len, bits);
    }
    return st;
}
