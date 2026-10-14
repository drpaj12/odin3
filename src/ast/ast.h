/*
 * ast.h — the AST store (AST-4..11): 24-byte node records, the child table, payloads, the
 * shape-checking builder with its depth cap, and scalar read accessors.
 */
#ifndef ODIN3_AST_AST_H
#define ODIN3_AST_AST_H

#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/hash.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A node ID, local to its store; 0 = none. IDs are dense from 1 in creation order. */
typedef struct odin3_ast_id {
    uint32_t v;
} odin3_ast_id;

/* The node record (AST-4): the field list is the contract; read it through the accessors. */
typedef struct odin3_ast_node {
    uint8_t kind;   /* odin3_ast_kind */
    uint8_t sub;    /* per-kind sub-kind */
    uint16_t flags; /* per-kind flag bits */
    uint32_t loc;   /* source range (§3.1); end 0 = unknown */
    uint32_t end;
    uint32_t name;   /* strtab ID, 0 = none */
    uint32_t child;  /* first child-table index of the span; payload kinds: the payload ID */
    uint32_t nchild; /* span length (0 for payload kinds) */
} odin3_ast_node;

/* The record's size, a memory gate (§12.5); ast.c asserts it at compile time. */
enum { ODIN3_AST_NODE_BYTES = 24 };

/* odin3_ast (the store) is declared in ir/design.h; its fields are in ast_internal.h. */

/* odin3_ast_spec (a builder call's scalar fields) is public: odin3.h. */

/* A read-only view of IDs; valid until the next builder call on the store. */
typedef struct odin3_ast_span {
    const odin3_ast_id *ids;
    uint32_t n;
} odin3_ast_span;

/*
 * A NUMBER payload (§4.5): the literal as written. Base and signedness live on the node (sub,
 * ODIN3_AST_F_NUMBER_SIGNED), never here.
 */
typedef struct odin3_ast_number {
    uint32_t width;      /* declared size, 0 = unsized */
    uint32_t nbits;      /* bits stored: what the digits spell (unsized decimal: 32 min) */
    const uint8_t *bits; /* nbits odin3_bit values (0 1 x z), LSB first, in the AST arena */
    bool has_xz;         /* any x or z digit (C9) */
    bool has_question;   /* a ? digit was written (stored as z; the printer restores ?) */
    bool warned;         /* number_value has reported this literal's sizing warning */
} odin3_ast_number;

/*
 * A literal's three tokens as byte views (L8: `8 'h FF` has blanks or comments between them):
 * size (decimal, may be empty), base (`'h`, `'sb`, …; empty = a plain decimal such as `5`) and
 * digits (`_` allowed).
 */
typedef struct odin3_number_text {
    odin3_bytes size;
    odin3_bytes base;
    odin3_bytes digits;
} odin3_number_text;

/* The comment kinds, ODIN3_AST_COMMENT_METACOMMENT and odin3_ast_comment_spec are public: odin3.h.
 */

/* A recorded comment; text is NUL-terminated and owned by the store. */
typedef struct odin3_ast_comment {
    const char *text;
    odin3_loc loc;
    odin3_loc end;
    uint32_t len;
    uint8_t kind;
    uint8_t flags;
} odin3_ast_comment;

/* A read-only run of comments, sorted by loc; valid until the store is destroyed. */
typedef struct odin3_ast_comment_span {
    const odin3_ast_comment *items;
    uint32_t n;
} odin3_ast_comment_span;

/* --- lifetime ------------------------------------------------------------------------------ */

/*
 * A new, empty store of the given form for pass run `run`, using the design's strtab and source
 * manager. INVALID_ARG (logged) for a NULL design or out or an unknown form; NO_MEMORY (with *out
 * unchanged) on out of memory.
 */
odin3_status odin3_ast_create(odin3_design *design, odin3_ast_form form, odin3_passrun_id run,
                              odin3_ast **out);

/* Frees the store and everything it owns (not the strtab's strings). NULL is a no-op. */
void odin3_ast_destroy(odin3_ast *ast);

/* --- building (§5) ------------------------------------------------------------------------- */

/*
 * Appends a node whose children are ids[0..n) (copied into the child table) and stores its ID in
 * *out. The shape is validated against the slot table; a violation, a NULL argument or a sealed
 * store is INVALID_ARG (logged). More than the child cap or a height above ODIN3_AST_MAX_DEPTH
 * is PARSE, located at spec->loc. NO_MEMORY on out of memory or ID exhaustion. Nothing changes
 * on failure.
 */
odin3_status odin3_ast_make(odin3_ast *ast, const odin3_ast_spec *spec, const odin3_ast_id *ids,
                            uint32_t n, odin3_ast_id *out);

/* The pending stack's height (a mark for make_marked/unwind); 0 for NULL or a sealed store. */
uint32_t odin3_ast_mark(const odin3_ast *ast);

/*
 * Pushes node (0, for an E*0 tail entry, or an ID made already) on the pending stack. INVALID_ARG
 * (logged) for a NULL or sealed store or an ID not made yet; NO_MEMORY. Unchanged on failure.
 */
odin3_status odin3_ast_push(odin3_ast *ast, odin3_ast_id node);

/*
 * odin3_ast_make with children = the pending stack from mark to its top; on success the stack is
 * popped back to mark. INVALID_ARG (logged) for a mark above the top; otherwise as make, with the
 * stack unchanged on failure.
 */
odin3_status odin3_ast_make_marked(odin3_ast *ast, const odin3_ast_spec *spec, uint32_t mark,
                                   odin3_ast_id *out);

/*
 * The pending stack from mark to its top, in push order: a view valid until the next push, make
 * or unwind (the parser's reordered kinds copy it, 2C §3.2). Empty for a NULL store or a mark at
 * or above the top.
 */
odin3_ast_span odin3_ast_pending_span(const odin3_ast *ast, uint32_t mark);

/* Pops the pending stack back to mark (error recovery); a mark at or above the top is a no-op. */
void odin3_ast_unwind(odin3_ast *ast, uint32_t mark);

/* Stores a REAL payload and its payload ID (from 1) in *payload. INVALID_ARG (logged) for a NULL
 * argument, a sealed store or a non-finite value (inf or nan: not Verilog); NO_MEMORY. */
odin3_status odin3_ast_real_new(odin3_ast *ast, double value, uint32_t *payload);

/*
 * Copies text (the raw bytes of an opaque construct) into the store as a TEXT payload and stores
 * its payload ID in *payload. INVALID_ARG (logged) for a NULL argument or a sealed store; PARSE
 * (logged; no location is known here) above ODIN3_AST_MAX_TEXT_BYTES; NO_MEMORY. Unchanged on
 * failure.
 */
odin3_status odin3_ast_text_new(odin3_ast *ast, odin3_bytes text, uint32_t *payload);

/* Interns str in the design's strtab (names live there, §14 Q2); NO_MEMORY; INVALID_ARG. */
odin3_status odin3_ast_intern(odin3_ast *ast, odin3_bytes str, uint32_t *name);

/*
 * Literal text → payload (§4.5), stored as written (digits → bits, no sizing; L4–L8): the
 * payload ID (from 1) goes to *payload. Give the node odin3_ast_number_kind's sub and flags.
 * INVALID_ARG (logged) for a NULL argument, a sealed store or a malformed base piece (or a size
 * without a base); PARSE, located at loc, for a zero or non-decimal size, a width above
 * ODIN3_AST_MAX_NUMBER_BITS, no digits, a digit invalid for the base (a decimal is all digits
 * or one x/z/? digit), more than ODIN3_AST_MAX_DECIMAL_DIGITS decimal digits or more bits than
 * the width cap; NO_MEMORY. Nothing changes on failure.
 */
odin3_status odin3_ast_number_parse(odin3_ast *ast, odin3_loc loc, odin3_number_text pieces,
                                    uint32_t *payload);

/*
 * The NUMBER node's sub-kind (ODIN3_AST_NUMBER_DEC/BIN/OCT/HEX) and flags (SIGNED for `'s…` and
 * for a plain decimal) for pieces, so the node agrees with its payload. INVALID_ARG (logged) for
 * a NULL out-pointer or a malformed base piece; the outputs are unchanged on failure.
 */
odin3_status odin3_ast_number_kind(odin3_number_text pieces, uint8_t *sub, uint16_t *flags);

/*
 * Copies a ready payload (adapters): nbits ≥ 1 bits of 0 1 x z, has_xz and has_question
 * consistent with them (warned is reset). INVALID_ARG (logged) for a NULL argument, a sealed
 * store or an inconsistent struct; PARSE, located at loc, for a width or nbits above
 * ODIN3_AST_MAX_NUMBER_BITS; NO_MEMORY. Nothing changes on failure.
 */
odin3_status odin3_ast_number_new(odin3_ast *ast, odin3_loc loc, const odin3_ast_number *num,
                                  uint32_t *payload);

/*
 * Attaches the ATTR nodes attrs[0..n) (n ≥ 1, copied into the child table) to node, once (§5,
 * §7). INVALID_ARG (logged) for a NULL argument, a sealed store, a node that is not made or is
 * itself an ATTR, an entry that is not a made ATTR, or a node already annotated; NO_MEMORY.
 * Nothing changes on failure.
 */
odin3_status odin3_ast_attach(odin3_ast *ast, odin3_ast_id node, const odin3_ast_id *attrs,
                              uint32_t n);

/*
 * Records a comment in the side table (§7), in stream order; finish sorts the table by loc.
 * INVALID_ARG (logged) for a NULL argument, a sealed store, non-zero reserved fields, an unknown
 * kind or flag, a loc that is not in a FILE buffer, or an end that is neither 0 nor a later loc
 * of that buffer; NO_MEMORY (also past 2^32 − 2 comments). Nothing changes on failure.
 */
odin3_status odin3_ast_comment_add(odin3_ast *ast, const odin3_ast_comment_spec *spec);

/*
 * odin3_ast_finish (public, odin3.h) seals the store: sorts the comment table by loc (stable for
 * equal records), frees the build scratch (pending stack, heights); a later builder call is
 * INVALID_ARG. INVALID_ARG (logged) for NULL or a store already sealed. The read API works before
 * and after.
 */

/* --- reading (§6): values, never faulting; NONE / 0 / empty for an out-of-range ID ---------- */

odin3_ast_kind odin3_ast_kind_of(const odin3_ast *ast, odin3_ast_id node);
uint32_t odin3_ast_sub(const odin3_ast *ast, odin3_ast_id node);
uint16_t odin3_ast_flags(const odin3_ast *ast, odin3_ast_id node);
odin3_loc odin3_ast_loc(const odin3_ast *ast, odin3_ast_id node);
odin3_loc odin3_ast_end(const odin3_ast *ast, odin3_ast_id node);
uint32_t odin3_ast_name(const odin3_ast *ast, odin3_ast_id node);

/* The node's name bytes from the design strtab ("" for none). */
const char *odin3_ast_name_str(const odin3_ast *ast, odin3_ast_id node);

/*
 * The spelling of the node's sub-kind ("+", "posedge", "casez", …); "" when its kind has none.
 * Never NULL. (By node rather than (kind, sub): two adjacent uint32_t fail the lint gate; a
 * kind-level lookup reads odin3_ast_kind_info_of(kind)->sub_names.)
 */
const char *odin3_ast_sub_name(const odin3_ast *ast, odin3_ast_id node);

uint32_t odin3_ast_nchild(const odin3_ast *ast, odin3_ast_id node);

/* Child idx (slots first, then the tail); 0 when idx ≥ nchild or the slot is absent. */
odin3_ast_id odin3_ast_child(const odin3_ast *ast, odin3_ast_id node, uint32_t idx);

/* The node's children as a view valid until the next builder call (make or attach). */
odin3_ast_span odin3_ast_children(const odin3_ast *ast, odin3_ast_id node);

/* The payload ID of a NUMBER, REAL or TEXT node; 0 otherwise. */
uint32_t odin3_ast_payload(const odin3_ast *ast, odin3_ast_id node);

/* A REAL node's value; 0.0 for any other node. */
double odin3_ast_real(const odin3_ast *ast, odin3_ast_id node);

/* A TEXT-payload node's bytes (owned by the store); {NULL, 0} for any other node. */
odin3_bytes odin3_ast_text(const odin3_ast *ast, odin3_ast_id node);

/* One past the last node ID (1 for an empty store); 0 for NULL. */
odin3_ast_id odin3_ast_node_end(const odin3_ast *ast);

odin3_ast_form odin3_ast_form_of(const odin3_ast *ast);
odin3_passrun_id odin3_ast_run(const odin3_ast *ast);

/* The largest node height so far (valid during the build and after finish). */
uint32_t odin3_ast_max_height(const odin3_ast *ast);

/* The UNIT nodes, in creation order. */
uint32_t odin3_ast_root_count(const odin3_ast *ast);
odin3_ast_id odin3_ast_root(const odin3_ast *ast, uint32_t idx);

/*
 * Where the library UNITs begin (2C §4.2 step 3, §9 amendment (b)): the odin3_ast_root index of
 * the first UNIT read from a -v library file or a -y library directory; 0 = none (a read run
 * always has a design UNIT first, so index 0 is never a library UNIT). Every root at or past it
 * came from a library, every root before it from a design file, so 2D never picks a library
 * module as top and lets a design unit shadow a library module. The setter records root_idx
 * (read_verilog sets it once its libraries are read; open or sealed store alike); a NULL store
 * or an index at or past the root count is ignored. The reader gives 0 for NULL.
 */
void odin3_ast_set_library_units_from(odin3_ast *ast, uint32_t root_idx);
uint32_t odin3_ast_library_units_from(const odin3_ast *ast);

/*
 * Library-read flags of a root (2D-7b, `read_verilog --lib`): ODIN3_UNIT_LIB marks a UNIT that
 * came from a library read (kept for later reads, elaborated only where instantiated), with the
 * elaboration flags of that read: ODIN3_UNIT_NOLATCHES, ODIN3_UNIT_NOMEM2REG and
 * ODIN3_UNIT_VTR_COMPAT. 0 = a plain
 * UNIT, which takes the flags of the read that elaborates it. odin3_ast_mark_units sets flags
 * for the roots [first, end) (open or sealed store alike; roots past the root count, a NULL store
 * or a range with first >= end are ignored; NO_MEMORY with the marks unchanged); the reader gives
 * 0 for NULL or a root past the count.
 */
enum {
    ODIN3_UNIT_LIB = 1,
    ODIN3_UNIT_NOLATCHES = 2,
    ODIN3_UNIT_NOMEM2REG = 4,
    ODIN3_UNIT_VTR_COMPAT = 8
};
odin3_status odin3_ast_mark_units(odin3_ast *ast, uint32_t first, uint32_t end, uint8_t flags);
uint8_t odin3_ast_unit_flags(const odin3_ast *ast, uint32_t root_idx);

/*
 * The preprocessed streams of a `read_verilog --lib` read (2D-7b, I1 of the PR review): each
 * design file's names, its stream FILE buffer and a copy of its preprocessed text, kept so a
 * later read parses exactly what the library read's own macros and include directories produced,
 * never what the later read's would. odin3_ast_keep_stream copies src.text into the store's
 * arena (NO_MEMORY with nothing kept); odin3_ast_stream_at returns the idx-th (read order) or
 * NULL past the end or for a NULL store. The record and its text live as long as the store.
 */
typedef struct odin3_ast_stream {
    uint32_t given, resolved, library;
    odin3_srcbuf_id buffer;
    odin3_bytes text;
} odin3_ast_stream;
odin3_status odin3_ast_keep_stream(odin3_ast *ast, const odin3_ast_stream *src);
const odin3_ast_stream *odin3_ast_stream_at(const odin3_ast *ast, uint32_t idx);

/* The number of comments recorded in the store's side table (§7); 0 for NULL. */
uint32_t odin3_ast_comment_count(const odin3_ast *ast);

/* The design whose strtab and source manager the store uses. */
const odin3_design *odin3_ast_design(const odin3_ast *ast);

/*
 * Bytes the store holds: nodes, child table, payload table, comment table and arena (strtab,
 * attribute map and symbol table excluded).
 */
size_t odin3_ast_bytes_reserved(const odin3_ast *ast);

/* A NUMBER node's payload (§6 `_number`); NULL for any other node. */
const odin3_ast_number *odin3_ast_number_of(const odin3_ast *ast, odin3_ast_id node);

/* A literal's value (odin3_ast_number_value). */
typedef struct odin3_ast_literal {
    odin3_value value; /* BITS, LSB first; bytes in the caller's arena */
    bool is_signed;    /* the node's SIGNED flag */
    bool is_unsized;   /* no size was written: extend to context with the MSB (below) */
} odin3_ast_literal;

/*
 * The NUMBER node's value, sized exactly as Yosys read_verilog sizes it (const2ast.cc; the exit
 * bar is Parmys/Yosys; §4.5 amendment). Sized: the width, zero-extended, or x/z-extended when
 * the written leading digit is x/z; wider digits are truncated, with one warning per literal
 * when the value requires more bits than the width (Yosys's rule and wording). Unsized: every
 * written bit, leading zeros included, extended to 32 when fewer were written; a signed
 * literal whose top bit is then 1 gains a 0 bit (`'sh8000_0000` is +2^31 in 33 bits; `5` is 32
 * bits, `2147483648` 33). Context extension (2D): an unsized value (is_unsized) is widened to
 * its context by repeating its top bit even when it is unsigned (Yosys bitsAsUnsizedConst:
 * `'hFFFF_FFFF` in a 64-bit context is all ones); a sized one by sign or zero extension.
 * INVALID_ARG (logged) for a NULL argument or a node without a NUMBER payload; NO_MEMORY (*out
 * unchanged, no warning spent). Usable after finish (it only sets the payload's warned flag).
 */
odin3_status odin3_ast_number_value(odin3_ast *ast, odin3_ast_id node, odin3_arena *arena,
                                    odin3_ast_literal *out);

/* The ATTR nodes attached to node (a view valid until the next builder call); empty if none. */
odin3_ast_span odin3_ast_attrs(const odin3_ast *ast, odin3_ast_id node);

/*
 * The comments whose loc lies in the expansion range of range ([loc, end) once mapped to one
 * FILE buffer). Empty before finish, for a NULL store, or when it does not map into one file (so an
 * includer's range never covers an included file's comments; ask per file).
 */
odin3_ast_comment_span odin3_ast_comments_in(const odin3_ast *ast, odin3_range range);

/* The comments of FILE buffer buf, in loc order; empty before finish or for any other buffer. */
odin3_ast_comment_span odin3_ast_comments_of(const odin3_ast *ast, odin3_srcbuf_id buf);

/* --- iteration (§6): explicit stacks, never recursion -------------------------------------- */

typedef enum odin3_ast_walk_action {
    ODIN3_AST_WALK_CONTINUE = 0, /* descend into the node's children */
    ODIN3_AST_WALK_SKIP          /* prune them (post still runs for the node) */
} odin3_ast_walk_action;

typedef odin3_ast_walk_action (*odin3_ast_pre_fn)(const odin3_ast *ast, odin3_ast_id node,
                                                  void *user);
typedef void (*odin3_ast_post_fn)(const odin3_ast *ast, odin3_ast_id node, void *user);

/*
 * Visits the subtree of root depth-first: pre on the way down, post on the way up (either may
 * be NULL); children in slot/tail order, ID-0 children (absent slots) skipped, attributes not
 * visited. The stack ({node, next child} frames) is sized from odin3_ast_max_height and
 * allocated before the first callback. INVALID_ARG (logged) for a NULL store or a root that is
 * 0 or not made, or a store deeper than its recorded height (corruption); NO_MEMORY before any
 * callback. The store must not be built on during the walk.
 */
odin3_status odin3_ast_walk(const odin3_ast *ast, odin3_ast_id root, odin3_ast_pre_fn pre,
                            odin3_ast_post_fn post, void *user);

typedef enum odin3_ast_event { ODIN3_AST_ENTER = 1, ODIN3_AST_LEAVE } odin3_ast_event;

/* One stack frame of a walk or cursor: a node and the index of its next child. */
typedef struct odin3_ast_frame {
    uint32_t node;
    uint32_t next;
} odin3_ast_frame;

/* A caller-driven walk (the printer, 2D): fields are private. */
typedef struct odin3_ast_cursor {
    const odin3_ast *ast;
    odin3_ast_frame *frames; /* owned; cap frames */
    uint32_t depth;
    uint32_t cap;
    bool fresh;   /* the root's ENTER is still to come */
    bool entered; /* the last event was an ENTER (skip applies to it) */
    bool corrupt; /* the store was deeper than its recorded height; iteration stopped */
} odin3_ast_cursor;

/*
 * Starts a cursor at root (its first event is root's ENTER). INVALID_ARG (logged) for a NULL
 * argument or a root that is 0 or not made; NO_MEMORY. On failure *cur is an empty cursor (next
 * returns false, free is a no-op).
 */
odin3_status odin3_ast_cursor_init(odin3_ast_cursor *cur, const odin3_ast *ast, odin3_ast_id root);

/*
 * The next event (ENTER on the way down, LEAVE on the way up; the same order as odin3_ast_walk)
 * into *ev and *node; false when the walk is done (or on a NULL argument).
 */
bool odin3_ast_cursor_next(odin3_ast_cursor *cur, odin3_ast_event *ev, odin3_ast_id *node);

/* Right after an ENTER: prunes that node's children, so the next event is its LEAVE. Otherwise
 * a no-op. */
void odin3_ast_cursor_skip(odin3_ast_cursor *cur);

/* Frees the cursor's stack; NULL and a freed or empty cursor are no-ops. */
void odin3_ast_cursor_free(odin3_ast_cursor *cur);

/* An on-demand parent array (4 bytes per node; §6): fields are private. */
typedef struct odin3_ast_parent_index {
    odin3_ast_id *parents; /* owned; n entries, slot 0 unused */
    uint32_t n;
} odin3_ast_parent_index;

/*
 * Builds the parent of every node made so far: the node whose span holds it, or for an ATTR the
 * node it annotates; 0 for roots (UNITs and abandoned subtrees). INVALID_ARG (logged) for a NULL
 * argument; NO_MEMORY with *out unchanged. Rebuild after further makes.
 */
odin3_status odin3_ast_parent_index_build(const odin3_ast *ast, odin3_ast_parent_index *out);

/* The parent of node; 0 for a root, a NULL index or an ID outside the index. */
odin3_ast_id odin3_ast_parent(const odin3_ast_parent_index *ix, odin3_ast_id node);

/* Frees the index and empties it; NULL is a no-op. */
void odin3_ast_parent_index_destroy(odin3_ast_parent_index *ix);

#endif
