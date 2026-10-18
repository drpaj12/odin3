/*
 * lower_latchref.h — the independent reference of the word latches (3B plan Task 7 step 1, spec
 * §14.1, §14.3): the simlib semantics of $dlatch $adlatch $dlatchsr $sr written as C, sharing no
 * code with src/passes/lower. One call is one cycle of the 1E simulator: the inputs hold for the
 * whole cycle, a latch is transparent while its enable is active, and the state is read at the end
 * of the cycle. Test code.
 */
#ifndef ODIN3_TESTS_UNIT_LOWER_LATCHREF_H
#define ODIN3_TESTS_UNIT_LOWER_LATCHREF_H

#include <stdbool.h>
#include <stdint.h>

/* The widest word the reference holds. */
enum { LTREF_MAX_WIDTH = 64 };

typedef enum ltref_kind {
    LTREF_DLATCH,
    LTREF_ADLATCH,
    LTREF_DLATCHSR,
    LTREF_SR,
    LTREF_KIND_COUNT
} ltref_kind;

/* A cell: its type, width and the polarity of every control (true: active high). */
typedef struct ltref_cfg {
    ltref_kind kind;
    uint32_t width;
    bool en_high;
    bool arst_high;
    bool set_high;
    bool clr_high;
    uint64_t value; /* ARST_VALUE */
} ltref_cfg;

/* The inputs of one cycle (the raw levels of the nets; a polarity is the cell's, not applied). */
typedef struct ltref_in {
    uint64_t data;
    uint64_t set;
    uint64_t clr;
    bool en;
    bool arst;
} ltref_in;

/* The type's name ("$dlatch", ...). */
const char *ltref_name(ltref_kind kind);

/* True for the types with an enable (all but $sr), with ARST, and with SET and CLR. */
bool ltref_has_en(ltref_kind kind);
bool ltref_has_arst(ltref_kind kind);
bool ltref_has_setclr(ltref_kind kind);

/* The state after one cycle with the inputs `in`, from `state` (bits above the width are 0). */
uint64_t ltref_step(const ltref_cfg *cfg, uint64_t state, const ltref_in *in);

#endif
