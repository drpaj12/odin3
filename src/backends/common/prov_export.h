/*
 * prov_export.h — what a provenance record is, for export and for the ABI (V1.2.0 3.3): its class
 * labels, its construct, and the joins it makes. One definition, shared by the ABI calls
 * (odin3_prov_get_info and friends) and the map writers (Task 5), so no consumer recomputes it.
 */
#ifndef ODIN3_BACKENDS_COMMON_PROV_EXPORT_H
#define ODIN3_BACKENDS_COMMON_PROV_EXPORT_H

#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/vec.h"

#include <stdint.h>

/* The most rows of J a record lists (3.3, "one cap rule"); n_joined is still the total. */
enum { ODIN3_PROV_JOIN_CAP = 16 };

/* The labels of one record. joined: every row of J (uint32_t), in the key order of 3.3. */
typedef struct odin3_prov_label {
    odin3_prov_info info;
    odin3_vec joined;
} odin3_prov_label;

/* Empties label (no allocation); odin3_prov_label_free releases what compute allocated. */
void odin3_prov_label_init(odin3_prov_label *label);
void odin3_prov_label_free(odin3_prov_label *label);

/*
 * Fills label for record: its fields, and for a coarsen record the join (3.3: J reduced to
 * anchors of level stmt or coarser, a join when two or more are distinct and no single data
 * parent has the same set; a join of instances when one anchor is reached through two context
 * chains) and the rows of J. Walks the record graph with an explicit worklist; the context
 * parents (the declared tail) are not entered. INVALID_ARG (logged) for record 0 or one past the
 * last; NO_MEMORY. label is unchanged on failure.
 */
odin3_status odin3_prov_label_compute(const odin3_design *design, uint32_t record,
                                      odin3_prov_label *label);

#endif
