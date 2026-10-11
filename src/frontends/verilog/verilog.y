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

/* u: a pending-stack mark, a spec (VP_SPEC: kind, sub-kind and flags of the node an item rule
 * builds from what it pushed), a flag set or an enum value. node: a made node (0: none). */
%type <u> source_text module_body module_items list_mark group_close module_params module_ports
%type <u> ansi_ports ansi_port_head ansi_port_open nonansi_ports
%type <u> header_params header_param_head header_param_open
%type <u> port_dir port_head port_spec port_kind signed_opt port_item port_slot port_declarators
%type <u> param_kw param_type param_spec param_item param_slot param_assigns
%type <u> item_decl net_item net_type net_start net_flags vector_opt net_spec net_slots
%type <u> net_declarators var_item var_reg var_head var_word var_slot var_noslot var_declarators
%type <u> event_item event_declarators genvar_item genvar_declarators assign_item assign_slots
%type <u> net_assigns defparam_list decl_dims strength0 strength1 strength_pair delay_pair
%type <u> mintypmax_pair attrs_opt attr_instance attr_specs
%type <u> hier_ref hier_index hier_open select_open select_part select_plus select_minus lvalues
%type <u> number_size number_base number_digits
%type <node> description module_decl module_item item_node directive
%type <node> port_entry port_or_empty empty_port port_declarator var_declarator dim_declarator
%type <node> genvar_declarator param_assign defparam_assign range range_opt strength_opt
%type <node> drive_strength charge_strength delay_opt delay3 delay_value mintypmax net_assign
%type <node> lvalue lvalue_concat select attr_spec expression primary number

/* ==== %type: Task 5 (expressions and primaries) ==== */

/* ==== %type: Task 6 (statements, timing, functions and tasks, case and metacomments) ==== */

/* ==== %type: Task 7 (instantiation, gates, generate) ==== */

%start source_text

%%

/* ==== A.1 Source text and directives — Task 4 ==== */

/* The driver makes the UNIT over the descriptions this pushes (§3.6 "Units and directives"). */
source_text: %empty                    { VP($$, vp_mark(ctx, &$$)); }
           | source_text description   { VP($$, vp_list_next(ctx, $1, $2, &$$)); }
           ;

description: attrs_opt module_decl     { VP($$, vp_attrs(ctx, $1, $2, &$$)); }
           | directive
           | OPAQUE_PRIMITIVE          { VP($$, vp_opaque(ctx, &@1, $1, ODIN3_AST_UDP_DECL, &$$)); }
           | OPAQUE_CONFIG             { VP($$, vp_opaque(ctx, &@1, $1, ODIN3_AST_CONFIG_DECL, &$$)); }
           | error ENDMODULE           { VP($$, vp_none(ctx, &$$)); }
           ;

/* A pass-through directive, at unit level or as a module item (§2.9). */
directive: DIRECTIVE                   { VP($$, vp_directive(ctx, &@1, $1, &$$)); } ;

/* ==== A.1.3–A.1.4 Module header and ports — Task 4 ==== */

/* MODULE(name; L? params; L? ports; I* items): the body pushes both slots, then the items. */
module_decl: MODULE IDENT module_body ENDMODULE
               { VP($$, vp_module(ctx, $3, &@$, $2, &$$)); }
           | MACROMODULE IDENT module_body ENDMODULE
               { VP($$, vp_macromodule(ctx, $3, &@$, $2, &$$)); }
           ;

module_body: list_mark module_params module_ports ';' module_items
               { VP($$, vp_value(ctx, $1, &$$)); }
           ;

list_mark: %empty                      { VP($$, vp_mark(ctx, &$$)); } ;

/* `module m;` has no parameter or port LIST; `#()` and `()` are empty LISTs, located at their
 * `#` and `(` (§3.6). */
module_params: %empty                  { VP($$, vp_push_none(ctx, &$$)); }
             | '#' '(' list_mark ')'   { VP($$, vp_list_slot(ctx, &@$, $3, &$$)); }
             | '#' '(' list_mark header_params ')'
                                       { VP($$, vp_group_list_slot(ctx, &@$, $3, &$$)); }
             ;

/* ANSI and non-ANSI lists are told apart by the token after `(` (§3.1 c). */
module_ports: %empty                   { VP($$, vp_push_none(ctx, &$$)); }
            | '(' list_mark ')'        { VP($$, vp_list_slot(ctx, &@$, $2, &$$)); }
            | '(' list_mark ansi_ports ')'
                                       { VP($$, vp_group_list_slot(ctx, &@$, $2, &$$)); }
            | '(' list_mark nonansi_ports ')'
                                       { VP($$, vp_list_slot(ctx, &@$, $2, &$$)); }
            ;

/* ANSI ports: a direction starts a PORT_DECL{IN_HEADER}; a name without one joins it as another
 * declarator. The open group is closed by the next direction or by `)`. */
ansi_ports: ansi_port_head
          | ansi_ports ',' ansi_port_head
          | ansi_ports ',' port_declarator     { VP($$, vp_group_join(ctx, $1, $3, &$$)); }
          ;

ansi_port_head: ansi_port_open range_opt port_declarator
                  { VP($$, vp_group_first(ctx, $2, $3, &$$)); }
              ;

ansi_port_open: group_close attrs_opt port_spec
                  { VP($$, vp_group_open(ctx, $2, &@3, $3, &$$)); }
              ;

group_close: %empty                    { VP($$, vp_group_close(ctx, &$$)); } ;

/* Non-ANSI ports (§3.1 k): the first entry may be empty only before a comma. */
nonansi_ports: port_entry                      { VP($$, vp_list_first(ctx, $1, &$$)); }
             | empty_port ',' port_or_empty    { VP($$, vp_list_two(ctx, $1, $3, &$$)); }
             | nonansi_ports ',' port_or_empty { VP($$, vp_list_next(ctx, $1, $3, &$$)); }
             ;

port_or_empty: empty_port | port_entry ;

empty_port: %empty                     { VP($$, vp_port_empty(ctx, &@$, &$$)); } ;

port_entry: IDENT                      { VP($$, vp_port_name(ctx, &@$, $1, &$$)); }
          | select                     { VP($$, vp_port_expr(ctx, &@$, $1, &$$)); }
          | lvalue_concat              { VP($$, vp_port_expr(ctx, &@$, $1, &$$)); }
          | '.' IDENT '(' ')'          { VP($$, vp_port_dotted(ctx, &@$, $2, &$$)); }
          | '.' IDENT '(' lvalue ')'   { VP($$, vp_port_dotted_expr(ctx, &@$, $2, $4, &$$)); }
          ;

/* A direction, a net kind or variable type, and a sign: PORT_DECL's spec (§3.6 modules). */
port_spec: port_head signed_opt        { VP($$, vp_or(ctx, $1, $2, &$$)); } ;

port_head: port_dir port_kind          { VP($$, vp_or(ctx, $1, $2, &$$)); } ;

port_dir: INPUT    { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_PORT_DECL, ODIN3_AST_PORT_DECL_INPUT), &$$)); }
        | OUTPUT   { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_PORT_DECL, ODIN3_AST_PORT_DECL_OUTPUT), &$$)); }
        | INOUT    { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_PORT_DECL, ODIN3_AST_PORT_DECL_INOUT), &$$)); }
        ;

/* NET = 1 + the net kind (0: none written); DT for reg, integer, time, real, realtime (M12). */
port_kind: %empty                      { VP($$, vp_value(ctx, VP_NO_FLAGS, &$$)); }
         | net_type                    { VP($$, vp_port_net(ctx, $1, &$$)); }
         | REG                         { VP($$, vp_value(ctx, VP_PORT_DT(ODIN3_AST_DT_REG), &$$)); }
         | INTEGER                     { VP($$, vp_value(ctx, VP_PORT_DT(ODIN3_AST_DT_INTEGER), &$$)); }
         | TIME                        { VP($$, vp_value(ctx, VP_PORT_DT(ODIN3_AST_DT_TIME), &$$)); }
         | REAL                        { VP($$, vp_value(ctx, VP_PORT_DT(ODIN3_AST_DT_REAL), &$$)); }
         | REALTIME                    { VP($$, vp_value(ctx, VP_PORT_DT(ODIN3_AST_DT_REALTIME), &$$)); }
         ;

/* `unsigned` is accepted wherever `signed` is and sets no flag (§3.6). */
signed_opt: %empty                     { VP($$, vp_value(ctx, VP_NO_FLAGS, &$$)); }
          | SIGNED                     { VP($$, vp_value(ctx, VP_SIGNED, &$$)); }
          | UNSIGNED                   { VP($$, vp_value(ctx, VP_NO_FLAGS, &$$)); }
          ;

port_declarator: IDENT                 { VP($$, vp_decl_name(ctx, &@$, $1, &$$)); }
               | IDENT '=' expression  { VP($$, vp_decl_init(ctx, &@$, $1, $3, &$$)); }
               ;

/* ==== A.2.1 Parameters — Task 4 ==== */

/* `#(parameter A = 1, B = 2, localparam C = 3)`: one PARAM_DECL{IN_HEADER} per keyword. */
header_params: header_param_head
             | header_params ',' header_param_head
             | header_params ',' param_assign     { VP($$, vp_group_join(ctx, $1, $3, &$$)); }
             ;

header_param_head: header_param_open range_opt param_assign
                     { VP($$, vp_group_first(ctx, $2, $3, &$$)); }
                 ;

header_param_open: group_close attrs_opt param_spec
                     { VP($$, vp_group_open(ctx, $2, &@3, $3, &$$)); }
                 ;

param_spec: param_kw param_type        { VP($$, vp_or(ctx, $1, $2, &$$)); } ;

param_kw: PARAMETER    { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_PARAM_DECL, ODIN3_AST_PARAM_DECL_PARAMETER), &$$)); }
        | LOCALPARAM   { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_PARAM_DECL, ODIN3_AST_PARAM_DECL_LOCALPARAM), &$$)); }
        ;

param_type: signed_opt
          | INTEGER                    { VP($$, vp_value(ctx, VP_PARAM_DT(ODIN3_AST_DT_INTEGER), &$$)); }
          | REAL                       { VP($$, vp_value(ctx, VP_PARAM_DT(ODIN3_AST_DT_REAL), &$$)); }
          | REALTIME                   { VP($$, vp_value(ctx, VP_PARAM_DT(ODIN3_AST_DT_REALTIME), &$$)); }
          | TIME                       { VP($$, vp_value(ctx, VP_PARAM_DT(ODIN3_AST_DT_TIME), &$$)); }
          ;

param_assign: IDENT '=' expression     { VP($$, vp_decl_init(ctx, &@$, $1, $3, &$$)); } ;

/* PARAM_DECL items: `parameter`/`localparam` with a type or sign and range; `specparam` with a
 * range (P7; inside `specify` it is opaque text). */
param_item: param_spec param_slot param_assigns ';'   { VP($$, vp_value(ctx, $1, &$$)); }
          | SPECPARAM param_slot param_assigns ';'
              { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_PARAM_DECL, ODIN3_AST_PARAM_DECL_SPECPARAM), &$$)); }
          ;

param_slot: range_opt                  { VP($$, vp_push_slot(ctx, $1, &$$)); } ;

param_assigns: param_assign            { VP($$, vp_list_first(ctx, $1, &$$)); }
             | param_assigns ',' param_assign { VP($$, vp_list_next(ctx, $1, $3, &$$)); }
             ;

/* One DEFPARAM per pair (P6), the target always a HIER_NAME; the attributes go to the first. */
defparam_list: attrs_opt DEFPARAM defparam_assign      { VP($$, vp_item_push(ctx, $1, $3, &$$)); }
             | defparam_list ',' defparam_assign       { VP($$, vp_list_next(ctx, $1, $3, &$$)); }
             ;

defparam_assign: hier_ref '=' expression  { VP($$, vp_defparam(ctx, $1, &@$, $3, &$$)); } ;

/* ==== A.2 Declarations — Task 4 ==== */

/* Each item rule pushes its node's slots and declarators and yields its spec; module_item makes
 * the node (§3.6 "Nets and variables"). Ports in the body are items with IN_HEADER clear. */
item_decl: port_item | net_item | var_item | param_item | event_item | genvar_item | assign_item ;

port_item: port_spec port_slot port_declarators ';'    { VP($$, vp_value(ctx, $1, &$$)); } ;

port_slot: range_opt                   { VP($$, vp_push_slot(ctx, $1, &$$)); } ;

port_declarators: port_declarator      { VP($$, vp_list_first(ctx, $1, &$$)); }
                | port_declarators ',' port_declarator { VP($$, vp_list_next(ctx, $1, $3, &$$)); }
                ;

/* NET_DECL[kind]{SIGNED VECTORED SCALARED}(STRENGTH?, R?, DELAY?; DECLARATOR+). */
net_item: net_spec net_slots net_declarators ';'       { VP($$, vp_value(ctx, $1, &$$)); } ;

net_spec: net_start net_flags          { VP($$, vp_or(ctx, $1, $2, &$$)); } ;

net_start: net_type strength_opt       { VP($$, vp_spec_slot(ctx, $1, $2, &$$)); } ;

net_flags: vector_opt signed_opt       { VP($$, vp_or(ctx, $1, $2, &$$)); } ;

vector_opt: %empty                     { VP($$, vp_value(ctx, VP_NO_FLAGS, &$$)); }
          | VECTORED                   { VP($$, vp_value(ctx, VP_FLAGS(ODIN3_AST_F_NET_DECL_VECTORED), &$$)); }
          | SCALARED                   { VP($$, vp_value(ctx, VP_FLAGS(ODIN3_AST_F_NET_DECL_SCALARED), &$$)); }
          ;

net_slots: range_opt delay_opt         { VP($$, vp_list_two(ctx, $1, $2, &$$)); } ;

net_type: WIRE     { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_NET_DECL, ODIN3_AST_NET_DECL_WIRE), &$$)); }
        | TRI      { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_NET_DECL, ODIN3_AST_NET_DECL_TRI), &$$)); }
        | WAND     { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_NET_DECL, ODIN3_AST_NET_DECL_WAND), &$$)); }
        | WOR      { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_NET_DECL, ODIN3_AST_NET_DECL_WOR), &$$)); }
        | TRIAND   { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_NET_DECL, ODIN3_AST_NET_DECL_TRIAND), &$$)); }
        | TRIOR    { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_NET_DECL, ODIN3_AST_NET_DECL_TRIOR), &$$)); }
        | TRI0     { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_NET_DECL, ODIN3_AST_NET_DECL_TRI0), &$$)); }
        | TRI1     { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_NET_DECL, ODIN3_AST_NET_DECL_TRI1), &$$)); }
        | TRIREG   { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_NET_DECL, ODIN3_AST_NET_DECL_TRIREG), &$$)); }
        | UWIRE    { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_NET_DECL, ODIN3_AST_NET_DECL_UWIRE), &$$)); }
        | SUPPLY0  { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_NET_DECL, ODIN3_AST_NET_DECL_SUPPLY0), &$$)); }
        | SUPPLY1  { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_NET_DECL, ODIN3_AST_NET_DECL_SUPPLY1), &$$)); }
        ;

net_declarators: var_declarator        { VP($$, vp_list_first(ctx, $1, &$$)); }
               | net_declarators ',' var_declarator { VP($$, vp_list_next(ctx, $1, $3, &$$)); }
               ;

/* VAR_DECL[reg]{SIGNED}(R?; DECLARATOR+); integer, time, real, realtime take neither. */
var_item: var_head var_slot var_declarators ';'        { VP($$, vp_value(ctx, $1, &$$)); }
        | var_word var_noslot var_declarators ';'      { VP($$, vp_value(ctx, $1, &$$)); }
        ;

var_head: var_reg signed_opt           { VP($$, vp_or(ctx, $1, $2, &$$)); } ;

var_reg: REG       { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_VAR_DECL, ODIN3_AST_VAR_DECL_REG), &$$)); } ;

var_word: INTEGER  { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_VAR_DECL, ODIN3_AST_VAR_DECL_INTEGER), &$$)); }
        | TIME     { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_VAR_DECL, ODIN3_AST_VAR_DECL_TIME), &$$)); }
        | REAL     { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_VAR_DECL, ODIN3_AST_VAR_DECL_REAL), &$$)); }
        | REALTIME { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_VAR_DECL, ODIN3_AST_VAR_DECL_REALTIME), &$$)); }
        ;

var_slot: range_opt                    { VP($$, vp_push_slot(ctx, $1, &$$)); } ;

var_noslot: %empty                     { VP($$, vp_push_none(ctx, &$$)); } ;

var_declarators: var_declarator        { VP($$, vp_list_first(ctx, $1, &$$)); }
               | var_declarators ',' var_declarator { VP($$, vp_list_next(ctx, $1, $3, &$$)); }
               ;

/* DECLARATOR(name; E? initialiser; R* unpacked dimensions, outermost first). */
var_declarator: dim_declarator
              | IDENT '=' expression   { VP($$, vp_decl_init(ctx, &@$, $1, $3, &$$)); }
              ;

dim_declarator: IDENT                  { VP($$, vp_decl_name(ctx, &@$, $1, &$$)); }
              | IDENT decl_dims        { VP($$, vp_decl_dims(ctx, $2, &@$, $1, &$$)); }
              ;

decl_dims: range                       { VP($$, vp_dims_first(ctx, $1, &$$)); }
         | decl_dims range             { VP($$, vp_list_next(ctx, $1, $2, &$$)); }
         ;

/* EVENT_DECL(DECLARATOR+) (N18), GENVAR_DECL(DECLARATOR+) (N6). */
event_item: EVENT event_declarators ';'
              { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_EVENT_DECL, 0), &$$)); }
          ;

event_declarators: dim_declarator      { VP($$, vp_list_first(ctx, $1, &$$)); }
                 | event_declarators ',' dim_declarator { VP($$, vp_list_next(ctx, $1, $3, &$$)); }
                 ;

genvar_item: GENVAR genvar_declarators ';'
               { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_GENVAR_DECL, 0), &$$)); }
           ;

genvar_declarators: genvar_declarator  { VP($$, vp_list_first(ctx, $1, &$$)); }
                  | genvar_declarators ',' genvar_declarator { VP($$, vp_list_next(ctx, $1, $3, &$$)); }
                  ;

genvar_declarator: IDENT               { VP($$, vp_decl_name(ctx, &@$, $1, &$$)); } ;

/* RANGE(msb, lsb) as written (N2). */
range: '[' expression ':' expression ']'   { VP($$, vp_range(ctx, &@$, $2, $4, &$$)); } ;

range_opt: %empty                      { VP($$, vp_none(ctx, &$$)); }
         | range
         ;

/* A.2.2.2 strengths: STRENGTH[drive] with S0 and S1, STRENGTH[charge] with S0 (A3, N19). */
strength_opt: %empty                   { VP($$, vp_none(ctx, &$$)); }
            | drive_strength
            | charge_strength
            ;

drive_strength: '(' strength_pair ')'  { VP($$, vp_leaf(ctx, &@$, $2, &$$)); } ;

strength_pair: strength0 ',' strength1 { VP($$, vp_or(ctx, $1, $3, &$$)); }
             | strength1 ',' strength0 { VP($$, vp_or(ctx, $1, $3, &$$)); }
             ;

strength0: SUPPLY0     { VP($$, vp_drive0(ctx, ODIN3_AST_DRIVE_SUPPLY, &$$)); }
         | STRONG0     { VP($$, vp_drive0(ctx, ODIN3_AST_DRIVE_STRONG, &$$)); }
         | PULL0       { VP($$, vp_drive0(ctx, ODIN3_AST_DRIVE_PULL, &$$)); }
         | WEAK0       { VP($$, vp_drive0(ctx, ODIN3_AST_DRIVE_WEAK, &$$)); }
         | HIGHZ0      { VP($$, vp_drive0(ctx, ODIN3_AST_DRIVE_HIGHZ, &$$)); }
         ;

strength1: SUPPLY1     { VP($$, vp_drive1(ctx, ODIN3_AST_DRIVE_SUPPLY, &$$)); }
         | STRONG1     { VP($$, vp_drive1(ctx, ODIN3_AST_DRIVE_STRONG, &$$)); }
         | PULL1       { VP($$, vp_drive1(ctx, ODIN3_AST_DRIVE_PULL, &$$)); }
         | WEAK1       { VP($$, vp_drive1(ctx, ODIN3_AST_DRIVE_WEAK, &$$)); }
         | HIGHZ1      { VP($$, vp_drive1(ctx, ODIN3_AST_DRIVE_HIGHZ, &$$)); }
         ;

charge_strength: '(' SMALL ')'         { VP($$, vp_charge(ctx, &@$, ODIN3_AST_CHARGE_SMALL, &$$)); }
               | '(' MEDIUM ')'        { VP($$, vp_charge(ctx, &@$, ODIN3_AST_CHARGE_MEDIUM, &$$)); }
               | '(' LARGE ')'         { VP($$, vp_charge(ctx, &@$, ODIN3_AST_CHARGE_LARGE, &$$)); }
               ;

/* A.2.2.3 delays with A.6.5's delay_value (§3.1 f): DELAY(one E | two | three; MINTYPMAX only
 * here). A delay operand is never a sized literal (§2.3). */
delay_opt: %empty                      { VP($$, vp_none(ctx, &$$)); }
         | delay3
         ;

delay3: '#' delay_value                { VP($$, vp_delay1(ctx, &@$, $2, &$$)); }
      | '#' '(' mintypmax ')'          { VP($$, vp_delay1(ctx, &@$, $3, &$$)); }
      | '#' '(' delay_pair ')'         { VP($$, vp_delay_marked(ctx, $3, &@$, &$$)); }
      | '#' '(' delay_pair ',' mintypmax ')'  { VP($$, vp_delay_last(ctx, $3, &@$, $5, &$$)); }
      ;

delay_pair: mintypmax ',' mintypmax    { VP($$, vp_list_two(ctx, $1, $3, &$$)); } ;

delay_value: number_size               { VP($$, vp_number(ctx, &@$, $1, &$$)); }
           | REAL_NUMBER               { VP($$, vp_real(ctx, &@1, $1, &$$)); }
           | IDENT                     { VP($$, vp_ident(ctx, &@1, $1, &$$)); }
           ;

mintypmax: expression
         | mintypmax_pair ':' expression   { VP($$, vp_mintypmax(ctx, $1, &@$, $3, &$$)); }
         ;

mintypmax_pair: expression ':' expression  { VP($$, vp_list_two(ctx, $1, $3, &$$)); } ;

/* ==== A.1.4 Module items — Task 4 ==== */

/* An item's node is made without its attributes, so its range starts at its own first token
 * (point (g)); several (* *) instances merge into one attach. */
module_items: %empty                   { VP($$, vp_mark(ctx, &$$)); }
            | module_items module_item { VP($$, vp_item_next(ctx, $1, $2, &$$)); }
            ;

module_item: attrs_opt item_decl       { VP($$, vp_item(ctx, $1, &@2, $2, &$$)); }
           | attrs_opt item_node       { VP($$, vp_attrs(ctx, $1, $2, &$$)); }
           | defparam_list ';'         { VP($$, vp_none(ctx, &$$)); }
           | directive
           | error ';'                 { VP($$, vp_none(ctx, &$$)); }
           ;

item_node: OPAQUE_SPECIFY  { VP($$, vp_opaque(ctx, &@1, $1, ODIN3_AST_SPECIFY_BLOCK, &$$)); } ;

/* A.6.1 continuous assignment: CONT_ASSIGN(STRENGTH?, DELAY?; NET_ASSIGN+) (A1–A4). */
assign_item: ASSIGN assign_slots net_assigns ';'
               { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_CONT_ASSIGN, 0), &$$)); }
           ;

assign_slots: strength_opt delay_opt   { VP($$, vp_list_two(ctx, $1, $2, &$$)); } ;

net_assigns: net_assign                { VP($$, vp_list_first(ctx, $1, &$$)); }
           | net_assigns ',' net_assign { VP($$, vp_list_next(ctx, $1, $3, &$$)); }
           ;

net_assign: lvalue '=' expression      { VP($$, vp_net_assign(ctx, &@$, $1, $3, &$$)); } ;

/* ==== A.3–A.4 Instantiation and gates — Task 7 ==== */

/* ==== A.4.2 Generate — Task 7 ==== */

/* ==== A.2.6–A.2.7 Functions and tasks — Task 6 ==== */

/* ==== A.5, A.7 UDP, config and specify (opaque tokens) — Task 4 ==== */

/* The units are descriptions and SPECIFY_BLOCK an item_node above: each is one OPAQUE_* token
 * whose TEXT is the body from the keyword to its end keyword (§2.8). */

/* ==== A.6 Statements — Task 6 ==== */

/* ==== A.6.5 Timing controls — Task 6 ==== */

/* ==== A.8 Expressions and primaries — Task 5 ==== */

/* ---- placeholder (Task 4; Task 5 replaces it with the operator grammar) ---- */

expression: primary ;

primary: number
       | REAL_NUMBER                   { VP($$, vp_real(ctx, &@1, $1, &$$)); }
       | STRING                        { VP($$, vp_string(ctx, &@1, $1, &$$)); }
       | hier_ref                      { VP($$, vp_hier(ctx, $1, &$$)); }
       | '(' expression ')'            { VP($$, vp_pass(ctx, $2, &$$)); }
       ;

/* ---- A.8.5 names and lvalues (Task 4; Task 5's primaries reuse them) ---- */

/* §3.1 h: one hierarchical name reduced lazily; its components stay on the pending stack until
 * a use resolves them (one IDENT alone, else a HIER_NAME). A non-final component may carry one
 * bit select (G7); the item set after `[ e ]` reduces to an index on `.`, else to a select. */
hier_ref: IDENT                        { VP($$, vp_hier_first(ctx, &@1, $1, &$$)); }
        | hier_ref '.' IDENT           { VP($$, vp_hier_next(ctx, $1, &@3, $3, &$$)); }
        | hier_index '.' IDENT         { VP($$, vp_hier_next(ctx, $1, &@3, $3, &$$)); }
        ;

hier_index: hier_open expression ']'   { VP($$, vp_hier_index(ctx, $1, &@$, $2, &$$)); } ;

hier_open: hier_ref '['                { VP($$, vp_value(ctx, $1, &$$)); } ;

/* SELECT[bit|part|part_plus|part_minus](base, a, b?); chains nest, base first (N13). */
select: hier_open expression ']'       { VP($$, vp_select_hier(ctx, $1, &@$, $2, &$$)); }
      | select_open expression ']'     { VP($$, vp_select_bit(ctx, $1, &@$, $2, &$$)); }
      | select_part expression ']'     { VP($$, vp_select_part(ctx, $1, &@$, $2, &$$)); }
      | select_plus expression ']'     { VP($$, vp_select_plus(ctx, $1, &@$, $2, &$$)); }
      | select_minus expression ']'    { VP($$, vp_select_minus(ctx, $1, &@$, $2, &$$)); }
      ;

select_open: select '['                { VP($$, vp_list_first(ctx, $1, &$$)); } ;

select_part: hier_open expression ':'         { VP($$, vp_select_open(ctx, $1, $2, &$$)); }
           | select_open expression ':'       { VP($$, vp_list_next(ctx, $1, $2, &$$)); }
           ;

select_plus: hier_open expression PLUS_COLON  { VP($$, vp_select_open(ctx, $1, $2, &$$)); }
           | select_open expression PLUS_COLON { VP($$, vp_list_next(ctx, $1, $2, &$$)); }
           ;

select_minus: hier_open expression MINUS_COLON { VP($$, vp_select_open(ctx, $1, $2, &$$)); }
            | select_open expression MINUS_COLON { VP($$, vp_list_next(ctx, $1, $2, &$$)); }
            ;

/* net_lvalue and variable_lvalue (§3.1 d): a name with selects or a concatenation of them. */
lvalue: hier_ref                       { VP($$, vp_hier(ctx, $1, &$$)); }
      | select
      | lvalue_concat
      ;

lvalue_concat: '{' lvalues '}'         { VP($$, vp_concat(ctx, $2, &@$, &$$)); } ;

lvalues: lvalue                        { VP($$, vp_list_first(ctx, $1, &$$)); }
       | lvalues ',' lvalue            { VP($$, vp_list_next(ctx, $1, $3, &$$)); }
       ;

/* ---- A.8.7 numbers (Task 4): the three pieces go to the context's pieces stack (§2.3) ---- */

number: number_size                            { VP($$, vp_number(ctx, &@$, $1, &$$)); }
      | number_size number_base number_digits  { VP($$, vp_number(ctx, &@$, $1, &$$)); }
      | number_base number_digits              { VP($$, vp_number(ctx, &@$, $1, &$$)); }
      | number_size number_base                { VP_ERR(vp_digitless(ctx, &@2)); }
      | number_base                            { VP_ERR(vp_digitless(ctx, &@1)); }
      ;

number_size: UNSIGNED_NUMBER           { VP($$, vp_piece(ctx, $1, &$$)); } ;

number_base: BASE                      { VP($$, vp_piece(ctx, $1, &$$)); } ;

number_digits: BASED_DIGITS            { VP($$, vp_piece(ctx, $1, &$$)); } ;

/* ==== A.9 Attributes — Task 4 ==== */

/* Zero or more (* *) instances, their ATTRs pushed from the mark this yields; the rule that
 * makes the annotated node attaches them all at once (§3.5). */
attrs_opt: %empty                      { VP($$, vp_mark(ctx, &$$)); }
         | attrs_opt attr_instance     { VP($$, vp_value(ctx, $1, &$$)); }
         ;

attr_instance: ATTR_OPEN attr_specs ATTR_CLOSE   { VP($$, vp_value(ctx, $2, &$$)); } ;

attr_specs: attr_spec                  { VP($$, vp_list_first(ctx, $1, &$$)); }
          | attr_specs ',' attr_spec   { VP($$, vp_list_next(ctx, $1, $3, &$$)); }
          ;

attr_spec: IDENT                       { VP($$, vp_attr(ctx, &@$, $1, &$$)); }
         | IDENT '=' expression        { VP($$, vp_attr_value(ctx, &@$, $1, $3, &$$)); }
         ;

%%
