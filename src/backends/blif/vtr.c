/* vtr.c — the VTR dialect's preflight (spec 3E VB-11), resolved options (VB-4) and legal names
 * (VB-10). */
#include "backends/blif/vtr.h"

#include "ir/ids.h"
#include "ir/module.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define VTR_PREFIX ODIN3_BLIF_VTR_PREFIX

enum {
    LEGAL_FIRST = 0x21, /* VB-10 rule 1: the printable bytes, blank excluded */
    LEGAL_LAST = 0x7E,
};

/* The printable bytes VB-10 rule 1 still escapes: VPR's ID_SET excludes the first four (R1; `#`
 * for ABC), and `%` is the escape. */
static const char ESCAPED[] = "=\"#\\%";

/* VB-10 rule 3's whole names, and the prefix of VPR's outpad blocks. */
static const char *const RESERVED[] = {"re", "fe", "ah", "al",  "as",  "NIL",   "0",
                                       "1",  "2",  "3",  "gnd", "vcc", "unconn"};
static const char OUTPAD_PREFIX[] = "out:";

/* Truncates out back to len, undoing a failed sequence of appends (the documented "unchanged"). */
static void truncate_to(odin3_strbuf *out, size_t len) {
    if (out->data != NULL) {
        out->len = len;
        out->data[len] = '\0';
    }
}

static bool byte_legal(unsigned char byte) {
    return byte >= LEGAL_FIRST && byte <= LEGAL_LAST &&
           memchr(ESCAPED, byte, sizeof ESCAPED - 1) == NULL;
}

bool odin3_blif_vtr_reserved(odin3_bytes name) {
    size_t prefix = sizeof OUTPAD_PREFIX - 1;
    if (name.len >= prefix && memcmp(name.ptr, OUTPAD_PREFIX, prefix) == 0) {
        return true;
    }
    for (size_t i = 0; i < sizeof RESERVED / sizeof RESERVED[0]; i++) {
        if (name.len == strlen(RESERVED[i]) && memcmp(name.ptr, RESERVED[i], name.len) == 0) {
            return true;
        }
    }
    return false;
}

odin3_status odin3_blif_vtr_legal_name(odin3_bytes name, odin3_strbuf *out) {
    size_t start = out->len;
    const unsigned char *bytes = name.ptr;
    odin3_status st =
        odin3_blif_vtr_reserved(name) ? odin3_strbuf_append(out, odin3_bytes_cstr("%")) : ODIN3_OK;
    size_t run = 0; /* start of the pending run of kept bytes */
    for (size_t i = 0; st == ODIN3_OK && i < name.len; i++) {
        if (byte_legal(bytes[i]) && !(i == 0 && bytes[i] == '.')) {
            continue;
        }
        st = odin3_strbuf_append(out, (odin3_bytes){bytes + run, i - run});
        if (st == ODIN3_OK) {
            st = odin3_strbuf_appendf(out, "%%%02X", (unsigned)bytes[i]);
        }
        run = i + 1;
    }
    if (st == ODIN3_OK) {
        st = odin3_strbuf_append(out, (odin3_bytes){bytes + run, name.len - run});
    }
    if (st != ODIN3_OK) {
        truncate_to(out, start);
    }
    return st;
}

/* True when byte `at` of text is the `^` of a `_^_`. */
static bool clock_caret(const unsigned char *text, size_t len, size_t at) {
    return text[at] == '^' && at > 0 && at + 1 < len && text[at - 1] == '_' && text[at + 1] == '_';
}

odin3_status odin3_blif_vtr_clock_name(odin3_bytes legal, odin3_strbuf *out) {
    size_t start = out->len;
    const unsigned char *bytes = legal.ptr;
    odin3_status st = ODIN3_OK;
    size_t run = 0;
    for (size_t i = 0; st == ODIN3_OK && i < legal.len; i++) {
        if (bytes[i] != '|' && !clock_caret(bytes, legal.len, i)) {
            continue;
        }
        st = odin3_strbuf_append(out, (odin3_bytes){bytes + run, i - run});
        if (st == ODIN3_OK) {
            st = odin3_strbuf_appendf(out, "%%%02X", (unsigned)bytes[i]);
        }
        run = i + 1;
    }
    if (st == ODIN3_OK) {
        st = odin3_strbuf_append(out, (odin3_bytes){bytes + run, legal.len - run});
    }
    if (st != ODIN3_OK) {
        truncate_to(out, start);
    }
    return st;
}

uint32_t odin3_blif_vtr_k(const odin3_blif_write_opts *opts) {
    return opts->lut_k == 0 ? ODIN3_BLIF_VTR_DEFAULT_K : opts->lut_k;
}

/* The options' own checks, for a direct caller of the preflight: through odin3_blif_write_with
 * they never fire, since its check_opts refuses the same options first. */
static odin3_status check_opts(const odin3_blif_write_opts *opts) {
    uint32_t k = odin3_blif_vtr_k(opts);
    if (opts->dialect != ODIN3_BLIF_DIALECT_VTR) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_blif_vtr_preflight: opts are not the VTR dialect");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (k < ODIN3_BLIF_VTR_MIN_K || k > ODIN3_BLIF_VTR_MAX_K) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_blif_vtr_preflight: lut_k %u is outside %u..%u",
                  (unsigned)k, ODIN3_BLIF_VTR_MIN_K, ODIN3_BLIF_VTR_MAX_K);
        return ODIN3_ERR_INVALID_ARG;
    }
    if (opts->name_style != ODIN3_BLIF_NAMES_SHORT &&
        opts->name_style != ODIN3_BLIF_NAMES_PROVENANCE) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_blif_vtr_preflight: name_style %d is unknown",
                  (int)opts->name_style);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

odin3_status odin3_blif_vtr_preflight(const odin3_design *design, const odin3_blif_write_opts *opts,
                                      odin3_blif_vtr_report *report) {
    if (design == NULL || opts == NULL || report == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_blif_vtr_preflight: NULL design, opts or report");
        return ODIN3_ERR_INVALID_ARG;
    }
    *report = (odin3_blif_vtr_report){0};
    odin3_status st = check_opts(opts);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_module_id top = odin3_design_top(design);
    if (!odin3_module_valid(top)) { /* E13, checked first and alone (plan 3E point (f)) */
        odin3_log(ODIN3_LOG_ERROR,
                  VTR_PREFIX "no top module (read_blif sets one; or run hierarchy)");
        report->errors = 1;
        return ODIN3_ERR_INVALID_ARG;
    }
    /* module IDs run from 1 to odin3_design_module_end - 1; all but the top are skipped */
    report->modules_skipped = odin3_design_module_end(design) - 2;
    st = odin3_blif_vtr_name_top(design, opts, report); /* VB-10: warnings, info, renamed */
    if (st == ODIN3_ERR_INVALID_ARG) {
        report->errors++;
    }
    return st;
}
