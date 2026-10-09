/*
 * reader.h — the .o3lib tech-library reader (docs/specs/2026-10-08-1G-techlib-design.md) and the
 * library data it attaches to every cell type it registers.
 *
 * Syntax: one statement per line or several separated by ';'; '#' starts a comment. The file opens
 * with `library NAME`; each cell is
 *
 *   cell NAME gate|hard|blackbox [area NUM] [delay NUM]
 *     param NAME int DEFAULT             # DEFAULT: a constant integer expression
 *     in|out|inout NAME WIDTH [signed] [clock]
 *     fn OUT = EXPR
 *     seq OUT <= EXPR @ posedge|negedge|high|low CLK [init EXPR|x]
 *     memory words EXPR ; width EXPR ; write sync|async PORT... ; read sync|async PORT...
 *   end
 *
 * WIDTH, DEFAULT, words and width are integer expressions over the cell's parameters declared
 * before them; fn/seq expressions name ports and parameters declared before them. Every output is
 * driven by exactly one fn, seq or the cell's memory (which drives every output no fn or seq
 * drives); a blackbox cell has no fn, seq or memory. `clock` marks an input; a seq's CLK and the
 * first port of a sync memory port must be such inputs. Port and parameter names share one
 * namespace per cell and cannot be `signed`, `clock` or `x`. A width without identifiers is folded
 * to a constant; a port width (constant, or with the default parameters) is at most
 * ODIN3_READER_MAX_WIDTH (2^20). A memory port's list is checked only for input directions and
 * the clock: `read sync clk` without an address and `read async` with no inputs are accepted
 * (memory semantics arrive in Phase 4). NUM is a decimal number (digits, optional fraction).
 */
#ifndef ODIN3_TECHLIB_READER_H
#define ODIN3_TECHLIB_READER_H

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "techlib/expr.h"
#include "util/hash.h"

#include <stdbool.h>
#include <stdint.h>

/* Cell kind; registered as granularity BIT, HARD and BLACKBOX. */
typedef enum odin3_techlib_kind {
    ODIN3_TECHLIB_GATE,
    ODIN3_TECHLIB_HARD,
    ODIN3_TECHLIB_BLACKBOX
} odin3_techlib_kind;

/* Port modifiers, one entry per port of the registered definition (same index). */
typedef struct odin3_techlib_port {
    bool is_signed; /* `signed`: operands of fn are signed */
    bool clock;     /* `clock`: a clock input */
} odin3_techlib_port;

/* `fn OUT = EXPR`. Ports are indices into the definition's ports. */
typedef struct odin3_techlib_fn {
    uint32_t port;
    const odin3_expr *expr;
    uint32_t line;
} odin3_techlib_fn;

typedef enum odin3_techlib_trigger {
    ODIN3_TECHLIB_POSEDGE,
    ODIN3_TECHLIB_NEGEDGE,
    ODIN3_TECHLIB_HIGH, /* level-sensitive (latch): transparent while CLK is 1 */
    ODIN3_TECHLIB_LOW
} odin3_techlib_trigger;

/* `seq OUT <= DATA @ TRIGGER CLK [init INIT]`; init NULL means x. */
typedef struct odin3_techlib_seq {
    uint32_t port;
    const odin3_expr *data;
    odin3_techlib_trigger trigger;
    uint32_t clock; /* input port index */
    const odin3_expr *init;
    uint32_t line;
} odin3_techlib_seq;

/* A memory `write` or `read` port: its mode and the input ports it lists, in order. */
typedef struct odin3_techlib_memport {
    bool write;
    bool sync;
    const uint32_t *ports;
    uint32_t n_ports;
    uint32_t line;
} odin3_techlib_memport;

/*
 * `memory words EXPR` with its `width`, `write` and `read` statements (semantics: Phase 4). The
 * i-th read port (in statement order) drives outs[i], the i-th memory-driven output in port order
 * (spec: the positional pairing rule).
 */
typedef struct odin3_techlib_memory {
    const odin3_expr *words;
    const odin3_expr *width;
    const odin3_techlib_memport *mports;
    uint32_t n_mports;
    const uint32_t *outs; /* output ports the memory drives */
    uint32_t n_outs;
    uint32_t line;
} odin3_techlib_memory;

/*
 * The library data of a registered cell (the function table entry for 1E and Phase 4). Lives in
 * the design arena as long as the design; identifiers in its expressions are design strtab IDs.
 */
struct odin3_techlib_cell {
    uint32_t library; /* strtab ID of the library name */
    const char *file; /* the source name the cell was read from */
    uint32_t line;    /* line of its `cell` statement */
    odin3_techlib_kind kind;
    uint32_t area;  /* strtab ID of the number as written; 0 when absent */
    uint32_t delay; /* strtab ID of the number as written; 0 when absent */
    const odin3_techlib_port *ports;
    uint32_t n_ports;
    const odin3_techlib_fn *fns;
    uint32_t n_fns;
    const odin3_techlib_seq *seqs;
    uint32_t n_seqs;
    const odin3_techlib_memory *memory; /* NULL when the cell has none */
};

/* A library held in memory: name (used in messages and recorded per cell) and contents. */
typedef struct odin3_techlib_text {
    const char *name;
    odin3_bytes text;
} odin3_techlib_text;

/*
 * Parses a whole library, then registers each cell as a design-local cell type (in file order)
 * with its library data attached (odin3_techlib_cell_get). A malformed library (syntax, duplicate
 * or unknown names, an undriven output, a cell name the design already has) is logged as
 * "name:line[:column]: message" and returns ODIN3_ERR_PARSE with no type registered.
 * ODIN3_ERR_NO_MEMORY on out of memory; when that happens while registering, the cells registered
 * before stay. Either way the design's strtab and arena may have grown.
 */
odin3_status odin3_techlib_read_text(odin3_design *design, const odin3_techlib_text *src);

/*
 * odin3_techlib_read_text on the contents of the file at path. ODIN3_ERR_IO (logged) when the file
 * cannot be opened or read.
 */
odin3_status odin3_techlib_read(odin3_design *design, const char *path);

/* The library data of cell type id; NULL for a type the tech-library reader did not register. */
const odin3_techlib_cell *odin3_techlib_cell_get(const odin3_design *design, odin3_celltype_id id);

#endif
