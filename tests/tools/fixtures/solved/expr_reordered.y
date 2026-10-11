/* expr.y with its rules in another order: other rule numbers, the same solved conflicts. */
%token N
%left '+'
%left '*'
%%
e: N | e '*' e | e '+' e ;
