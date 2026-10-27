/*
 * script.h — the ABC script checker (3D spec §7.3, AB-21) and its data (§7.4, §7.5): the alias
 * table copied from the pinned ABC's abc.rc, the allow-list of ABC commands with the class, the
 * effect on the two network spaces and the file, string and crash options of each, and the
 * embedded default script (§7.7, AB-23). Pure C: nothing here includes or links ABC.
 */
#ifndef ODIN3_PASSES_ABC_SCRIPT_H
#define ODIN3_PASSES_ABC_SCRIPT_H

#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One `alias` line of abc.rc: `alias <name> <expansion>`, the expansion's words joined by one
 * blank (a quoted expansion is kept as written between its quotes). */
typedef struct odin3_abc_alias {
    const char *name;
    const char *expansion;
} odin3_abc_alias;

/*
 * The alias called name, or NULL. Looks in the table set by odin3_abc_test_set_aliases
 * (abc_test.h) while one is set, else in k_abc_aliases: every `alias` line of
 * third_party/abc/abc.rc at the pin (a3001b72), in file order. Never allocates.
 */
const odin3_abc_alias *odin3_abc_alias_find(const char *name);

/* The number of aliases in the table odin3_abc_alias_find looks in. */
uint32_t odin3_abc_alias_count(void);

/* The alias at index in that table (file order), or NULL at or past odin3_abc_alias_count(). */
const odin3_abc_alias *odin3_abc_alias_at(uint32_t index);

/* The bytes of the objective word of a checked script, NUL included (see odin3_abc_objective). */
enum { ODIN3_ABC_OBJECTIVE_BYTES = 32 };

/* A command's class (§7.5): the four allowed P3-11 groups, then the refused ones. */
typedef enum odin3_abc_class {
    ODIN3_ABC_CLASS_OTHER = 0, /* not allowed, no specific reason (§7.3 step 8's last ending) */
    ODIN3_ABC_CLASS_OPT,       /* optimisation, in the ntk space */
    ODIN3_ABC_CLASS_MAP,       /* LUT mapping, in the ntk space */
    ODIN3_ABC_CLASS_GIA,       /* the & space (and &get/&put between the spaces) */
    ODIN3_ABC_CLASS_PRINT,     /* printing: output logged at INFO */
    ODIN3_ABC_CLASS_FILES,     /* refused: reads or writes files */
    ODIN3_ABC_CLASS_PROCESS,   /* refused: controls the ABC process */
    ODIN3_ABC_CLASS_LATCHES,   /* refused: retimes or changes initial values (PHASE3 #12) */
    ODIN3_ABC_CLASS_CRASH      /* refused: crashes ABC at the pin (Task 10 review erratum) */
} odin3_abc_class;

/* A command's effect on the spaces (§7.5's "Effect" column, in row order; §7.3 step 11). */
typedef enum odin3_abc_effect {
    ODIN3_ABC_FX_NONE = 0,       /* printing, and every refused row */
    ODIN3_ABC_FX_NTK_AIG,        /* ntk := aig */
    ODIN3_ABC_FX_NTK_KEEP,       /* ntk keeps its state */
    ODIN3_ABC_FX_NTK_MAPPED,     /* ntk := mapped */
    ODIN3_ABC_FX_GIA_GET,        /* &get: gia := ntk's state with -m, else aig */
    ODIN3_ABC_FX_NTK_PUT,        /* &put: ntk := gia's state */
    ODIN3_ABC_FX_GIA_AIG,        /* gia := aig */
    ODIN3_ABC_FX_GIA_MAPPED,     /* gia := mapped */
    ODIN3_ABC_FX_GIA_KEEP,       /* gia keeps its state */
    ODIN3_ABC_FX_NTK_CELLMAPPED, /* ntk := cell-mapped (map: a gate library mapping) */
    ODIN3_ABC_FX_GIA_CELLMAPPED  /* gia := cell-mapped (&nf) */
} odin3_abc_effect;

/*
 * One row of the allow-list table. cls is an odin3_abc_class and effect an odin3_abc_effect.
 * file_opts lists the option letters that read or write a file, or that take a string, read at
 * the pin from the command's Extra_UtilGetopt handling, usage text and the code each option
 * enables ("" for none); §7.3 step 9 refuses them, flags included, even with a numeric value.
 * crash_opts lists the option letters with which the command crashes ABC at the pin (a SIGSEGV,
 * which kills the whole process); step 9 refuses them too ("" for none). A refused row has effect
 * NONE, file_opts "" and crash_opts "".
 */
typedef struct odin3_abc_command_info {
    const char *name;
    uint8_t cls;
    uint8_t effect;
    const char *file_opts;
    const char *crash_opts;
} odin3_abc_command_info;

/*
 * The row of the ABC command called name (an expanded word 0, never an alias): an allowed row
 * (class OPT, MAP, GIA or PRINT), a refused row (class FILES, PROCESS, LATCHES or CRASH), or NULL
 * for "other" — an unlisted name, or a row of class OTHER (the table lists the commands that stop a
 * refused abc.rc alias, §7.4, so that they are verified at the pin). Never allocates.
 */
const odin3_abc_command_info *odin3_abc_command_find(const char *name);

/* The number of rows in the table, class OTHER included. */
uint32_t odin3_abc_command_count(void);

/* The row at index (§7.5's order), or NULL at or past odin3_abc_command_count(). */
const odin3_abc_command_info *odin3_abc_command_at(uint32_t index);

/* --- The checker (§7.3, AB-21) --------------------------------------------------------------- */

/*
 * Where a script comes from. given is the path as given (the CLI or project word), or
 * ODIN3_ABC_DEFAULT_SCRIPT_NAME; every message names it. When text.ptr is NULL the checker reads
 * the file at resolved; otherwise text holds the bytes (resolved may then be NULL).
 */
typedef struct odin3_abc_script_src {
    const char *given;
    const char *resolved;
    odin3_bytes text;
} odin3_abc_script_src;

/* The network space a script ends in (§7.3 step 11): ABC's Abc_Ntk space or its & (Gia) space. */
typedef enum odin3_abc_space { ODIN3_ABC_SPACE_NTK = 0, ODIN3_ABC_SPACE_GIA = 1 } odin3_abc_space;

/*
 * One checked command. line holds its words after alias expansion and {K}, joined by one blank
 * and NUL-terminated, so ABC's CmdSplitLine splits it into exactly those words. src_line is the
 * script line it came from (1-based). alias is the outermost alias that produced it, or NULL.
 */
typedef struct odin3_abc_cmd {
    const char *line;
    uint32_t src_line;
    const char *alias;
} odin3_abc_cmd;

/*
 * A checked script: cmds[0..n_cmds) in run order, uses_k (the script holds {K}), final_space (an
 * odin3_abc_space, mapped by construction) and the SHA-256 of the bytes read, before any
 * expansion, as 64 lower-case hex digits. Every string lives in arena, which the script owns.
 */
typedef struct odin3_abc_script {
    const odin3_abc_cmd *cmds;
    uint32_t n_cmds;
    bool uses_k;
    uint8_t final_space;
    bool objective_stated; /* objective was a `# objective: <word>` comment, not the rule */
    char objective[ODIN3_ABC_OBJECTIVE_BYTES]; /* "depth", "area", a stated word, or "" */
    char sha256_hex[ODIN3_SHA256_HEX_CHARS];   /* 65: 64 digits and the NUL */
    odin3_arena *arena;
} odin3_abc_script;

/*
 * Checks a script (AB-21's twelve steps, in order: one pass over the bytes, then one over the
 * expanded commands) and, when it passes, fills *out. lut_k replaces {K} (0: no --lut; else from
 * ODIN3_ABC_LUT_MIN to ODIN3_LUT_MAX_WIDTH); exchange is an odin3_abc_exchange (abc.h).
 * Returns ODIN3_OK; ODIN3_ERR_PARSE after logging exactly one error, `abc: <given>:<line>: …` (or
 * `abc: <given>: …` for the size and the unmapped end); ODIN3_ERR_IO (logged as `abc:
 * <given>: cannot read the script: <strerror>`, the file as given) when it cannot be read;
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL src, given or out, no text and no path, or a bad
 * lut_k or exchange; ODIN3_ERR_NO_MEMORY (not logged). *out is untouched on failure, and the
 * caller frees a filled one with odin3_abc_script_free. Never includes or calls ABC.
 */
odin3_status odin3_abc_script_parse(const odin3_abc_script_src *src, uint32_t lut_k,
                                    uint32_t exchange, odin3_abc_script *out);

/*
 * How a script is checked (gatemap spec 5): lut_k and exchange as for odin3_abc_script_parse, and
 * gates, true for abc --gates. With gates the script must end cell-mapped (`map` or `&nf`) and a
 * LUT ending is refused; without it `map` and `&nf` are refused (no gate library is loaded).
 */
typedef struct odin3_abc_script_mode {
    uint32_t lut_k;
    uint32_t exchange;
    bool gates;
} odin3_abc_script_mode;

/* odin3_abc_script_parse with a mode; the other function is this one with gates false. */
odin3_status odin3_abc_script_parse_mode(const odin3_abc_script_src *src,
                                         const odin3_abc_script_mode *mode, odin3_abc_script *out);

/* Frees what odin3_abc_script_parse filled and zeroes *script. NULL and a zeroed script are
 * no-ops. */
void odin3_abc_script_free(odin3_abc_script *script);

/*
 * The objective of a script, for the provenance map (V1.2.0 3.6). out gets, as a NUL-terminated
 * word, the first `# objective: <word>` comment of text (*stated true), else `area` or `depth`
 * from the last mapping command of cmds (`if`, `&if`, `map`, `&nf`; `area` when its `a` flags
 * are odd in number), else "" (a script that states none). Pure; never allocates.
 */
void odin3_abc_objective(odin3_bytes text, const odin3_abc_cmd *cmds, uint32_t n_cmds,
                         char out[ODIN3_ABC_OBJECTIVE_BYTES], bool *stated);

/* The command at index, or NULL for a NULL script or an index at or past n_cmds. */
const odin3_abc_cmd *odin3_abc_script_cmd(const odin3_abc_script *script, uint32_t index);

/* --- The default script (§7.7, AB-23) ------------------------------------------------------- */

/* The name messages and the run information give the embedded default script. */
#define ODIN3_ABC_DEFAULT_SCRIPT_NAME "builtin:lib/abc/vtr.abc"

/* lib/abc/vtr.abc's bytes, embedded at build time (generated/abc/default_script.c), and their
 * number (no NUL is counted or relied on). */
extern const unsigned char odin3_abc_default_script[];
extern const uint32_t odin3_abc_default_script_len;

/* The name messages and the run information give lib/abc/gates.abc, the default script of
 * abc --gates (gatemap spec G-5), embedded the same way (generated/abc/gates_script.c). */
#define ODIN3_ABC_GATES_SCRIPT_NAME "builtin:lib/abc/gates.abc"
extern const unsigned char odin3_abc_gates_script[];
extern const uint32_t odin3_abc_gates_script_len;

/* The SHA-256 of the embedded default script's bytes (the route's identity, §7.9). Never fails. */
void odin3_abc_default_sha256(uint8_t digest[ODIN3_SHA256_DIGEST_BYTES]);

#endif
