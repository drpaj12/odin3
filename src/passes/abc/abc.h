/*
 * abc.h — the abc pass's caps and exchange kinds (3D spec §7.8, AB-24; plan Task 11, point (m)).
 * The checker (script.c) reads the script caps; Task 14 adds the pass-level caps and the budget
 * term, and odin3_abc_test_set_limits (abc_test.h) to lower them in tests.
 */
#ifndef ODIN3_PASSES_ABC_ABC_H
#define ODIN3_PASSES_ABC_ABC_H

#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/sha256.h"

#include <stdbool.h>
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

/* The pass-level caps (§7.8, AB-24). */
enum {
    ODIN3_ABC_MAX_DOMAINS = 64,           /* clock domains per module */
    ODIN3_ABC_MAX_AIG_OBJS = 1 << 26,     /* objects of one run's AIG */
    ODIN3_ABC_NAMEMAP_MAX_OBJS = 1 << 23, /* name map skipped above this (in-memory only) */
    ODIN3_ABC_GATE_MAX_INPUTS = 6,        /* wider gate types are cut cells */
    ODIN3_ABC_BYTES_PER_AIG_OBJ = 512,    /* until bench_abc calibrates it (it only goes up) */
    ODIN3_ABC_PROV_EDGES_PER_OBJ = 16,    /* provenance edges per old cell and new object */
    ODIN3_ABC_PROV_EDGES_BASE = 65536     /* ... plus this, per run */
};

/*
 * The script caps the checker reads (abc_test.h's odin3_abc_test_set_limits lowers them in tests).
 * Each defaults to its ODIN3_ABC_MAX_* constant above.
 */
typedef struct odin3_abc_limits {
    uint32_t script_bytes;
    uint32_t line_bytes;
    uint32_t commands;
    uint32_t words;
    uint32_t alias_depth;
} odin3_abc_limits;

/*
 * The pass-level caps (abc_test.h's odin3_abc_test_set_run_limits lowers them in tests; a 0 field
 * keeps the default). prov_edges is a fixed provenance edge budget per run instead of the formula
 * of §7.8 (0: the formula).
 */
typedef struct odin3_abc_run_limits {
    uint32_t domains;
    uint32_t aig_objs;
    uint32_t namemap_objs;
    uint64_t prov_edges;
} odin3_abc_run_limits;

/* The pass-level caps in force (the defaults, or a test's lower ones). Never NULL. */
const odin3_abc_run_limits *odin3_abc_run_limits_get(void);

/* The subprocess path's limits (§7.9, AB-33; an agent default, 3D-8 calibrates the time). */
enum {
    ODIN3_ABC_CHILD_SECONDS = 3600,                 /* wall clock per child run */
    ODIN3_ABC_CHILD_OUTPUT_BYTES = 64 * 1024 * 1024 /* merged output per child run */
};

/* The child runner's limits (abc_test.h's odin3_abc_test_set_child_limits lowers them in tests). */
typedef struct odin3_abc_child_limits {
    uint32_t seconds;
    uint64_t output_bytes;
} odin3_abc_child_limits;

/* How the network reaches ABC (§6): in memory (MiniAIG in, MiniLUT out) or through BLIF files. */
enum odin3_abc_exchange { ODIN3_ABC_EXCHANGE_AIG = 0, ODIN3_ABC_EXCHANGE_BLIF = 1 };

/* Which engine runs a script (§7.9): the linked one in process, or the odin3-abc subprocess. */
typedef enum odin3_abc_route_kind {
    ODIN3_ABC_ROUTE_LINKED = 0,
    ODIN3_ABC_ROUTE_CHILD = 1
} odin3_abc_route_kind;

/*
 * The route of a script whose bytes have this SHA-256 (odin3_abc_script.sha256 as 32 bytes),
 * decided once per pass run after the checker: LINKED when the digest is the embedded default
 * script's (by content, so another path to the same bytes is still LINKED), CHILD for every other
 * script. odin3_abc_test_set_route (abc_test.h) overrides the answer in tests. Never fails, never
 * logs.
 */
odin3_abc_route_kind odin3_abc_route(const unsigned char sha256[ODIN3_SHA256_DIGEST_BYTES]);

/* --- The pass (§7.1, AB-19; Task 14) ------------------------------------------------------- */

typedef enum odin3_abc_prov_mode {
    ODIN3_ABC_PROV_FINE = 0,
    ODIN3_ABC_PROV_CONE = 1
} odin3_abc_prov_mode;

/* The pass's options; NULL or 0 means not given. Every string points into the arena given to
 * odin3_abc_parse_args. */
typedef struct odin3_abc_opts {
    const char *script;
    uint32_t lut_k;
    const char *via_blif;
    const char *log;
    uint8_t prov; /* an odin3_abc_prov_mode */
} odin3_abc_opts;

/*
 * Splits args with odin3_pass_arg_next (§7.1, AB-19); each option may appear once. Returns
 * ODIN3_ERR_INVALID_ARG after logging exactly one error: `abc: unknown argument '<arg>'`, `abc:
 * <option> given twice`, `abc: <option> needs a value`, `abc: --lut must be an integer from 2 to
 * 16, not '<v>'` or `abc: --prov must be fine or cone, not '<v>'`. ODIN3_ERR_NO_MEMORY (not
 * logged). *out is zeroed first and filled as far as the parse got.
 */
odin3_status odin3_abc_parse_args(odin3_bytes args, odin3_arena *arena, odin3_abc_opts *out);

/* Where the chosen script comes from (AB-20), in precedence order. */
typedef enum odin3_abc_script_source {
    ODIN3_ABC_SRC_PASS = 1,
    ODIN3_ABC_SRC_PROJECT,
    ODIN3_ABC_SRC_CLI,
    ODIN3_ABC_SRC_BUILTIN
} odin3_abc_script_source;

/* given: the path as given (what the run information records); resolved: the path against the
 * working directory, or "embedded" for the built-in script. */
typedef struct odin3_abc_script_choice {
    const char *given;
    const char *resolved;
    uint8_t source; /* an odin3_abc_script_source */
} odin3_abc_script_choice;

/*
 * AB-20, in order: `--script` (resolved against the working directory); the project record's
 * abc_script (given as written, resolved as the reader resolved it); the pass option
 * (odin3_pass_get_options().abc_script, resolved against the working directory);
 * ODIN3_ABC_DEFAULT_SCRIPT_NAME, resolved "embedded". The strings live in arena. Does not read
 * the file. ODIN3_ERR_NO_MEMORY (not logged).
 */
odin3_status odin3_abc_choose_script(const odin3_design *design, const odin3_abc_opts *opts,
                                     odin3_arena *arena, odin3_abc_script_choice *out);

/*
 * The exchange of this invocation (§6; point (z)): BLIF when opts->via_blif is given; else the
 * environment variable ODIN3_ABC_EXCHANGE (`aig` or `blif`); else BLIF (the default until Task
 * 21's flip). Any other value of the variable is ODIN3_ERR_INVALID_ARG after logging `abc:
 * ODIN3_ABC_EXCHANGE must be aig or blif, not '<v>'`. *exchange is an odin3_abc_exchange.
 */
odin3_status odin3_abc_exchange_choice(const odin3_abc_opts *opts, uint32_t *exchange);

/*
 * The pass body (run.c; defined as odin3_pass_abc, point (n): wants_ast false, processes REFUSE).
 * Start-up order, before any IR change or engine call: arguments; the --via-blif path bytes
 * (I12); the script choice and odin3_abc_script_parse; the exchange; the engine; the views; the
 * route; --log opened; the first INFO line. Then one run per module and clock domain.
 */
odin3_status odin3_abc_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                void *user);

/*
 * The 32-byte digest of a 64-digit lower-case hex string (odin3_abc_script.sha256_hex), the form
 * odin3_abc_route takes; false (digest untouched) for any other string. 3D-5a's hand-over.
 */
bool odin3_abc_hex_digest(const char *hex, unsigned char digest[ODIN3_SHA256_DIGEST_BYTES]);

#endif
