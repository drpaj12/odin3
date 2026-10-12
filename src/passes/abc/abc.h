/*
 * abc.h — the abc pass's caps and exchange kinds (3D spec §7.8, AB-24; plan Task 11, point (m)).
 * The checker (script.c) reads the script caps; Task 14 adds the pass-level caps and the budget
 * term, and odin3_abc_test_set_limits (abc_test.h) to lower them in tests.
 */
#ifndef ODIN3_PASSES_ABC_ABC_H
#define ODIN3_PASSES_ABC_ABC_H

#include <stdint.h>

/* The script caps (P3-1, AB-24): reaching one is a script error (ODIN3_ERR_PARSE). */
enum {
    ODIN3_ABC_MAX_SCRIPT_BYTES = 65536, /* the whole script */
    ODIN3_ABC_MAX_LINE_BYTES = 4096,    /* one line, without its LF or CRLF */
    ODIN3_ABC_MAX_COMMANDS = 1024,      /* commands after alias expansion */
    ODIN3_ABC_MAX_WORDS = 64,           /* words of one command, before alias expansion */
    ODIN3_ABC_MAX_ALIAS_DEPTH = 8,      /* nested alias expansions (abc.rc's deepest is 3) */
    ODIN3_ABC_MAX_NUMBER = 1000000,     /* every number argument lies in 0 .. this (Task 11 fix
                                           round 1, ruling C2: large values crash ABC, and atoi
                                           wraps) */
    ODIN3_ABC_LUT_MIN = 2,              /* the smallest --lut */
    ODIN3_ABC_MAX_LUT_LINKED = 12       /* the widest LUT a script may map with linked ABC: wider
                                           mappings crash &st and &synch2 at the pin (Dsm_Man
                                           TruthToGia, dauGia.c:451; final review C1), so {K} and
                                           -K stay at or below it although --lut allows 16 */
};

/*
 * The script caps the checker reads (abc_test.h's odin3_abc_test_set_limits lowers them in tests;
 * Task 14 adds the pass-level caps). Each defaults to its ODIN3_ABC_MAX_* constant above.
 */
typedef struct odin3_abc_limits {
    uint32_t script_bytes;
    uint32_t line_bytes;
    uint32_t commands;
    uint32_t words;
    uint32_t alias_depth;
} odin3_abc_limits;

/* How the network reaches ABC (§6): in memory (MiniAIG in, MiniLUT out) or through BLIF files. */
enum odin3_abc_exchange { ODIN3_ABC_EXCHANGE_AIG = 0, ODIN3_ABC_EXCHANGE_BLIF = 1 };

#endif
