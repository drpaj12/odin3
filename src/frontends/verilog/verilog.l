/*
 * verilog.l — the Verilog-2005 lexer (2C spec §2, VP-2..VP-10): a reentrant Flex scanner over
 * one 2B stream, handing tokens with {loc, end, off} ranges to verilog.y.
 *
 * Every action is one call into lexer_support.c beside Flex's BEGIN/yyless (tools/check_rules.py
 * enforces it); the prologue holds #include lines and YY_USER_ACTION only. This rules section is
 * the Task 1 skeleton: end of input, and every byte reported as unexpected. Task 3 replaces it
 * with the token rules.
 */

%option reentrant bison-bridge bison-locations noyywrap nounput noinput never-interactive
%option nodefault warn 8bit noyyalloc noyyrealloc
%option prefix="odin3_vp_"
%option extra-type="struct odin3_vl_state *"

%{
#include "frontends/verilog/lexer_support.h"
#define YY_USER_ACTION odin3_vl_user_action(yyextra, yyleng);
%}

%%

<<EOF>>         { return odin3_vl_eof(yyextra, yylloc); }
.|\n            { odin3_vl_unexpected(yyextra, yytext); }

%%
