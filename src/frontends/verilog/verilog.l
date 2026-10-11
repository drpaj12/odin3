/*
 * verilog.l — the Verilog-2005 lexer (2C spec §2, VP-2..VP-10): a reentrant Flex scanner over
 * one 2B stream, handing tokens with {loc, end, off} ranges to verilog.y.
 *
 * Every action is one call into lexer_support.c beside Flex's BEGIN/yyless(0), or one BEGIN
 * alone (tools/check_rules.py enforces it); the prologue holds #include lines and YY_USER_ACTION
 * only. The functions read the token's bytes from the stream at [tok_start, offset), which
 * YY_USER_ACTION keeps, so no action passes yytext. A token function returns its token, or
 * scans on inside an opaque body (§2.8), whose bytes every rule consumes as usual.
 *
 * Start conditions: BASED after a base until the digits (§2.3); CMT and BCMT inside a block
 * comment opened in INITIAL or in BASED (§2.6), so the closing delimiter returns to the right one.
 */

%option reentrant bison-bridge bison-locations noyywrap nounput noinput never-interactive
%option nodefault warn 8bit noyyalloc noyyrealloc
%option prefix="odin3_vp_"
%option extra-type="struct odin3_vl_state *"

%x BASED CMT BCMT

%{
#include "frontends/verilog/lexer_support.h"
#define YY_USER_ACTION odin3_vl_user_action(yyextra, yyleng);
%}

BLANK       [ \t\r\n\f]
DEC         [0-9][0-9_]*
EXP         [eE][+-]?{DEC}
WORD        [a-zA-Z_][a-zA-Z0-9_$]*
TSPART      [0-9]+[ \t]*[a-zA-Z]+
PRAGMA_TEXT ([^/\n]|"/"[^/\n]|\"[^"\n]*\")*

%%

    /* --- blanks and comments (§2.4, §2.6) */
{BLANK}+                    { odin3_vl_skip(yyextra); }
"//"[^\n]*                  { odin3_vl_line_comment(yyextra); }
"/*"                        { BEGIN(CMT); odin3_vl_comment_open(yyextra); }
<CMT,BCMT>[^*\n]+|\n|"*"+   { odin3_vl_skip(yyextra); }
<CMT>"*"+"/"                { BEGIN(INITIAL); odin3_vl_comment_close(yyextra); }
<BCMT>"*"+"/"               { BEGIN(BASED); odin3_vl_comment_close(yyextra); }

    /* --- numbers (§2.3) */
{DEC}                       { return odin3_vl_number(yyextra, yylval, yylloc); }
{DEC}"."{DEC}{EXP}?|{DEC}{EXP} { return odin3_vl_real(yyextra, yylval, yylloc); }
"'"[sS]?[bBoOdDhH]          { BEGIN(BASED); return odin3_vl_base(yyextra, yylval, yylloc); }
<BASED>{BLANK}+             { odin3_vl_skip(yyextra); }
<BASED>"//"[^\n]*           { odin3_vl_line_comment(yyextra); }
<BASED>"/*"                 { BEGIN(BCMT); odin3_vl_comment_open(yyextra); }
<BASED>"end"(specify|primitive|config){WORD}? { BEGIN(INITIAL); return odin3_vl_word(yyextra, yylval, yylloc); }
<BASED>[0-9a-fA-FxXzZ?][0-9a-fA-FxXzZ?_]* { BEGIN(INITIAL); return odin3_vl_digits(yyextra, yylval, yylloc); }
<BASED>.                    { yyless(0); BEGIN(INITIAL); odin3_vl_based_exit(yyextra); }

    /* --- identifiers, keywords, system names, strings (§2.4) */
{WORD}                      { return odin3_vl_word(yyextra, yylval, yylloc); }
"\\"[^ \t\r\n]+             { return odin3_vl_escaped(yyextra, yylval, yylloc); }
"\\"                        { odin3_vl_escape_empty(yyextra); }
"$"[a-zA-Z0-9_$]+           { return odin3_vl_system(yyextra, yylval, yylloc); }
\"([^"\\\n]|\\.)*\"?        |
\"([^"\\\n]|\\.)*\\         { return odin3_vl_string(yyextra, yylval, yylloc); }

    /* --- attributes and the implicit event control (§2.7) */
"(*"                        { return odin3_vl_op(yyextra, yylloc, TOK_ATTR_OPEN); }
"*)"                        { return odin3_vl_op(yyextra, yylloc, TOK_ATTR_CLOSE); }
"@"{BLANK}*("*"|"("{BLANK}*"*"{BLANK}*")") { return odin3_vl_op(yyextra, yylloc, TOK_AT_STAR); }

    /* --- SystemVerilog's implicit port connection: one located error, not a bad '*)' */
".*"                        { odin3_vl_implicit_ports(yyextra); }

    /* --- operators (§2.4) */
"||"                        { return odin3_vl_op(yyextra, yylloc, TOK_OR_OR); }
"&&"                        { return odin3_vl_op(yyextra, yylloc, TOK_AND_AND); }
"~^"|"^~"                   { return odin3_vl_op(yyextra, yylloc, TOK_XNOR); }
"~&"                        { return odin3_vl_op(yyextra, yylloc, TOK_NAND); }
"~|"                        { return odin3_vl_op(yyextra, yylloc, TOK_NOR); }
"=="                        { return odin3_vl_op(yyextra, yylloc, TOK_EQ); }
"!="                        { return odin3_vl_op(yyextra, yylloc, TOK_NE); }
"==="                       { return odin3_vl_op(yyextra, yylloc, TOK_CASE_EQ); }
"!=="                       { return odin3_vl_op(yyextra, yylloc, TOK_CASE_NE); }
"<="                        { return odin3_vl_op(yyextra, yylloc, TOK_LE); }
">="                        { return odin3_vl_op(yyextra, yylloc, TOK_GE); }
"<<"                        { return odin3_vl_op(yyextra, yylloc, TOK_SHL); }
">>"                        { return odin3_vl_op(yyextra, yylloc, TOK_SHR); }
"<<<"                       { return odin3_vl_op(yyextra, yylloc, TOK_ASHL); }
">>>"                       { return odin3_vl_op(yyextra, yylloc, TOK_ASHR); }
"**"                        { return odin3_vl_op(yyextra, yylloc, TOK_POW); }
"+:"                        { return odin3_vl_op(yyextra, yylloc, TOK_PLUS_COLON); }
"-:"                        { return odin3_vl_op(yyextra, yylloc, TOK_MINUS_COLON); }
"->"                        { return odin3_vl_op(yyextra, yylloc, TOK_TRIGGER); }
[-+*/%!~&|^<>=?:;,.()\[\]{}@#] { return odin3_vl_punct(yyextra, yylloc); }

    /* --- pass-through directives (§2.9): one rule per argument shape, so code after an
     * argument-less directive on its line is lexed as code */
"`"(celldefine|endcelldefine|resetall|end_keywords|nounconnected_drive) |
"`"(default_nettype|unconnected_drive)([ \t]+{WORD})? |
"`begin_keywords"([ \t]+\"[^"\n]*\")? |
"`timescale"([ \t]+{TSPART}[ \t]*"/"[ \t]*{TSPART})? |
"`pragma"([ \t]{PRAGMA_TEXT})? { return odin3_vl_directive(yyextra, yylval, yylloc); }
"`pragma"[ \t]{PRAGMA_TEXT}"/"/\n { return odin3_vl_directive(yyextra, yylval, yylloc); }
"`"{WORD}                   { odin3_vl_unknown_directive(yyextra); }

    /* --- bytes outside the grammar (§2.1, point (h)) */
[\x80-\xff]+                |
.|\n                        { odin3_vl_unexpected(yyextra); }

<*><<EOF>>                  { BEGIN(INITIAL); return odin3_vl_eof(yyextra, yylloc); }

%%
