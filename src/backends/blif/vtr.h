/*
 * vtr.h — the VTR dialect of the BLIF writer (spec 3E): the preflight that refuses, before any byte
 * is written, every construct VPR would reject or misread, and VB-10's legal names.
 */
#ifndef ODIN3_BACKENDS_BLIF_VTR_H
#define ODIN3_BACKENDS_BLIF_VTR_H

#include "backends/blif/writer.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/str.h"

#include <stdint.h>

/* What the VTR preflight found (spec 3E VB-11); the counts feed the info lines and the tests. */
typedef struct odin3_blif_vtr_report {
    uint32_t errors;                  /* refusals; > 0 means nothing may be written */
    uint32_t names, latches, subckts; /* cells the write will emit */
    uint32_t x_to_gnd;        /* x/undriven bits on .names/.latch/output pins written as gnd */
    uint32_t x_to_unconn;     /* x/undriven/unconnected hard-block input bits written as unconn */
    uint32_t unconn_logic;    /* .names/.latch input pins on a net named 'unconn' (VB-2a) */
    uint32_t renamed;         /* names legalised by VB-10 */
    uint32_t modules_skipped; /* modules other than the top, not written */
} odin3_blif_vtr_report;

/* Checks that the design's top can be written in the VTR dialect with opts (lut_k resolved).
 * Logs each refusal (VB-11, at most ODIN3_BLIF_VTR_MAX_ERRORS, then one count line) and fills
 * report. Returns ODIN3_OK when report->errors == 0, ODIN3_ERR_INVALID_ARG when it is not or an
 * argument is NULL, ODIN3_ERR_NO_MEMORY. Reads only; O(nodes + pins). */
odin3_status odin3_blif_vtr_preflight(const odin3_design *design, const odin3_blif_write_opts *opts,
                                      odin3_blif_vtr_report *report);

/* VB-10: appends the VPR-legal form of name to out (the identity for a legal name). */
odin3_status odin3_blif_vtr_legal_name(odin3_bytes name, odin3_strbuf *out);

/* The K the VTR dialect writes with: opts->lut_k, or ODIN3_BLIF_VTR_DEFAULT_K when it is 0. */
uint32_t odin3_blif_vtr_k(const odin3_blif_write_opts *opts);

#endif
