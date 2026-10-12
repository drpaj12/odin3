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

/* Task 5: 1364-2005 §5.1.2 Table 5-4, lowest first. Every binary operator associates left to
 * right, `**` included. Unary operators have none: they bind to a primary by the grammar's shape
 * (A.8.3). ':' has none: ranges, case items and labels use it, so a conflict there surfaces. */
%right '?'                              /* conditional: the rule's %prec '?' (§3.3)          */
%left  OR_OR                            /* ||                                                */
%left  AND_AND                          /* &&                                                */
%left  '|' NOR                          /* binary |  ~| (~| is not in A.8.6: Yosys, Odin II)  */
%left  '^' XNOR                         /* ^  ^~  ~^                                         */
%left  '&' NAND                         /* binary &  ~& (~& is not in A.8.6: Yosys, Odin II)  */
%left  EQ NE CASE_EQ CASE_NE            /* == != === !==                                     */
%left  '<' '>' LE GE                    /* < <= > >=                                         */
%left  SHL SHR ASHL ASHR                /* << >> <<< >>>                                     */
%left  '+' '-'                          /* binary + -                                        */
%left  '*' '/' '%'                      /* * / %                                             */
%left  POW                              /* **                                                */

/* Task 6: the dangling else (§3.3) — an `if` without `else` reduces below an `else`, so the
 * inner `if` takes it (shift), as Verilog, C and the printer assume. */
%precedence LOWER_THAN_ELSE
%precedence ELSE

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
%type <node> lvalue lvalue_concat select attr_spec number

/* ==== %type: Task 5 (expressions and primaries) ==== */

/* u: an `expr` (and an operand or argument list) is the mark below the node(s) it left on the
 * pending stack; an operator's spec (VP_SPEC); a callee's strtab name. node: a made node. */
%type <u> expr operand unary_op expr_list call_head hier_call_head system_head
%type <u> sys_args sys_arg sys_empty
%type <node> expression primary concat

/* ==== %type: Task 6 (statements, timing, functions and tasks, case and metacomments) ==== */

/* u: a pending-stack mark; a spec (VP_SPEC) or flag set; a ctx->headers or ctx->windows index.
 * node: a made node (0: none, or an absent optional statement). */
%type <u> function_spec function_type task_spec automatic_opt function_header task_header
%type <u> tf_items block_items case_head case_kw case_items if_head for_head for_cond for_header
%type <u> assign_lhs assign_lhs_nb assign_ctl ev_none ev_repeat ev_tail ev_exprs ev_item
%type <u> hier_task_name block_decl tf_ports tf_port_head tf_port_open
%type <node> tf_port_name
%type <node> function_decl task_decl tf_item block_item seq_block par_block
%type <node> statement stmt_or_null stmt_opt stmt_core case_stmt case_item for_assign
%type <node> delay_control event_control repeat_control always_item initial_item

/* ==== %type: Task 7 (instantiation, gates, generate) ==== */

/* u: a pending-stack mark (a list, an instance's array slot, the params slot); a spec (VP_SPEC:
 * GATE_DECL with its sub-kind); a ctx->windows index (the case generate). node: a made node. */
%type <u> inst_params instances inst_range inst_conns conns param_conns gate_item gate_kind
%type <u> pull_kind gate_strength gate_delay pull_strength gate_instances gate_terms gen_open
%type <u> gen_if_head gen_case_head gen_case_items
%type <node> instantiation module_instance connection conn_empty conn_or_empty gate_instance
%type <node> gate_term pull_one generate_region gen_block gen_body gen_body_or_null gen_item
%type <node> gen_for gen_if gen_case gen_case_item

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

/* The fourth error production (review of Task 4): an item missing its `;` before `endmodule`
 * recovers at the `endmodule`, so the next module's header is parsed, not discarded. */
module_body: list_mark module_params module_ports ';' module_items
               { VP($$, vp_value(ctx, $1, &$$)); }
           | list_mark module_params module_ports ';' module_items error
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
           | attrs_opt gate_item       { VP($$, vp_gate(ctx, $1, &@2, $2, &$$)); }  /* Task 7 */
           | defparam_list ';'         { VP($$, vp_none(ctx, &$$)); }
           | directive
           | error ';'                 { VP($$, vp_none(ctx, &$$)); }
           ;

item_node: OPAQUE_SPECIFY  { VP($$, vp_opaque(ctx, &@1, $1, ODIN3_AST_SPECIFY_BLOCK, &$$)); }
         | always_item     /* Task 6 */
         | initial_item    /* Task 6 */
         | function_decl   /* Task 6 */
         | task_decl       /* Task 6 */
         | instantiation   /* Task 7 */
         | generate_region /* Task 7 */
         | gen_for         /* Task 7 */
         | gen_if          /* Task 7 */
         | gen_case        /* Task 7 */
         ;

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

/* INSTANTIATION(name m; L? parameter CONNECTIONs; INSTANCE+) (I1–I7, P5). After the module name
 * the token decides: `#` a parameter override (always: §3.1 i, its values are expressions), `(`
 * an unnamed instance, an identifier a named one. A strength (`m (strong0, strong1) u (…)`) is
 * a syntax error: INSTANTIATION has no slot for it. */
instantiation: IDENT inst_params instances ';'
                 { VP($$, vp_instantiation(ctx, $2, &@$, $1, &$$)); }
             ;

/* The params slot: none, or a LIST of CONNECTIONs at `#` … `)` (`#()`: an empty one, printed
 * back as `#()`); ordered and named values may mix (2D judges, I2). */
inst_params: %empty                    { VP($$, vp_push_none(ctx, &$$)); }
           | '#' '(' list_mark ')'     { VP($$, vp_list_slot(ctx, &@$, $3, &$$)); }
           | '#' '(' list_mark param_conns ')'
                                       { VP($$, vp_list_slot(ctx, &@$, $3, &$$)); }
           ;

param_conns: connection                { VP($$, vp_list_first(ctx, $1, &$$)); }
           | param_conns ',' connection { VP($$, vp_list_next(ctx, $1, $3, &$$)); }
           ;

instances: module_instance             { VP($$, vp_list_first(ctx, $1, &$$)); }
         | instances ',' module_instance { VP($$, vp_list_next(ctx, $1, $3, &$$)); }
         ;

/* INSTANCE(name or 0; R? array (I6); C*): the array slot pushed, then the connections. An
 * unnamed instance is parsed for every module (a UDP instance may be unnamed); the pass enforces
 * 2A's rule 4 (§4.2 step 5). */
module_instance: IDENT inst_range '(' inst_conns ')'
                   { VP($$, vp_instance(ctx, $2, &@$, $1, &$$)); }
               | '(' list_mark inst_conns ')'
                   { VP($$, vp_instance_unnamed(ctx, $2, &@$, &$$)); }
               ;

inst_range: range_opt                  { VP($$, vp_push_slot(ctx, $1, &$$)); } ;

/* §3.1 k: `()` is zero connections, `(,)` two empty ones; the first entry may be empty only
 * before a comma. An empty slot is CONNECTION(0; none), zero width one past the `(` or `,`. */
inst_conns: %empty                     { VP($$, vp_mark(ctx, &$$)); }
          | conns
          ;

conns: connection                      { VP($$, vp_list_first(ctx, $1, &$$)); }
     | conn_empty ',' conn_or_empty    { VP($$, vp_list_two(ctx, $1, $3, &$$)); }
     | conns ',' conn_or_empty         { VP($$, vp_list_next(ctx, $1, $3, &$$)); }
     ;

conn_or_empty: conn_empty | connection ;

conn_empty: %empty           { VP($$, vp_connection(ctx, &@$, 0, VP_NO_NODE, &$$)); } ;

/* CONNECTION(name 0, E) ordered; (p, E) for `.p(e)`; (q; none) for `.q()` (I1). No attribute
 * (§3.5: CONNECTION has no attach in 2A §7). */
connection: expression                 { VP($$, vp_connection(ctx, &@$, 0, $1, &$$)); }
          | '.' IDENT '(' ')'          { VP($$, vp_connection(ctx, &@$, $2, VP_NO_NODE, &$$)); }
          | '.' IDENT '(' expression ')'
                                       { VP($$, vp_connection(ctx, &@$, $2, $4, &$$)); }
          ;

/* GATE_DECL[kind](STRENGTH?, DELAY?; INSTANCE+) (Q1–Q6), the §3.1 g expanded form: nothing
 * nullable precedes a `(`, so after `kind (` a strength keyword or an expression decides. The
 * strength, delay and instances are pushed in source order; vp_gate places the two slots. */
gate_item: gate_kind gate_instances ';'                     { VP($$, vp_value(ctx, $1, &$$)); }
         | gate_kind gate_delay gate_instances ';'          { VP($$, vp_value(ctx, $1, &$$)); }
         | gate_kind gate_strength gate_instances ';'       { VP($$, vp_value(ctx, $1, &$$)); }
         | gate_kind gate_strength gate_delay gate_instances ';'
                                                            { VP($$, vp_value(ctx, $1, &$$)); }
         | pull_kind gate_instances ';'                     { VP($$, vp_value(ctx, $1, &$$)); }
         | pull_kind pull_strength gate_instances ';'       { VP($$, vp_value(ctx, $1, &$$)); }
         ;

gate_strength: drive_strength          { VP($$, vp_push_slot(ctx, $1, &$$)); } ;

gate_delay: delay3                     { VP($$, vp_push_slot(ctx, $1, &$$)); } ;

/* A.3.1 pullup_strength / pulldown_strength: a pair or one strength (the other field 0). */
pull_strength: drive_strength          { VP($$, vp_push_slot(ctx, $1, &$$)); }
             | pull_one                { VP($$, vp_push_slot(ctx, $1, &$$)); }
             ;

pull_one: '(' strength0 ')'            { VP($$, vp_leaf(ctx, &@$, $2, &$$)); }
        | '(' strength1 ')'            { VP($$, vp_leaf(ctx, &@$, $2, &$$)); }
        ;

gate_instances: gate_instance          { VP($$, vp_list_first(ctx, $1, &$$)); }
              | gate_instances ',' gate_instance { VP($$, vp_list_next(ctx, $1, $3, &$$)); }
              ;

/* INSTANCE(name opt; R? array (Q5); one CONNECTION(0, E) per terminal); terminals are never
 * empty (A.3.3). */
gate_instance: IDENT inst_range '(' gate_terms ')'
                 { VP($$, vp_instance(ctx, $2, &@$, $1, &$$)); }
             | '(' list_mark gate_terms ')'
                 { VP($$, vp_instance_unnamed(ctx, $2, &@$, &$$)); }
             ;

gate_terms: gate_term                  { VP($$, vp_list_first(ctx, $1, &$$)); }
          | gate_terms ',' gate_term   { VP($$, vp_list_next(ctx, $1, $3, &$$)); }
          ;

gate_term: expression                  { VP($$, vp_connection(ctx, &@$, 0, $1, &$$)); } ;

gate_kind: AND      { VP($$, vp_value(ctx, VP_GATE(AND), &$$)); }
         | KW_NAND  { VP($$, vp_value(ctx, VP_GATE(NAND), &$$)); }
         | OR       { VP($$, vp_value(ctx, VP_GATE(OR), &$$)); }
         | KW_NOR   { VP($$, vp_value(ctx, VP_GATE(NOR), &$$)); }
         | XOR      { VP($$, vp_value(ctx, VP_GATE(XOR), &$$)); }
         | KW_XNOR  { VP($$, vp_value(ctx, VP_GATE(XNOR), &$$)); }
         | BUF      { VP($$, vp_value(ctx, VP_GATE(BUF), &$$)); }
         | NOT      { VP($$, vp_value(ctx, VP_GATE(NOT), &$$)); }
         | BUFIF0   { VP($$, vp_value(ctx, VP_GATE(BUFIF0), &$$)); }
         | BUFIF1   { VP($$, vp_value(ctx, VP_GATE(BUFIF1), &$$)); }
         | NOTIF0   { VP($$, vp_value(ctx, VP_GATE(NOTIF0), &$$)); }
         | NOTIF1   { VP($$, vp_value(ctx, VP_GATE(NOTIF1), &$$)); }
         | NMOS     { VP($$, vp_value(ctx, VP_GATE(NMOS), &$$)); }
         | PMOS     { VP($$, vp_value(ctx, VP_GATE(PMOS), &$$)); }
         | CMOS     { VP($$, vp_value(ctx, VP_GATE(CMOS), &$$)); }
         | RNMOS    { VP($$, vp_value(ctx, VP_GATE(RNMOS), &$$)); }
         | RPMOS    { VP($$, vp_value(ctx, VP_GATE(RPMOS), &$$)); }
         | RCMOS    { VP($$, vp_value(ctx, VP_GATE(RCMOS), &$$)); }
         | TRAN     { VP($$, vp_value(ctx, VP_GATE(TRAN), &$$)); }
         | RTRAN    { VP($$, vp_value(ctx, VP_GATE(RTRAN), &$$)); }
         | TRANIF0  { VP($$, vp_value(ctx, VP_GATE(TRANIF0), &$$)); }
         | TRANIF1  { VP($$, vp_value(ctx, VP_GATE(TRANIF1), &$$)); }
         | RTRANIF0 { VP($$, vp_value(ctx, VP_GATE(RTRANIF0), &$$)); }
         | RTRANIF1 { VP($$, vp_value(ctx, VP_GATE(RTRANIF1), &$$)); }
         ;

pull_kind: PULLUP   { VP($$, vp_value(ctx, VP_GATE(PULLUP), &$$)); }
         | PULLDOWN { VP($$, vp_value(ctx, VP_GATE(PULLDOWN), &$$)); }
         ;

/* ==== A.4.2 Generate — Task 7 ==== */

/* GENERATE(I*): any number of items, genvar declarations included (G1); the region's items are
 * the module's (2D judges the ones Annex A leaves out of a region). The error alternative, like
 * the closer twins of §3.4, resynchronises an item missing its `;` at `endgenerate`. */
generate_region: GENERATE module_items ENDGENERATE
                   { VP($$, vp_generate(ctx, $2, &@$, &$$)); }
               | GENERATE module_items error ENDGENERATE
                   { VP($$, vp_none(ctx, &$$)); }
               ;

/* GEN_FOR(BLOCKING_ASSIGN, E, BLOCKING_ASSIGN, I) (G2–G4): Task 6's `for` header; the body is a
 * block or one bare item, never null (A.4.2 generate_block). */
gen_for: for_header gen_body           { VP($$, vp_gen_for(ctx, $1, &@$, $2, &$$)); } ;

/* GEN_IF(E, I, I?) (G5); the dangling else by precedence, as the statement `if` (§3.3). */
gen_if: gen_if_head %prec LOWER_THAN_ELSE   { VP($$, vp_gen_if(ctx, $1, &@$, VP_NO_NODE, &$$)); }
      | gen_if_head ELSE gen_body_or_null   { VP($$, vp_gen_if(ctx, $1, &@$, $3, &$$)); }
      ;

gen_if_head: IF '(' expression ')' gen_body_or_null
               { VP($$, vp_list_two(ctx, $3, $5, &$$)); }
           ;

/* GEN_CASE(E; CASE_ITEM+ with item bodies) (G6): `case` only (A.4.2), the header's metacomment
 * window and the item reduction as CASE's (§2.6, §3.2). */
gen_case: gen_case_head gen_case_items ENDCASE
            { VP($$, vp_make_case(ctx, $1, &@$, $2, &$$)); }
        | gen_case_head error ENDCASE             { VP($$, vp_none(ctx, &$$)); }
        | gen_case_head gen_case_items error ENDCASE  { VP($$, vp_none(ctx, &$$)); }
        ;

gen_case_head: CASE '(' expression ')' { VP($$, vp_gen_case(ctx, &@4, $3, &$$)); } ;

gen_case_items: gen_case_item          { VP($$, vp_case_first(ctx, &@1, $1, &$$)); }
              | gen_case_items gen_case_item { VP($$, vp_list_next(ctx, $1, $2, &$$)); }
              ;

gen_case_item: expr_list ':' gen_body_or_null
                 { VP($$, vp_make_case_item(ctx, &@$, $1, $3, &$$)); }
             | DEFAULT list_mark gen_body_or_null
                 { VP($$, vp_make_case_item(ctx, &@$, $2, $3, &$$)); }
             | DEFAULT list_mark ':' gen_body_or_null
                 { VP($$, vp_make_case_item(ctx, &@$, $2, $4, &$$)); }
             ;

/* A.4.2 generate_block_or_null: `;` is a NULL_STMT (G5). */
gen_body_or_null: gen_body
                | ';'                  { VP($$, vp_leaf(ctx, &@1, VP_SPEC(ODIN3_AST_NULL_STMT, 0), &$$)); }
                ;

/* A.4.2 generate_block: GEN_BLOCK(name or 0; no index in the parsed form, 2A §4.6; I*), or one
 * bare item. No name is made up here: 2D numbers the unnamed blocks (genblk<n>). */
gen_body: gen_item
        | gen_block
        ;

gen_block: BEGIN gen_open module_items END
             { VP($$, vp_gen_block(ctx, $2, &@$, 0, &$$)); }
         | BEGIN ':' IDENT gen_open module_items END
             { VP($$, vp_gen_block(ctx, $4, &@$, $3, &$$)); }
         | BEGIN gen_open module_items error END            { VP($$, vp_none(ctx, &$$)); }
         | BEGIN ':' IDENT gen_open module_items error END  { VP($$, vp_none(ctx, &$$)); }
         ;

gen_open: %empty                       { VP($$, vp_push_none(ctx, &$$)); } ;

/* One bare item (a module item's forms with its attributes; a defparam of one pair, since the
 * body is one node). */
gen_item: attrs_opt item_decl          { VP($$, vp_item(ctx, $1, &@2, $2, &$$)); }
        | attrs_opt item_node          { VP($$, vp_attrs(ctx, $1, $2, &$$)); }
        | attrs_opt gate_item          { VP($$, vp_gate(ctx, $1, &@2, $2, &$$)); }
        | attrs_opt DEFPARAM defparam_assign ';'   { VP($$, vp_attrs(ctx, $1, $3, &$$)); }
        ;

/* ==== A.2.6–A.2.7 Functions and tasks — Task 6 ==== */

/* FUNCTION_DECL(name; {AUTOMATIC SIGNED DT}; R? return; L ports; L locals; S) and TASK_DECL(name;
 * {AUTOMATIC}; L ports; L locals; S). From the mark: the return range (functions), the ANSI
 * header's PORT_DECL{IN_HEADER}s, then the body items in source order; vp_make_function
 * partitions them (§3.2). The header record (name, flags, the header's end) is ctx->headers[$3]. */
function_decl: FUNCTION list_mark function_header tf_items ENDFUNCTION
                 { VP($$, vp_make_function(ctx, $2, &@$, $3, &$$)); }
             | FUNCTION list_mark function_header tf_items error ENDFUNCTION
                 { VP($$, vp_none(ctx, &$$)); }
             ;

task_decl: TASK list_mark task_header tf_items ENDTASK
             { VP($$, vp_make_function(ctx, $2, &@$, $3, &$$)); }
         | TASK list_mark task_header tf_items error ENDTASK
             { VP($$, vp_none(ctx, &$$)); }
         ;

function_header: function_spec IDENT ';'
                   { VP($$, vp_tf_header(ctx, $1, &@3, $2, &$$)); }
               | function_spec IDENT '(' tf_ports ')' ';'
                   { VP($$, vp_tf_header_ansi(ctx, $1, &@6, $2, &$$)); }
               ;

task_header: task_spec IDENT ';'       { VP($$, vp_tf_header(ctx, $1, &@3, $2, &$$)); }
           | task_spec IDENT '(' ')' ';'
                                       { VP($$, vp_tf_header(ctx, $1, &@5, $2, &$$)); }
           | task_spec IDENT '(' tf_ports ')' ';'
                                       { VP($$, vp_tf_header_ansi(ctx, $1, &@6, $2, &$$)); }
           ;

/* The ANSI header ports (A.2.6 function_port_list, A.2.7 task_port_list; `task t ();` has
 * none): the module header's groups (Task 4) without attributes, which §3.5 does not support
 * on a function or task port, and without initialisers (A.2.7 tf_*_declaration). */
tf_ports: tf_port_head
        | tf_ports ',' tf_port_head
        | tf_ports ',' tf_port_name            { VP($$, vp_group_join(ctx, $1, $3, &$$)); }
        ;

tf_port_head: tf_port_open range_opt tf_port_name
                { VP($$, vp_group_first(ctx, $2, $3, &$$)); }
            ;

tf_port_name: IDENT                    { VP($$, vp_decl_name(ctx, &@$, $1, &$$)); } ;

tf_port_open: group_close list_mark port_spec
                { VP($$, vp_group_open(ctx, $2, &@3, $3, &$$)); }
            ;

function_spec: automatic_opt function_type   { VP($$, vp_or(ctx, $1, $2, &$$)); } ;

task_spec: automatic_opt   { VP($$, vp_or(ctx, VP_SPEC(ODIN3_AST_TASK_DECL, 0), $1, &$$)); } ;

/* AUTOMATIC is bit 0 of FUNCTION_DECL and TASK_DECL alike (actions_stmt.c asserts it). */
automatic_opt: %empty      { VP($$, vp_value(ctx, VP_NO_FLAGS, &$$)); }
             | AUTOMATIC   { VP($$, vp_value(ctx, VP_FLAGS(ODIN3_AST_F_FUNCTION_DECL_AUTOMATIC), &$$)); }
             ;

/* A.2.6 function_range_or_type: [signed] [range] (the range pushed, 0 when absent) or a type. */
function_type: signed_opt range_opt    { VP($$, vp_function_range(ctx, $1, $2, &$$)); }
             | INTEGER                 { VP($$, vp_function_dt(ctx, ODIN3_AST_DT_INTEGER, &$$)); }
             | REAL                    { VP($$, vp_function_dt(ctx, ODIN3_AST_DT_REAL, &$$)); }
             | REALTIME                { VP($$, vp_function_dt(ctx, ODIN3_AST_DT_REALTIME, &$$)); }
             | TIME                    { VP($$, vp_function_dt(ctx, ODIN3_AST_DT_TIME, &$$)); }
             ;

/* Function and task body items: a port declaration (A.2.7 tf_*_declaration) or a block item. */
tf_items: %empty                       { VP($$, vp_mark(ctx, &$$)); }
        | tf_items tf_item             { VP($$, vp_item_next(ctx, $1, $2, &$$)); }
        ;

tf_item: block_item
       | attrs_opt port_item           { VP($$, vp_item(ctx, $1, &@2, $2, &$$)); }
       ;

/* ==== A.5, A.7 UDP, config and specify (opaque tokens) — Task 4 ==== */

/* The units are descriptions and SPECIFY_BLOCK an item_node above: each is one OPAQUE_* token
 * whose TEXT is the body from the keyword to its end keyword (§2.8). */

/* ==== A.6 Statements — Task 6 ==== */

/* ALWAYS[always](S), INITIAL(S) (§3.6 "Procedural blocks and timing"). */
always_item: ALWAYS statement          { VP($$, vp_always(ctx, &@$, $2, &$$)); } ;

initial_item: INITIAL statement        { VP($$, vp_initial(ctx, &@$, $2, &$$)); } ;

/* §3.1 e: one mixed list of block items, attributes first, for begin/fork (and, with port
 * declarations, for function and task bodies); vp_make_block partitions it (§3.2). */
block_items: %empty                    { VP($$, vp_mark(ctx, &$$)); }
           | block_items block_item    { VP($$, vp_item_next(ctx, $1, $2, &$$)); }
           ;

block_item: attrs_opt block_decl       { VP($$, vp_item(ctx, $1, &@2, $2, &$$)); }
          | attrs_opt stmt_core        { VP($$, vp_attrs(ctx, $1, $2, &$$)); }
          | attrs_opt ';'              { VP($$, vp_null(ctx, $1, &@2, &$$)); }
          | error ';'                  { VP($$, vp_none(ctx, &$$)); }
          ;

/* A.2.8 block_item_declaration: reg, integer, time, real, realtime, event, parameter. */
block_decl: var_item | event_item | param_item ;

/* SEQ_BLOCK / PAR_BLOCK(name; L? locals; S*). */
seq_block: BEGIN block_items END              { VP($$, vp_make_block(ctx, $2, &@$, 0, &$$)); }
         | BEGIN ':' IDENT block_items END    { VP($$, vp_make_block(ctx, $4, &@$, $3, &$$)); }
         | BEGIN block_items error END        { VP($$, vp_none(ctx, &$$)); }
         | BEGIN ':' IDENT block_items error END   { VP($$, vp_none(ctx, &$$)); }
         ;

par_block: FORK block_items JOIN              { VP($$, vp_make_fork(ctx, $2, &@$, 0, &$$)); }
         | FORK ':' IDENT block_items JOIN    { VP($$, vp_make_fork(ctx, $4, &@$, $3, &$$)); }
         | FORK block_items error JOIN        { VP($$, vp_none(ctx, &$$)); }
         | FORK ':' IDENT block_items error JOIN   { VP($$, vp_none(ctx, &$$)); }
         ;

/* A statement and its attributes (§3.5): the node is made without them (point (g)). The error
 * production resynchronises at the next `;` inside a procedural block (§3.4); an error just
 * before a closer without a `;` (`end`, `join`, `endcase`, `endfunction`, `endtask`)
 * resynchronises at that closer through the error alternatives of the block, case, function
 * and task rules, so the item after it is still parsed (Task 6 review #8). */
statement: attrs_opt stmt_core         { VP($$, vp_attrs(ctx, $1, $2, &$$)); }
         | error ';'                   { VP($$, vp_none(ctx, &$$)); }
         ;

/* A.6.4 statement_or_null: `;` is a NULL_STMT (T20) ... */
stmt_or_null: statement
            | attrs_opt ';'            { VP($$, vp_null(ctx, $1, &@2, &$$)); }
            ;

/* ... except as the S? of TIMING_STMT and WAIT, where a bare `;` leaves it absent. */
stmt_opt: statement
        | attrs_opt ';'                { VP($$, vp_null_opt(ctx, $1, &@2, &$$)); }
        ;

stmt_core: assign_lhs assign_ctl expression ';'
             { VP($$, vp_blocking(ctx, $1, &@$, $3, &$$)); }
         | assign_lhs_nb assign_ctl expression ';'
             { VP($$, vp_nonblocking(ctx, $1, &@$, $3, &$$)); }
         | ASSIGN lvalue '=' expression ';'   { VP($$, vp_proc_assign(ctx, &@$, $2, $4, &$$)); }
         | FORCE lvalue '=' expression ';'    { VP($$, vp_proc_force(ctx, &@$, $2, $4, &$$)); }
         | DEASSIGN lvalue ';'                { VP($$, vp_proc_deassign(ctx, &@$, $2, &$$)); }
         | RELEASE lvalue ';'                 { VP($$, vp_proc_release(ctx, &@$, $2, &$$)); }
         | if_head %prec LOWER_THAN_ELSE      { VP($$, vp_if(ctx, $1, &@$, VP_NO_NODE, &$$)); }
         | if_head ELSE stmt_or_null          { VP($$, vp_if(ctx, $1, &@$, $3, &$$)); }
         | case_stmt
         | for_header stmt_or_null              { VP($$, vp_for(ctx, $1, &@$, $2, &$$)); }
         | WHILE '(' expression ')' stmt_or_null   { VP($$, vp_while(ctx, &@$, $3, $5, &$$)); }
         | REPEAT '(' expression ')' stmt_or_null  { VP($$, vp_repeat(ctx, &@$, $3, $5, &$$)); }
         | FOREVER stmt_or_null               { VP($$, vp_forever(ctx, &@$, $2, &$$)); }
         | WAIT '(' expression ')' stmt_opt   { VP($$, vp_wait(ctx, &@$, $3, $5, &$$)); }
         | delay_control stmt_opt             { VP($$, vp_timing(ctx, &@$, $1, $2, &$$)); }
         | event_control stmt_opt             { VP($$, vp_timing(ctx, &@$, $1, $2, &$$)); }
         | DISABLE hier_ref ';'               { VP($$, vp_disable(ctx, $2, &@$, &$$)); }
         | TRIGGER hier_ref ';'               { VP($$, vp_trigger(ctx, $2, &@$, &$$)); }
         | IDENT list_mark ';'                { VP($$, vp_task_call(ctx, $2, &@$, $1, &$$)); }
         | call_head expr_list ')' ';'        { VP($$, vp_task_call(ctx, $2, &@$, $1, &$$)); }
         | hier_task_name list_mark ';'       { VP($$, vp_task_call_hier(ctx, $2, &@$, $1, &$$)); }
         | hier_call_head expr_list ')' ';'   { VP($$, vp_task_call_hier(ctx, $2, &@$, $1, &$$)); }
         | SYSTEM_IDENT list_mark ';'         { VP($$, vp_task_call_system(ctx, $2, &@$, $1, &$$)); }
         | system_head list_mark ')' ';'      { VP($$, vp_task_call_system(ctx, $2, &@$, $1, &$$)); }
         | system_head sys_args ')' ';'       { VP($$, vp_task_call_system(ctx, $2, &@$, $1, &$$)); }
         | seq_block
         | par_block
         ;

/* §3.1 d: the targets are lvalues, never expressions, so `<=` after one is the assignment. The
 * value is the mark below the lvalue; the intra-assignment control (or 0) follows it. */
assign_lhs: lvalue '='                 { VP($$, vp_list_first(ctx, $1, &$$)); } ;

assign_lhs_nb: lvalue LE               { VP($$, vp_list_first(ctx, $1, &$$)); } ;

assign_ctl: %empty                     { VP($$, vp_push_none(ctx, &$$)); }
          | delay_control              { VP($$, vp_push_slot(ctx, $1, &$$)); }
          | event_control              { VP($$, vp_push_slot(ctx, $1, &$$)); }
          | repeat_control             { VP($$, vp_push_slot(ctx, $1, &$$)); }
          ;

/* §3.1 h: a task enable is decided on the token after its name: `;`, `(`, or (a hierarchical
 * one) after the last component. CALL{HIER}'s name rule (§3.6). */
hier_task_name: hier_ref '.' IDENT     { VP($$, vp_hier_call_name(ctx, $1, $3, &$$)); }
              | hier_index '.' IDENT   { VP($$, vp_hier_call_name(ctx, $1, $3, &$$)); }
              ;

/* IF(E, S, S?): the condition and the then-branch pushed; the dangling else by precedence. */
if_head: IF '(' expression ')' stmt_or_null   { VP($$, vp_list_two(ctx, $3, $5, &$$)); } ;

/* FOR(BLOCKING_ASSIGN, E, BLOCKING_ASSIGN, S) (F1); the three heads pushed in order. */
for_head: FOR '(' for_assign ';'       { VP($$, vp_list_first(ctx, $3, &$$)); } ;

for_cond: for_head expression ';'      { VP($$, vp_list_next(ctx, $1, $2, &$$)); } ;

for_header: for_cond for_assign ')'      { VP($$, vp_list_next(ctx, $1, $2, &$$)); } ;

for_assign: lvalue '=' expression      { VP($$, vp_assign_plain(ctx, &@$, $1, $3, &$$)); } ;

/* CASE[case|casez|casex](E; CASE_ITEM+); the metacomment window (§2.6) runs from one past the
 * header's `)` to the first item, recorded in ctx->windows[$2] by case_head and case_items. */
case_stmt: case_head case_items ENDCASE
             { VP($$, vp_make_case(ctx, $1, &@$, $2, &$$)); }
         | case_head error ENDCASE              { VP($$, vp_none(ctx, &$$)); }
         | case_head case_items error ENDCASE   { VP($$, vp_none(ctx, &$$)); }
         ;

case_head: case_kw '(' expression ')'  { VP($$, vp_case_head(ctx, &@4, $1, $3, &$$)); } ;

case_kw: CASE      { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_CASE, ODIN3_AST_CASE_CASE), &$$)); }
       | CASEZ     { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_CASE, ODIN3_AST_CASE_CASEZ), &$$)); }
       | CASEX     { VP($$, vp_value(ctx, VP_SPEC(ODIN3_AST_CASE, ODIN3_AST_CASE_CASEX), &$$)); }
       ;

case_items: case_item                  { VP($$, vp_case_first(ctx, &@1, $1, &$$)); }
          | case_items case_item       { VP($$, vp_list_next(ctx, $1, $2, &$$)); }
          ;

/* CASE_ITEM{DEFAULT}(S; E*): the body first, then the match expressions (2A §4.2 convention 4);
 * `default` with or without a colon has an empty tail (C6, C7). */
case_item: expr_list ':' stmt_or_null          { VP($$, vp_make_case_item(ctx, &@$, $1, $3, &$$)); }
         | DEFAULT list_mark stmt_or_null      { VP($$, vp_make_case_item(ctx, &@$, $2, $3, &$$)); }
         | DEFAULT list_mark ':' stmt_or_null  { VP($$, vp_make_case_item(ctx, &@$, $2, $4, &$$)); }
         ;

/* ==== A.6.5 Timing controls — Task 6 ==== */

/* §3.1 f: `#` takes a delay value (a number, a real, an identifier) or one parenthesised
 * min:typ:max expression — never a bare expression. DELAY(E) over the `#` (A.6.5). */
delay_control: '#' delay_value         { VP($$, vp_delay1(ctx, &@$, $2, &$$)); }
             | '#' '(' mintypmax ')'   { VP($$, vp_delay1(ctx, &@$, $3, &$$)); }
             ;

/* EVENT_CONTROL{STAR NO_PARENS}(E? repeat count; EVENT_EXPR*): the slot pushed first (0, or the
 * count of an intra-assignment `repeat (n)`), then the tail; ev_tail yields the flags. */
event_control: ev_none ev_tail         { VP($$, vp_event_control(ctx, $1, &@$, $2, &$$)); } ;

repeat_control: ev_repeat ev_tail      { VP($$, vp_event_control(ctx, $1, &@$, $2, &$$)); } ;

ev_none: %empty                        { VP($$, vp_push_none(ctx, &$$)); } ;

ev_repeat: REPEAT '(' expression ')'   { VP($$, vp_push_slot(ctx, $3, &$$)); } ;

/* `@name` (T4, NO_PARENS), `@( list )`, and the implicit event: `@*` / `@(*)` as one token
 * (§2.7) and the three forms a comment inside the parentheses defeats. */
ev_tail: '@' hier_ref                  { VP($$, vp_event_name(ctx, $2, &$$)); }
       | '@' '(' ev_exprs ')'          { VP($$, vp_value(ctx, VP_NO_FLAGS, &$$)); }
       | AT_STAR                       { VP($$, vp_value(ctx, ODIN3_AST_F_EVENT_CONTROL_STAR, &$$)); }
       | '@' '(' '*' ')'               { VP($$, vp_value(ctx, ODIN3_AST_F_EVENT_CONTROL_STAR, &$$)); }
       | '@' ATTR_OPEN ')'             { VP($$, vp_value(ctx, ODIN3_AST_F_EVENT_CONTROL_STAR, &$$)); }
       | '@' '(' ATTR_CLOSE            { VP($$, vp_value(ctx, ODIN3_AST_F_EVENT_CONTROL_STAR, &$$)); }
       ;

/* EVENT_EXPR[none|posedge|negedge]{AFTER_OR}(E), each at its own first token; AFTER_OR on an
 * expression that followed `or` rather than `,` (T1, T2; printer only). */
ev_exprs: ev_item                      { VP($$, vp_event_first(ctx, &@1, $1, &$$)); }
        | ev_exprs ',' ev_item         { VP($$, vp_event_next(ctx, $1, &@3, $3, &$$)); }
        | ev_exprs OR ev_item          { VP($$, vp_event_or(ctx, $1, &@3, $3, &$$)); }
        ;

ev_item: expression        { VP($$, vp_spec_slot(ctx, VP_SPEC(ODIN3_AST_EVENT_EXPR, ODIN3_AST_EVENT_EXPR_NONE), $1, &$$)); }
       | POSEDGE expression { VP($$, vp_spec_slot(ctx, VP_SPEC(ODIN3_AST_EVENT_EXPR, ODIN3_AST_EVENT_EXPR_POSEDGE), $2, &$$)); }
       | NEGEDGE expression { VP($$, vp_spec_slot(ctx, VP_SPEC(ODIN3_AST_EVENT_EXPR, ODIN3_AST_EVENT_EXPR_NEGEDGE), $2, &$$)); }
       ;

/* ==== A.8 Expressions and primaries — Task 5 ==== */

/* ---- A.8.3 expressions (Task 5) ---- */

/* An expression's node. Inside the operator rules an `expr` leaves its node on the pending stack
 * and its value is the mark below it, so an operator node is made from one pending span:
 * [left operand] [the operator's ATTRs] [right operands], in source order (plan point (s): a
 * vp_* takes at most two values; §3.5: attributes follow the operator). */
expression: expr                       { VP($$, vp_expr(ctx, $1, &$$)); } ;

/* Each binary rule carries its operator token, so the rule has that token's precedence (§3.3);
 * a unary operator takes a primary only: `- -a` is a syntax error, `-a ** b` is `(-a) ** b`. The
 * conditional's last terminal is ':', which has no precedence, hence the explicit %prec. */
expr: operand
    | unary_op attrs_opt operand       { VP($$, vp_unary(ctx, &@$, $2, $1, &$$)); }
    | expr OR_OR attrs_opt expr        { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(LOR), &$$)); }
    | expr AND_AND attrs_opt expr      { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(LAND), &$$)); }
    | expr '|' attrs_opt expr          { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(OR), &$$)); }
    | expr NOR attrs_opt expr          { VP($$, vp_binary_not(ctx, &@$, $1, VP_BINARY(OR), &$$)); }
    | expr '^' attrs_opt expr          { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(XOR), &$$)); }
    | expr XNOR attrs_opt expr         { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(XNOR), &$$)); }
    | expr '&' attrs_opt expr          { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(AND), &$$)); }
    | expr NAND attrs_opt expr         { VP($$, vp_binary_not(ctx, &@$, $1, VP_BINARY(AND), &$$)); }
    | expr EQ attrs_opt expr           { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(EQ), &$$)); }
    | expr NE attrs_opt expr           { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(NE), &$$)); }
    | expr CASE_EQ attrs_opt expr      { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(CASE_EQ), &$$)); }
    | expr CASE_NE attrs_opt expr      { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(CASE_NE), &$$)); }
    | expr '<' attrs_opt expr          { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(LT), &$$)); }
    | expr '>' attrs_opt expr          { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(GT), &$$)); }
    | expr LE attrs_opt expr           { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(LE), &$$)); }
    | expr GE attrs_opt expr           { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(GE), &$$)); }
    | expr SHL attrs_opt expr          { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(SHL), &$$)); }
    | expr SHR attrs_opt expr          { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(SHR), &$$)); }
    | expr ASHL attrs_opt expr         { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(ASHL), &$$)); }
    | expr ASHR attrs_opt expr         { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(ASHR), &$$)); }
    | expr '+' attrs_opt expr          { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(ADD), &$$)); }
    | expr '-' attrs_opt expr          { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(SUB), &$$)); }
    | expr '*' attrs_opt expr          { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(MUL), &$$)); }
    | expr '/' attrs_opt expr          { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(DIV), &$$)); }
    | expr '%' attrs_opt expr          { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(MOD), &$$)); }
    | expr POW attrs_opt expr          { VP($$, vp_binary(ctx, &@$, $1, VP_BINARY(POW), &$$)); }
    | expr '?' attrs_opt expr ':' expr %prec '?'
                                       { VP($$, vp_ternary(ctx, &@$, $1, &$$)); }
    ;

/* UNARY[op] (A.8.6): `^~` lexes as XNOR, so it is stored as `~^`; unary `+` is kept (E4). */
unary_op: '+'                          { VP($$, vp_value(ctx, VP_UNARY(PLUS), &$$)); }
        | '-'                          { VP($$, vp_value(ctx, VP_UNARY(MINUS), &$$)); }
        | '!'                          { VP($$, vp_value(ctx, VP_UNARY(LNOT), &$$)); }
        | '~'                          { VP($$, vp_value(ctx, VP_UNARY(NOT), &$$)); }
        | '&'                          { VP($$, vp_value(ctx, VP_UNARY(AND), &$$)); }
        | NAND                         { VP($$, vp_value(ctx, VP_UNARY(NAND), &$$)); }
        | '|'                          { VP($$, vp_value(ctx, VP_UNARY(OR), &$$)); }
        | NOR                          { VP($$, vp_value(ctx, VP_UNARY(NOR), &$$)); }
        | '^'                          { VP($$, vp_value(ctx, VP_UNARY(XOR), &$$)); }
        | XNOR                         { VP($$, vp_value(ctx, VP_UNARY(XNOR), &$$)); }
        ;

/* A primary as an operator operand: its node pushed, the mark below it its value. */
operand: primary                       { VP($$, vp_list_first(ctx, $1, &$$)); } ;

/* Expressions left on the pending stack in source order; the value is the first one's mark. */
expr_list: expr
         | expr_list ',' expr          { VP($$, vp_value(ctx, $1, &$$)); }
         ;

/* ---- A.8.4 primaries (Task 5) ---- */

/* Parentheses make no node (§3.6). */
primary: number
       | REAL_NUMBER                   { VP($$, vp_real(ctx, &@1, $1, &$$)); }
       | STRING                        { VP($$, vp_string(ctx, &@1, $1, &$$)); }
       | hier_ref                      { VP($$, vp_hier(ctx, $1, &$$)); }
       | select
       | concat
       | '{' expr concat '}'           { VP($$, vp_replicate(ctx, &@$, $2, $3, &$$)); }
       | call_head expr_list ')'       { VP($$, vp_call(ctx, $2, &@$, $1, &$$)); }
       | hier_call_head expr_list ')'  { VP($$, vp_call_hier(ctx, $2, &@$, $1, &$$)); }
       | SYSTEM_IDENT list_mark        { VP($$, vp_call_system(ctx, $2, &@$, $1, &$$)); }
       | system_head list_mark ')'     { VP($$, vp_call_system(ctx, $2, &@$, $1, &$$)); }
       | system_head sys_args ')'      { VP($$, vp_call_system(ctx, $2, &@$, $1, &$$)); }
       | '(' expression ')'            { VP($$, vp_pass(ctx, $2, &$$)); }
       ;

/* CONCAT(E+); a replication's inner list is always a CONCAT (E12, E13). */
concat: '{' expr_list '}'              { VP($$, vp_concat(ctx, $2, &@$, &$$)); } ;

/* CALL(name; E+) (A.8.2: a user call has at least one argument, none empty, §3.1 j). The callee
 * is taken from its tokens before any node is made for it: `IDENT (` is decided on the `(`. */
call_head: IDENT '('                   { VP($$, vp_value(ctx, $1, &$$)); } ;

/* CALL{HIER}: the components as the printer spells them, joined with `.` (§3.6). */
hier_call_head: hier_ref '.' IDENT '('    { VP($$, vp_hier_call_name(ctx, $1, $3, &$$)); }
              | hier_index '.' IDENT '('  { VP($$, vp_hier_call_name(ctx, $1, $3, &$$)); }
              ;

/* CALL{SYSTEM}(name with `$`; E*0): `$time`, `$random()` and `$f(a, , b)` (§3.1 k's list shape:
 * the first argument may be empty only before a comma; an empty argument is a 0 entry). */
system_head: SYSTEM_IDENT '('          { VP($$, vp_value(ctx, $1, &$$)); } ;

sys_args: expr
        | sys_empty ',' sys_arg        { VP($$, vp_value(ctx, $1, &$$)); }
        | sys_args ',' sys_arg         { VP($$, vp_value(ctx, $1, &$$)); }
        ;

sys_arg: sys_empty | expr ;

sys_empty: %empty                      { VP($$, vp_push_none(ctx, &$$)); } ;

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
