/*
 * attrs.h — how the JSON and Verilog writers name and spell IR attributes (IR-10).
 */
#ifndef ODIN3_BACKENDS_COMMON_ATTRS_H
#define ODIN3_BACKENDS_COMMON_ATTRS_H

#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* Where an attribute goes. */
typedef enum odin3_wattr_role {
    ODIN3_WATTR_SKIP,      /* bookkeeping (the BLIF reader's blif_extras list): not written */
    ODIN3_WATTR_ATTRIBUTE, /* an attribute */
    ODIN3_WATTR_PARAMETER, /* a cell parameter (a BLIF `.param` line) */
} odin3_wattr_role;

/* How its value is spelled. */
typedef enum odin3_wattr_form {
    ODIN3_WATTR_VALUE,  /* the IR value as it is */
    ODIN3_WATTR_TEXT,   /* a string: `text` */
    ODIN3_WATTR_BINARY, /* a bit vector: `text` holds its digits MSB first; any byte but '0' is 1 */
} odin3_wattr_form;

typedef struct odin3_wattr {
    odin3_wattr_role role;
    odin3_wattr_form form;
    odin3_bytes key;          /* the name to write */
    odin3_bytes text;         /* TEXT and BINARY */
    const odin3_value *value; /* VALUE */
} odin3_wattr;

/*
 * Classifies attribute key_str of an object. The BLIF reader's keys are mapped back to what the
 * BLIF lines meant (frontends/blif/attrs.h): `blif.attr:K` is attribute K and `blif.param:K`
 * parameter K, their STRING value spelled as Yosys read_blif reads it (a value starting with `"`
 * is the string between the quotes, anything else a binary bit vector); `blif_extras` is skipped.
 * Every other key is an attribute with its IR value. The bytes point into tab.
 */
odin3_wattr odin3_wattr_classify(const odin3_strtab *tab, uint32_t key_str,
                                 const odin3_value *value);

/* The (role, key) pairs already written for one object, so a later duplicate is skipped. */
typedef struct odin3_wattr_seen {
    odin3_vec entries;
} odin3_wattr_seen;

void odin3_wattr_seen_init(odin3_wattr_seen *seen);
void odin3_wattr_seen_free(odin3_wattr_seen *seen);
void odin3_wattr_seen_clear(odin3_wattr_seen *seen);

/* The key to claim, and whether it is new for this object. */
typedef struct odin3_wattr_claim_req {
    odin3_wattr_role role;
    odin3_bytes key;
} odin3_wattr_claim_req;

/*
 * *fresh = true and records the pair unless it is already recorded (then *fresh = false).
 * ODIN3_ERR_NO_MEMORY leaves seen unchanged.
 */
odin3_status odin3_wattr_claim(odin3_wattr_seen *seen, odin3_wattr_claim_req req, bool *fresh);

#endif
