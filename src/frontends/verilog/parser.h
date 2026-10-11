/*
 * parser.h — the Verilog-2005 parser's driver interface (2C spec §2.2, §3.4, §5): one 2B stream
 * in, one UNIT of the parsed-form AST out, with a per-run read context that carries the error
 * count, the `begin_keywords stack and the caps across the run's streams.
 *
 * The parser is generated from verilog.y and verilog.l into the build tree (§6); this header
 * holds what the generated code and its callers share, and includes nothing generated.
 */
#ifndef ODIN3_FRONTENDS_VERILOG_PARSER_H
#define ODIN3_FRONTENDS_VERILOG_PARSER_H

#include "ast/ast.h"
#include "ast/srcman.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* The caps of §5 that belong to the parser and lexer (the AST's own caps are 2A's). */
enum {
    ODIN3_VP_MAX_ERRORS = 20,                      /* error diagnostics per run (§3.4) */
    ODIN3_VP_YYMAXDEPTH = 8 * ODIN3_AST_MAX_DEPTH, /* Bison stack entries: 262144 */
    ODIN3_VP_YYINITDEPTH = 256,                    /* Bison's first stack, on the C stack */
    ODIN3_VL_MAX_KEYWORD_DEPTH = 64,               /* `begin_keywords nesting (§2.5) */
    ODIN3_VL_MAX_COMMENT_BYTES = 1 << 20,          /* recorded comment text, cut beyond */
    ODIN3_VL_MAX_DIRECTIVE_BYTES = 4096            /* a pass-through directive's argument */
};

/* The keyword-set masks (§2.5): each keyword carries the sets it belongs to. */
enum {
    ODIN3_VL_KW_1995 = 1,
    ODIN3_VL_KW_2001_NOCONFIG = 2,
    ODIN3_VL_KW_2001 = 4,
    ODIN3_VL_KW_2005 = 8
};

/* The parser's per-run context (actions.h); the generated parser's yyparse takes it. */
typedef struct odin3_vp_ctx odin3_vp_ctx;

/*
 * The Bison location of a token or a node (§2.2; api.location.type): the node range (2A AST-2)
 * and the stream offset of the first byte, which orders tokens within a stream (locs do not:
 * an include or an expansion is another buffer). 12 bytes per parser stack entry.
 */
enum { ODIN3_VP_LOC_BYTES = 12 }; /* the record's size, a memory gate (§5) */
typedef struct odin3_vp_loc {
    odin3_loc loc, end;
    uint32_t off;
} odin3_vp_loc;
_Static_assert(sizeof(odin3_vp_loc) == ODIN3_VP_LOC_BYTES, "2C §2.2: odin3_vp_loc is 12 bytes");

/*
 * One stream to parse (plan point (b)): the 2B stream text (borrowed; must outlive the parse),
 * the project FILE buffer its segment map belongs to, and the UNIT's name (a strtab ID: the
 * file's path as given).
 */
typedef struct odin3_vp_stream {
    odin3_bytes text;
    odin3_srcbuf_id stream;
    uint32_t name;
} odin3_vp_stream;

/* The run's caps (§5); odin3_vl_run_init sets the defaults, a test lowers them (parser_test.h). */
typedef struct odin3_vp_limits {
    uint32_t yymaxdepth;      /* ODIN3_VP_YYMAXDEPTH */
    uint32_t max_errors;      /* ODIN3_VP_MAX_ERRORS */
    uint32_t keyword_depth;   /* ODIN3_VL_MAX_KEYWORD_DEPTH */
    uint32_t ident_bytes;     /* ODIN3_AST_MAX_IDENT_BYTES */
    uint32_t string_bytes;    /* ODIN3_AST_MAX_STRING_BYTES */
    uint32_t text_bytes;      /* ODIN3_AST_MAX_TEXT_BYTES */
    uint32_t comment_bytes;   /* ODIN3_VL_MAX_COMMENT_BYTES */
    uint32_t directive_bytes; /* ODIN3_VL_MAX_DIRECTIVE_BYTES */
} odin3_vp_limits;

/*
 * The per-run read context (plan point (c)): one per read run, across its streams (a run is one
 * compilation unit, §2.5). kw_mask[0 .. kw_depth) and kw_at are the `begin_keywords stack: the
 * pushed sets (ODIN3_VL_KW_*, the innermost last) and where each directive was; with none the
 * 1364-2005 set applies. errors counts the error diagnostics (§3.4, point (r)); stop is set at
 * the cap or by a fatal failure; tokens is NULL or a caller-owned vec of odin3_vp_loc to which
 * the lexer appends every returned token's location, the end of input included (the location
 * oracle's map).
 */
typedef struct odin3_vl_run {
    odin3_design *design;
    uint32_t kw_mask[ODIN3_VL_MAX_KEYWORD_DEPTH];
    odin3_loc kw_at[ODIN3_VL_MAX_KEYWORD_DEPTH];
    uint32_t kw_depth, errors;
    bool stop;
    odin3_vp_limits limits;
    odin3_vec *tokens;
} odin3_vl_run;

/*
 * Starts a run on design: an empty keyword stack (the 1364-2005 set), no errors, the default
 * limits, no token map. Never fails; NULL run is a no-op.
 */
void odin3_vl_run_init(odin3_vl_run *run, odin3_design *design);

/*
 * Parses one stream into the store ast (parsed form, open): on success stores the stream's UNIT
 * in *unit and returns ODIN3_OK. ODIN3_ERR_PARSE, with *unit 0, after the stream's located
 * diagnostics (any error in the stream, including a run already stopped by this stream's cap);
 * the pending stack is unwound to where it was, and the nodes already made stay as abandoned
 * subtrees (the pass destroys the store, §4.2). ODIN3_ERR_NO_MEMORY (the run is stopped).
 * ODIN3_ERR_INVALID_ARG (logged) for a NULL argument or a run whose stop flag is already set.
 */
odin3_status odin3_verilog_parse_stream(odin3_vl_run *run, odin3_ast *ast,
                                        const odin3_vp_stream *stream, odin3_ast_id *unit);

#endif
