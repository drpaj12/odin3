/*
 * verilog.y — the Verilog-2005 grammar (2C spec §3, VP-11..VP-17): a pure LALR(1) Bison parser
 * over the tokens of verilog.l, building the parsed-form AST bottom-up through the 2A builders.
 *
 * Every action is exactly one VP(...) or VP_ERR(...) statement around a vp_* function of
 * actions*.c (tools/check_rules.py enforces it), so this file holds no logic and every action is
 * inert after the first error (§3.2, §3.4). The prologue holds #include lines and the YY* macros
 * only. Sections follow §3.2's Annex A order; each banner names the task that owns it.
 */

%require "3.8"
%define api.pure full
%define api.prefix {odin3_vp_}
%define api.token.prefix {TOK_}
%define api.location.type {odin3_vp_loc}
%define parse.error detailed
%define parse.lac full
%define lr.type lalr
%locations
%expect 0
%parse-param {odin3_vp_ctx *ctx}
%lex-param {void *ODIN3_VP_SCANNER}

%code requires {
#include "frontends/verilog/parser.h"
}

%code {
#include "frontends/verilog/actions.h"
#define YYLLOC_DEFAULT(Cur, Rhs, N) odin3_vp_lloc(&(Cur), (Rhs), (N))
#define YYMAXDEPTH odin3_vp_yymaxdepth()
#define YYINITDEPTH ODIN3_VP_YYINITDEPTH
#define YYSTACK_USE_ALLOCA 0
}

/* node: a made node (0 in error mode); u: a strtab ID, payload ID, mark or enum value;
 * text: a view into the scanner's copy of the stream (number pieces, directives, opaque bodies). */
%union {
    odin3_ast_id node;
    uint32_t u;
    odin3_bytes text;
}

/* ==== Tokens (§2.3–§2.9). String aliases spell each token as Verilog does, so Bison's
 * detailed messages read "unexpected endmodule, expecting ';'" (§3.4, plan point (e)). ==== */

/* Value tokens (§2.3, §2.4, §2.8, §2.9; plan point (a)). */
%token <u> IDENT "identifier"
%token <u> SYSTEM_IDENT "system name"
%token <u> STRING "string"
%token <u> REAL_NUMBER "real number"
%token <text> UNSIGNED_NUMBER "number"
%token <text> BASE "base"
%token <text> BASED_DIGITS "digits"
%token <text> DIRECTIVE "directive"
%token <text> OPAQUE_SPECIFY "specify block"
%token <text> OPAQUE_PRIMITIVE "primitive"
%token <text> OPAQUE_CONFIG "config"

/* Keywords: the 1364-2005 Annex B list (§2.5), sorted. `specify`, `primitive` and `config` are
 * not here: the lexer turns each into its OPAQUE_* token above (§2.8). The gate keywords nand,
 * nor and xnor are KW_* because NAND, NOR and XNOR name the operators ~&, ~| and ~^. */
%token ALWAYS "always" AND "and" ASSIGN "assign" AUTOMATIC "automatic" BEGIN "begin" BUF "buf"
%token BUFIF0 "bufif0" BUFIF1 "bufif1" CASE "case" CASEX "casex" CASEZ "casez" CELL "cell" CMOS "cmos"
%token DEASSIGN "deassign" DEFAULT "default" DEFPARAM "defparam" DESIGN "design" DISABLE "disable"
%token EDGE "edge" ELSE "else" END "end" ENDCASE "endcase" ENDCONFIG "endconfig"
%token ENDFUNCTION "endfunction" ENDGENERATE "endgenerate" ENDMODULE "endmodule"
%token ENDPRIMITIVE "endprimitive" ENDSPECIFY "endspecify" ENDTABLE "endtable" ENDTASK "endtask"
%token EVENT "event" FOR "for" FORCE "force" FOREVER "forever" FORK "fork" FUNCTION "function"
%token GENERATE "generate" GENVAR "genvar" HIGHZ0 "highz0" HIGHZ1 "highz1" IF "if" IFNONE "ifnone"
%token INCDIR "incdir" INCLUDE "include" INITIAL "initial" INOUT "inout" INPUT "input"
%token INSTANCE "instance" INTEGER "integer" JOIN "join" LARGE "large" LIBLIST "liblist"
%token LIBRARY "library" LOCALPARAM "localparam" MACROMODULE "macromodule" MEDIUM "medium"
%token MODULE "module" KW_NAND "nand" NEGEDGE "negedge" NMOS "nmos" KW_NOR "nor"
%token NOSHOWCANCELLED "noshowcancelled" NOT "not" NOTIF0 "notif0" NOTIF1 "notif1" OR "or"
%token OUTPUT "output" PARAMETER "parameter" PMOS "pmos" POSEDGE "posedge" PULL0 "pull0" PULL1 "pull1"
%token PULLDOWN "pulldown" PULLUP "pullup" PULSESTYLE_ONDETECT "pulsestyle_ondetect"
%token PULSESTYLE_ONEVENT "pulsestyle_onevent" RCMOS "rcmos" REAL "real" REALTIME "realtime" REG "reg"
%token RELEASE "release" REPEAT "repeat" RNMOS "rnmos" RPMOS "rpmos" RTRAN "rtran" RTRANIF0 "rtranif0"
%token RTRANIF1 "rtranif1" SCALARED "scalared" SHOWCANCELLED "showcancelled" SIGNED "signed"
%token SMALL "small" SPECPARAM "specparam" STRONG0 "strong0" STRONG1 "strong1" SUPPLY0 "supply0"
%token SUPPLY1 "supply1" TABLE "table" TASK "task" TIME "time" TRAN "tran" TRANIF0 "tranif0"
%token TRANIF1 "tranif1" TRI "tri" TRI0 "tri0" TRI1 "tri1" TRIAND "triand" TRIOR "trior"
%token TRIREG "trireg" UNSIGNED "unsigned" USE "use" UWIRE "uwire" VECTORED "vectored" WAIT "wait"
%token WAND "wand" WEAK0 "weak0" WEAK1 "weak1" WHILE "while" WIRE "wire" WOR "wor" KW_XNOR "xnor"
%token XOR "xor"

/* Operators and attribute brackets (§2.4, §2.7); every other operator and punctuation mark is a
 * character literal ('+', ';', '@', …). `^~` lexes as XNOR too. */
%token OR_OR "||" AND_AND "&&" XNOR "~^" NAND "~&" NOR "~|"
%token EQ "==" NE "!=" CASE_EQ "===" CASE_NE "!==" LE "<=" GE ">="
%token SHL "<<" SHR ">>" ASHL "<<<" ASHR ">>>" POW "**"
%token PLUS_COLON "+:" MINUS_COLON "-:" TRIGGER "->"
%token ATTR_OPEN "(*" ATTR_CLOSE "*)" AT_STAR "@*"

/* ==== Precedence (§3.3). Bison -Werror rejects a precedence no rule uses, so each line arrives
 * with its rules (plan point (f)): Task 5 adds %right '?' and the eleven operator lines, Task 6
 * the two dangling-else lines. Nothing else ever gets a precedence (§3.1). ==== */

/* ==== %type: Task 4 (source text, modules, ports, parameters, declarations, items, opaque
 * units, attributes) ==== */

/* ==== %type: Task 5 (expressions and primaries) ==== */

/* ==== %type: Task 6 (statements, timing, functions and tasks, case and metacomments) ==== */

/* ==== %type: Task 7 (instantiation, gates, generate) ==== */

%start source_text

%%

/* ==== A.1 Source text and directives — Task 4 ==== */

source_text: %empty ;

/* ==== A.1.3–A.1.4 Module header and ports — Task 4 ==== */

/* ==== A.2.1 Parameters — Task 4 ==== */

/* ==== A.2 Declarations — Task 4 ==== */

/* ==== A.1.4 Module items — Task 4 ==== */

/* ==== A.3–A.4 Instantiation and gates — Task 7 ==== */

/* ==== A.4.2 Generate — Task 7 ==== */

/* ==== A.2.6–A.2.7 Functions and tasks — Task 6 ==== */

/* ==== A.5, A.7 UDP, config and specify (opaque tokens) — Task 4 ==== */

/* ==== A.6 Statements — Task 6 ==== */

/* ==== A.6.5 Timing controls — Task 6 ==== */

/* ==== A.8 Expressions and primaries — Task 5 ==== */

/* ==== A.9 Attributes — Task 4 ==== */

%%
