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
#include "util/str.h"
#include "util/vec.h"
#include "verilog_parser.h" /* generated (build tree): ODIN3_VP_STYPE, TOK_* */

#include <setjmp.h>
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
 * (lexer_support.c), no fatal error is reachable in practice.
 */
#define YY_EXIT_FAILURE ((void)odin3_vl_flex_fatal(msg, yyscanner), 2)

/* The blocks yy_scan_bytes asks the allocator for, reserved ahead (lexer_support.c). */
enum { ODIN3_VL_SETUP_BLOCKS = 3 };

/* A pending metacomment (§2.6): its stream offset, its loc and its keys (full_case,
 * parallel_case bits), in stream-offset order until a case takes it. */
typedef struct odin3_vl_meta {
    uint32_t off;
    odin3_loc loc;
    uint32_t keys;
} odin3_vl_meta;

/*
 * The scanner state (Flex's yyextra; 2C spec §2.1). run/ast/stream: what is scanned and where
 * tokens go; cur: the segment cursor over the stream's map; offset: the stream offset of the
 * next byte, tok_start: of the current token's first byte; token_start: the first byte of a
 * token scanned over several rules (a block comment, an opaque body); last_end: the end of the
 * last token returned (the end-of-input token's location, §2.2); based: inside a based literal
 * (§2.3); scratch: string, comment and opaque-body text; meta: the pending metacomments
 * (odin3_vl_meta, §2.6); fatal: a lexer-side NO_MEMORY or an internal error, which ends the
 * stream (§3.4).
 */
typedef struct odin3_vl_state {
    odin3_vl_run *run;
    odin3_ast *ast;
    odin3_vp_stream stream;
    odin3_srcman_cursor cur;
    uint32_t offset, tok_start, token_start;
    odin3_loc last_end;
    bool based;
    odin3_strbuf scratch;
    odin3_vec meta;
    odin3_status fatal;
    jmp_buf *escape;                          /* armed by the entry points (lexer_support.c) */
    void *setup[ODIN3_VL_SETUP_BLOCKS];       /* reserved setup blocks, handed out in order */
    size_t setup_size[ODIN3_VL_SETUP_BLOCKS]; /* their sizes */
    uint32_t setup_next;                      /* the next block to hand out; past the end: none */
} odin3_vl_state;

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
 * The one diagnostic path of the lexer, the actions and yyerror (§3.4): logs the located
 * message through odin3_diag. An error counts against the run's cap (plan point (r)): the error
 * after the cap-th is replaced by "too many errors; stopping" (located at it) and sets
 * run->stop; warnings and infos are never counted. Nothing is logged once the run has stopped.
 */
void odin3_vl_report(odin3_vl_run *run, odin3_log_level level, odin3_loc loc, const char *fmt, ...)
    ODIN3_PRINTF(4, 5);

/* Whether the run has reported an error or stopped (actions are inert from then on, §3.4). */
bool odin3_vl_in_error(const odin3_vl_run *run);

/* --- called by the generated scanner (verilog.l) ------------------------------------------- */

/* YY_USER_ACTION: the token's first byte is at the current offset; advance past len bytes. */
void odin3_vl_user_action(odin3_vl_state *st, int len);

/* The end of input: stores {last_end, last_end, stream length} in *loc; returns TOK_YYEOF. */
int odin3_vl_eof(odin3_vl_state *st, odin3_vp_loc *loc);

/* A byte no rule takes: the located error "unexpected byte 0x..". */
void odin3_vl_unexpected(odin3_vl_state *st, const char *text);

/* Flex's fatal error (YY_EXIT_FAILURE): records the fatal status and returns to the armed
 * entry point (odin3_vl_guard). */
_Noreturn void odin3_vl_flex_fatal(const char *msg, void *scanner);

#endif
