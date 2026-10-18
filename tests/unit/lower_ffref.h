/*
 * lower_ffref.h — the independent reference stepper of the word flip-flops (3B plan Task 6 step 1,
 * spec §14.1, §14.3): the simlib semantics of the eleven types written as C, sharing no code with
 * src/passes/lower. One call is one clock cycle of the 1E simulator: the inputs hold for the whole
 * cycle, an asynchronous control acts at once, the active edge comes once, and the state is read at
 * the end of the cycle. Test code.
 */
#ifndef ODIN3_TESTS_UNIT_LOWER_FFREF_H
#define ODIN3_TESTS_UNIT_LOWER_FFREF_H

#include <stdbool.h>
#include <stdint.h>

/* The widest word the stepper holds. */
enum { FFREF_MAX_WIDTH = 64 };

typedef enum ffref_kind {
    FFREF_DFF,
    FFREF_DFFE,
    FFREF_SDFF,
    FFREF_SDFFE,
    FFREF_SDFFCE,
    FFREF_ADFF,
    FFREF_ADFFE,
    FFREF_ALDFF,
    FFREF_ALDFFE,
    FFREF_DFFSR,
    FFREF_DFFSRE,
    FFREF_KIND_COUNT
} ffref_kind;

/* A cell: its type, width and the polarity of every control (true: active high). */
typedef struct ffref_cfg {
    ffref_kind kind;
    uint32_t width;
    bool en_high;
    bool ctl_high; /* ARST, SRST or ALOAD */
    bool set_high;
    bool clr_high;
    uint64_t value;    /* ARST_VALUE or SRST_VALUE */
    bool enable_first; /* the Parmys variant: the enable over a reset or load (spec §16 row 4) */
} ffref_cfg;

/* The inputs of one cycle (the raw levels of the nets; a polarity is the cell's, not applied). */
typedef struct ffref_in {
    uint64_t data;
    uint64_t load; /* AD */
    uint64_t set;
    uint64_t clr;
    bool en;
    bool ctl;
} ffref_in;

/* The type's name ("$dff", ...). */
const char *ffref_name(ffref_kind kind);

/* True for the types with an asynchronous control. */
bool ffref_is_async(ffref_kind kind);

/* True for the types with an enable. */
bool ffref_has_en(ffref_kind kind);

/* The state after one cycle with the inputs `in`, from `state` (bits above the width are 0). */
uint64_t ffref_step(const ffref_cfg *cfg, uint64_t state, const ffref_in *in);

#endif
