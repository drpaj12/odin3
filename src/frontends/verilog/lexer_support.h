/*
 * lexer_support.h — the hand-written half of the Verilog lexer (2C spec §2): the scanner state
 * (Flex's yyextra), the lexer object that owns the reentrant scanner, the one diagnostic path of
 * the lexer, the grammar actions and yyerror, and the functions the actions of verilog.l call.
 *
 * Included by the generated scanner (verilog.l's prologue), the generated parser (via
 * actions.h) and the tests; it includes the generated parser header for the token numbers and
 * ODIN3_VP_STYPE, never the generated scanner header.
 */
#ifndef ODIN3_FRONTENDS_VERILOG_LEXER_SUPPORT_H
#define ODIN3_FRONTENDS_VERILOG_LEXER_SUPPORT_H

#include "ast/ast.h"
#include "ast/srcman.h"
#include "frontends/verilog/parser.h"
#include "odin3/odin3.h"
#include "util/attr.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/vec.h"
#include "verilog_parser.h" /* generated (build tree): ODIN3_VP_STYPE, TOK_* */

#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

/* Flex's bison-bridge names the value and location types YYSTYPE and YYLTYPE; api.prefix
 * renamed Bison's. The generated parser defines both macros identically. */
#ifndef YYSTYPE
#define YYSTYPE ODIN3_VP_STYPE
#endif
#ifndef YYLTYPE
#define YYLTYPE ODIN3_VP_LTYPE
#endif

/*
 * Flex's fatal errors: the skeleton's yy_fatal_error(msg, yyscanner) prints msg and calls
 * exit(YY_EXIT_FAILURE), which the core may not (spec §15.1; tools/check-symbols.sh rejects the
 * import). Its argument is evaluated first and never returns: odin3_vl_flex_fatal records the
 * fatal status and returns to the lexer entry point that armed the escape, so the exit() call is
 * dead and the compilers drop it. With the input in memory and the setup allocations reserved
 * (lexer_support.c), no fatal error is reachable in practice. Should one become reachable, an
 * escape out of the parser (odin3_vl_guard around yyparse) skips Bison's cleanup and leaks its
 * grown stacks and message buffer (never memory corruption); the setup paths leak nothing.
 * Flex's own fprintf(stderr, msg) before the handler stays (an unreachable path).
 */
#define YY_EXIT_FAILURE ((void)odin3_vl_flex_fatal(msg, yyscanner), 2)

/*
 * The scanner function Flex generates (YY_DECL): odin3_vp_lex (lexer_support.c) wraps it, so an
 * action may scan on without returning a token (the bytes of an opaque body, §2.8).
 */
#define YY_DECL int odin3_vl_scan(YYSTYPE *yylval_param, YYLTYPE *yylloc_param, void *yyscanner)

/* The blocks yy_scan_bytes asks the allocator for, reserved ahead (lexer_support.c). */
enum { ODIN3_VL_SETUP_BLOCKS = 3 };

/* The keys of a pending metacomment (§2.6). */
enum { ODIN3_VL_META_FULL_CASE = 1, ODIN3_VL_META_PARALLEL_CASE = 2 };

/* A pending metacomment (§2.6): its stream offset, its range and its keys
 * (ODIN3_VL_META_*), in stream-offset order until a case takes it. */
typedef struct odin3_vl_meta {
    uint32_t off;
    odin3_loc loc, end;
    uint8_t keys;
} odin3_vl_meta;

/*
 * The opaque body being scanned (§2.8): kind is its token (TOK_OPAQUE_*), 0 outside a body;
 * start and loc: its opening keyword; name: the first identifier after the keyword (primitive,
 * config); first: no token has followed the keyword yet.
 */
typedef struct odin3_vl_body {
    int kind;
    uint32_t start;
    odin3_loc loc;
    uint32_t name;
    bool first;
} odin3_vl_body;

/*
 * The scanner state (Flex's yyextra; 2C spec §2.1). run/ast/stream: what is scanned and where
 * tokens go; cur: the segment cursor over the stream's map; offset: the stream offset of the
 * next byte, tok_start: of the current rule's first byte; token_start/token_loc: the first byte
 * of a block comment, scanned over several rules; last_end: the end of the last token returned
 * (the end-of-input token's location, §2.2); body: the opaque body being scanned; eof_seen: the
 * end of input was reported once; error_mode: the flag a lexer error sets (the parser context's,
 * odin3_vl_bind_error_mode, else error_local); scratch: string, real and message text; meta:
 * the pending metacomments (odin3_vl_meta, §2.6); names: the names of the opaque bodies
 * (odin3_vl_name); fatal: a lexer-side NO_MEMORY or an internal error, which ends the stream.
 */
typedef struct odin3_vl_state {
    odin3_vl_run *run;
    odin3_ast *ast;
    odin3_vp_stream stream;
    odin3_srcman_cursor cur;
    uint32_t offset, tok_start, token_start;
    odin3_loc token_loc, last_end;
    odin3_vl_body body;
    odin3_bytes base; /* the last base token (`'h`), for the BASED exit's message */
    bool eof_seen, error_local;
    bool *error_mode;
    odin3_strbuf scratch;
    odin3_vec meta, names;
    odin3_status fatal;
    jmp_buf *escape;                          /* armed by the entry points (lexer_support.c) */
    void *setup[ODIN3_VL_SETUP_BLOCKS];       /* reserved setup blocks, handed out in order */
    size_t setup_size[ODIN3_VL_SETUP_BLOCKS]; /* their sizes */
    uint32_t setup_next;                      /* the next block to hand out; past the end: none */
} odin3_vl_state;

/* The name of an opaque body (odin3_vl_state.names): its token's offset and the name. */
typedef struct odin3_vl_name {
    uint32_t off;
    uint32_t name;
} odin3_vl_name;

/* The lexer: the reentrant scanner and its state. Opaque. */
typedef struct odin3_vl_lexer odin3_vl_lexer;

/*
 * Opens a lexer over stream (its text is copied once, by yy_scan_bytes) for the store ast and the
 * run run; *out gets it. ODIN3_ERR_NO_MEMORY (nothing allocated stays); ODIN3_ERR_INVALID_ARG
 * (logged) for a NULL argument or a stream over ODIN3_SRC_MAX_FILE_BYTES.
 */
odin3_status odin3_vl_open(odin3_vl_run *run, odin3_ast *ast, const odin3_vp_stream *stream,
                           odin3_vl_lexer **out);

/* Frees the lexer and everything it owns; NULL is a no-op. */
void odin3_vl_close(odin3_vl_lexer *lx);

/* The reentrant scanner (Flex's yyscan_t), the generated parser's lex parameter. */
void *odin3_vl_scanner(const odin3_vl_lexer *lx);

/* The generated scanner (Flex's YY_DECL under bison-bridge): the generated parser's yylex. */
int odin3_vp_lex(ODIN3_VP_STYPE *yylval_param, odin3_vp_loc *yylloc_param, void *yyscanner);

/* The UNIT name of the lexer's stream (odin3_vp_stream.name). */
uint32_t odin3_vl_stream_name(const odin3_vl_lexer *lx);

/*
 * Scans one token (tests): its kind (a TOK_* value, 0 at the end of input) is returned, its
 * value and location stored. After a fatal failure every call returns 0.
 */
int odin3_vl_next(odin3_vl_lexer *lx, ODIN3_VP_STYPE *val, odin3_vp_loc *loc);

/*
 * Runs fn(arg) with the lexer's fatal escape armed and returns its result; -1 when the scanner
 * hit a fatal error inside it (odin3_vl_fatal_status says which). The driver runs the generated
 * parser through it.
 */
int odin3_vl_guard(odin3_vl_lexer *lx, int (*fn)(void *arg), void *arg);

/* ODIN3_OK, or the lexer's fatal status (NO_MEMORY, or PARSE for an internal scanner error). */
odin3_status odin3_vl_fatal_status(const odin3_vl_lexer *lx);

/*
 * Points the lexer's error flag at *flag (the parser context's error_mode, §3.4): every lexer
 * error sets it, so the grammar's actions are inert from then on. Until bound, the lexer sets a
 * flag of its own.
 */
void odin3_vl_bind_error_mode(odin3_vl_lexer *lx, bool *flag);

/*
 * Moves the pending metacomments whose offset lies in [lo, hi) to the end of out (a vec of
 * odin3_vl_meta), in offset order, and removes them from the lexer's list (§2.6; binary search).
 * NO_MEMORY, with the list and out unchanged.
 */
odin3_status odin3_vl_meta_take(odin3_vl_lexer *lx, uint32_t lo, uint32_t hi, odin3_vec *out);

/* Empties the pending metacomment list (endmodule, §2.6). */
void odin3_vl_meta_clear(odin3_vl_lexer *lx);

/*
 * The name (a strtab ID) of the primitive or config body whose token starts at stream offset
 * off; 0 when there is none (the lexer reported it).
 */
uint32_t odin3_vl_opaque_name(const odin3_vl_lexer *lx, uint32_t off);

/*
 * The kind (odin3_ast_directive_kind) of the DIRECTIVE token starting at stream offset off;
 * ODIN3_AST_DIRECTIVE_COUNT when no pass-through directive starts there.
 */
uint32_t odin3_vl_directive_kind(const odin3_vl_lexer *lx, uint32_t off);

/*
 * The one diagnostic path of the lexer, the actions and yyerror (§3.4): logs the located
 * message through odin3_diag. An error counts against the run's cap (plan point (r)): the error
 * after the cap-th is replaced by "too many errors; stopping" (located at it) and sets
 * run->stop; warnings and infos are never counted. Nothing is logged once the run has stopped.
 */
void odin3_vl_report(odin3_vl_run *run, odin3_log_level level, odin3_loc loc, const char *fmt, ...)
    ODIN3_PRINTF(4, 5);

/* odin3_vl_report with a va_list. */
void odin3_vl_vreport(odin3_vl_run *run, odin3_log_level level, odin3_loc loc, const char *fmt,
                      va_list args) ODIN3_PRINTF(4, 0);

/* Whether the run has reported an error or stopped (actions are inert from then on, §3.4). */
bool odin3_vl_in_error(const odin3_vl_run *run);

/* --- keywords (keywords.c, §2.5) ---------------------------------------------------------- */

/* One keyword: its spelling, its token (TOK_*, or TOK_OPAQUE_* for an opaque opener) and the
 * keyword sets it belongs to (ODIN3_VL_KW_* bits). */
typedef struct odin3_vl_kw {
    const char *word;
    int token;
    uint32_t mask;
} odin3_vl_kw;

/* The keyword table, sorted by word (bytewise); *count gets its length. */
const odin3_vl_kw *odin3_vl_keywords(size_t *count);

/* The token of word when it is a keyword of a set in mask, else 0. */
int odin3_vl_keyword(odin3_bytes word, uint32_t mask);

/* The set of a `begin_keywords version string as written, with its quotes ("1364-1995",
 * "1364-2001", "1364-2001-noconfig", "1364-2005"): true and *mask; false for any other text. */
bool odin3_vl_keyword_version(odin3_bytes quoted, uint32_t *mask);

/* --- called by the generated scanner (verilog.l) ------------------------------------------- */

/* The generated scanner (YY_DECL); odin3_vp_lex calls it until it returns a token. */
int odin3_vl_scan(YYSTYPE *yylval_param, YYLTYPE *yylloc_param, void *yyscanner);

/* YY_USER_ACTION: the rule's first byte is at the current offset; advance past len bytes. */
void odin3_vl_user_action(odin3_vl_state *st, int len);

/*
 * The end of input (every start condition): reports an unterminated comment or opaque body and,
 * once per stream, each `begin_keywords still open; stores {last_end, last_end, stream length}
 * in *loc and returns TOK_YYEOF.
 */
int odin3_vl_eof(odin3_vl_state *st, odin3_vp_loc *loc);

/* A byte no rule takes, or a run of bytes >= 0x80 (one error, point (h)): "unexpected byte". */
void odin3_vl_unexpected(odin3_vl_state *st);

/* Blanks and the inside of a block comment: nothing to do (offsets carry the text). */
void odin3_vl_skip(odin3_vl_state *st);

/* §2.3: an unsigned number, a base (the scanner enters BASED), based digits (back to
 * INITIAL), a real. */
int odin3_vl_number(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc);
int odin3_vl_base(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc);
int odin3_vl_digits(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc);
int odin3_vl_real(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc);

/* The BASED exit after yyless(0): the byte is put back (a '_' is a located error). */
void odin3_vl_based_exit(odin3_vl_state *st);

/* §2.4: a word (keyword, identifier or opaque opener), an escaped identifier, a lone '', a
 * system name, a string. */
int odin3_vl_word(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc);
int odin3_vl_escaped(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc);
void odin3_vl_escape_empty(odin3_vl_state *st);
int odin3_vl_system(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc);
int odin3_vl_string(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc);

/* An operator or attribute token of kind (§2.4, §2.7); a one-byte punctuation mark (the byte). */
int odin3_vl_op(odin3_vl_state *st, odin3_vp_loc *loc, int kind);
int odin3_vl_punct(odin3_vl_state *st, odin3_vp_loc *loc);

/* §2.6: a line comment; a block comment's opening and closing delimiters. */
void odin3_vl_line_comment(odin3_vl_state *st);
void odin3_vl_comment_open(odin3_vl_state *st);
void odin3_vl_comment_close(odin3_vl_state *st);

/* §2.9: a pass-through directive with its argument; any other backtick word. */
int odin3_vl_directive(odin3_vl_state *st, YYSTYPE *val, odin3_vp_loc *loc);
void odin3_vl_unknown_directive(odin3_vl_state *st);

/* SystemVerilog's `.*` (implicit port connections, not Verilog-2005): a located error at the
 * '.'; both bytes are dropped, so `m u (.*);` yields this one error and no cascade. */
void odin3_vl_implicit_ports(odin3_vl_state *st);

/* Flex's fatal error (YY_EXIT_FAILURE): records the fatal status and returns to the armed
 * entry point (odin3_vl_guard). */
_Noreturn void odin3_vl_flex_fatal(const char *msg, void *scanner);

#endif
