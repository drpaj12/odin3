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

#include <stdbool.h>
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

/* The prefix of every preflight refusal and of the dialect's own lines (spec 3E §11), and the
 * writer's name for itself in the lines its VTR naming logs (`write_blif: --vtr: model …`). */
#define ODIN3_BLIF_VTR_WHO "write_blif: --vtr"
#define ODIN3_BLIF_VTR_PREFIX ODIN3_BLIF_VTR_WHO ": "

/* VB-10: appends the VPR-legal form of name to out (the identity for a legal name).
 *
 * Rules 1-3 of spec 3E §10, in one pass: a whole name that odin3_blif_vtr_reserved accepts gets a
 * `%` prefix; a leading `.` is written `%2E`; every byte outside 0x21-0x7E, and each of `=` `"`
 * `#` `\` `%`, is written `%XX` with two upper-case hex digits; every other byte is kept. `%`
 * followed by two upper-case hex digits is therefore always a byte and `%` followed by anything
 * else the reserved prefix, so the map is injective (`fe` -> `%fe`, the byte 0xFE -> `%FE`) and
 * decodable (plan 3E point (o)). A name containing `%` after the call is exactly a name that
 * changed. The exception of rule 3 for the generators `gnd`/`vcc`/`unconn` is the caller's.
 * ODIN3_ERR_NO_MEMORY leaves out as it was. */
odin3_status odin3_blif_vtr_legal_name(odin3_bytes name, odin3_strbuf *out);

/* VB-10 rule 3's reserved names: the whole name is one of `re fe ah al as NIL 0 1 2 3` (the
 * `.latch` tokens, R2) or `gnd vcc unconn` (the generators, VB-9), or it starts with `out:` (VPR's
 * outpad blocks, R12). Case-sensitive. */
bool odin3_blif_vtr_reserved(odin3_bytes name);

/* VB-10 rule 8, for a net used as a `.latch` control or a hard cell's clock input: appends legal
 * (a name odin3_blif_vtr_legal_name gave) to out with each `|` written `%7C` and the `^` of each
 * `_^_` written `%5E` (`x_^_y` -> `x_%5E_y`; overlapping `_^_^_` -> `_%5E_%5E_`), so VTR's
 * blackbox_latches.pl, which builds `latch|<type>|<clk>|<init>` and splits on `_^_`, reads the
 * clock whole. The result stays injective and decodable. ODIN3_ERR_NO_MEMORY leaves out as it
 * was. */
odin3_status odin3_blif_vtr_clock_name(odin3_bytes legal, odin3_strbuf *out);

/* Defined in writer.c, whose naming machinery it runs: names the design's top as the VTR dialect
 * writes it (VB-10: rules 1-3 and 8 before 1C's uniqueness fallback; spec 3E §10) without writing
 * anything, logs the warning `write_blif: --vtr: port '<p>' written as '<legal>' (VPR cannot read
 * '<p>')` once per port whose bit names changed, and, when N > 0 nets are written under a name
 * VB-10 changed, the info line `write_blif: --vtr: N names legalised (first: '<orig>' ->
 * '<legal>')` for the lowest such net ID; sets report->renamed = N. The top must exist (E13 is
 * the caller's). Returns ODIN3_OK; ODIN3_ERR_INVALID_ARG (logged) when two ports on different
 * nets end up with one BLIF name; ODIN3_ERR_NO_MEMORY. */
odin3_status odin3_blif_vtr_name_top(const odin3_design *design, const odin3_blif_write_opts *opts,
                                     odin3_blif_vtr_report *report);

/* The K the VTR dialect writes with: opts->lut_k, or ODIN3_BLIF_VTR_DEFAULT_K when it is 0. */
uint32_t odin3_blif_vtr_k(const odin3_blif_write_opts *opts);

#endif
