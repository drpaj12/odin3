/*
 * prov_extra.h — the post-Odin lines (A, Q, K) of a restored version 2 map, as the writer puts
 * them out again (V1.2.0 4.5): held in the design's extra table with the design's record ids, they
 * are written with the record numbers of this map, and with `stale=t<n>` in the last field when
 * the netlist was changed after the restore. Also the fresh objective-only Q line of a join made
 * after a restore (3.6). Internal to prov_export.c.
 */
#ifndef ODIN3_BACKENDS_COMMON_PROV_EXTRA_H
#define ODIN3_BACKENDS_COMMON_PROV_EXTRA_H

#include "backends/common/name_map.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* A stored extra line split at its TABs. refs: the index of the field that names records (a
 * comma list `r<id>`), stale: the index of the last field (`-` or `t<run>`). */
typedef struct odin3_xline {
    char type;
    uint32_t n;
    uint32_t refs, stale;
    odin3_bytes f[ODIN3_NM2_MAX_FIELDS];
} odin3_xline;

/*
 * The extra table also holds `X<TAB>r<id><TAB>flags` lines: the flags of the record `id` that
 * this version does not define (the site coordinates of odin3-prov-post, for one), kept as the file
 * had them and written again with the record. They are not post-Odin data and not roots. True and
 * *record, *flags (escaped, comma separated) for such a line.
 */
bool odin3_xline_flags(odin3_bytes line, uint32_t *record, odin3_bytes *flags);

/* Splits a stored line; false for a type other than A, Q, K or too few fields. */
bool odin3_xline_split(odin3_bytes line, odin3_xline *out);

/*
 * The design run (a pass-run row, 0 for none) that makes the line stale: its own stale field when
 * it names one (a line that was stale when written keeps its first mutating run), else the
 * design's mutating run, else 0.
 */
uint32_t odin3_xline_stale(const odin3_design *design, const odin3_xline *line);

/* Appends the design record ids the line names (A: its recs; Q and K: the id) to ids (uint32_t). */
odin3_status odin3_xline_records(const odin3_xline *line, odin3_vec *ids);

/* The numbers the writer assigned: record id -> r number, run -> t number. */
typedef struct odin3_xline_nums {
    const odin3_design *design;
    const odin3_u64map *num_of;
    const odin3_u64map *run_num;
} odin3_xline_nums;

/* Appends one stored line as an A, Q or K line of the map being written. */
odin3_status odin3_xline_emit(const odin3_xline_nums *nums, const odin3_xline *line,
                              odin3_strbuf *out);

/* Appends the objective-only Q line of the join record numbered rnum, made by the pass `pass`
 * (empty: no objective is stated, as for an abc script, Task 10). stale is `-`. */
odin3_status odin3_xline_emit_fresh_q(uint32_t rnum, odin3_bytes pass, odin3_strbuf *out);

#endif
