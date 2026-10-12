/*
 * script.h — the ABC script checker's data (3D spec §7.4, §7.5): the alias table copied from the
 * pinned ABC's abc.rc, and the allow-list of ABC commands with the class, the effect on the two
 * network spaces and the file or string options of each. Task 11 adds the parser's declarations.
 * Pure C: nothing here includes or links ABC.
 */
#ifndef ODIN3_PASSES_ABC_SCRIPT_H
#define ODIN3_PASSES_ABC_SCRIPT_H

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

/* A command's class (§7.5): the four allowed P3-11 groups, then the refused ones. */
typedef enum odin3_abc_class {
    ODIN3_ABC_CLASS_OTHER = 0, /* not allowed, no specific reason (§7.3 step 8's last ending) */
    ODIN3_ABC_CLASS_OPT,       /* optimisation, in the ntk space */
    ODIN3_ABC_CLASS_MAP,       /* LUT mapping, in the ntk space */
    ODIN3_ABC_CLASS_GIA,       /* the & space (and &get/&put between the spaces) */
    ODIN3_ABC_CLASS_PRINT,     /* printing: output logged at INFO */
    ODIN3_ABC_CLASS_FILES,     /* refused: reads or writes files */
    ODIN3_ABC_CLASS_PROCESS,   /* refused: controls the ABC process */
    ODIN3_ABC_CLASS_LATCHES    /* refused: retimes or changes initial values (PHASE3 #12) */
} odin3_abc_class;

/* A command's effect on the spaces (§7.5's "Effect" column, in row order; §7.3 step 11). */
typedef enum odin3_abc_effect {
    ODIN3_ABC_FX_NONE = 0,   /* printing, and every refused row */
    ODIN3_ABC_FX_NTK_AIG,    /* ntk := aig */
    ODIN3_ABC_FX_NTK_KEEP,   /* ntk keeps its state */
    ODIN3_ABC_FX_NTK_MAPPED, /* ntk := mapped */
    ODIN3_ABC_FX_GIA_GET,    /* &get: gia := ntk's state with -m, else aig */
    ODIN3_ABC_FX_NTK_PUT,    /* &put: ntk := gia's state */
    ODIN3_ABC_FX_GIA_AIG,    /* gia := aig */
    ODIN3_ABC_FX_GIA_MAPPED, /* gia := mapped */
    ODIN3_ABC_FX_GIA_KEEP    /* gia keeps its state */
} odin3_abc_effect;

/*
 * One row of the allow-list table. cls is an odin3_abc_class and effect an odin3_abc_effect.
 * file_opts lists the option letters whose value is a file or a string, read at the pin from the
 * command's Extra_UtilGetopt handling and usage text in base/abci/abc.c ("" for none); §7.3 step
 * 9 refuses them even with a numeric value. A refused row has effect NONE and file_opts "".
 */
typedef struct odin3_abc_command_info {
    const char *name;
    uint8_t cls;
    uint8_t effect;
    const char *file_opts;
} odin3_abc_command_info;

/*
 * The row of the ABC command called name (an expanded word 0, never an alias): an allowed row
 * (class OPT, MAP, GIA or PRINT), a refused row (class FILES, PROCESS or LATCHES), or NULL for
 * "other" — an unlisted name, or a row of class OTHER (the table lists the commands that stop a
 * refused abc.rc alias, §7.4, so that they are verified at the pin). Never allocates.
 */
const odin3_abc_command_info *odin3_abc_command_find(const char *name);

/* The number of rows in the table, class OTHER included. */
uint32_t odin3_abc_command_count(void);

/* The row at index (§7.5's order), or NULL at or past odin3_abc_command_count(). */
const odin3_abc_command_info *odin3_abc_command_at(uint32_t index);

#endif
